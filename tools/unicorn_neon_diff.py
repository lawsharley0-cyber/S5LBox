"""Differential VFPv3/Advanced SIMD test: S5LBox's ARMv7 arm_step() vs Unicorn.

The floating-point and NEON counterpart of tools/unicorn_thumb2_diff.py, and
like it a standalone research script: Unicorn is not a build dependency and
nothing imports this. It drives core/arm_diff_probe in its VFP form (see the
header of tools/arm_diff_probe.c) with arch = ARM_ARCH_V7_A8, and Unicorn's
Cortex-A8 model, from the same core registers, FPSCR, d0-d31, code and RAM,
and compares r0-r15, CPSR, a hash of RAM, FPSCR and d0-d31.

Each case is one instruction drawn from an encoding group, run in ARM state
or in Thumb state (the same instruction in its Thumb-2 encoding). Register
values lean towards the interesting floating-point classes (zeros,
denormals, infinities, NaNs, the smallest normal, integers) and towards
addresses inside RAM for the base registers of loads and stores.

Buckets, as in unicorn_thumb2_diff.py:
  agree        everything matches
  DIFF         both executed and something differs  <- a bug in one of them
  qemu-bug     a case in a form where Unicorn 2.1.4 (QEMU 5.0) is known to
               be wrong; see known_qemu_bug(), which names each one
  nan-payload  the only differences are between two NaNs: the known payload
               deviation vfp.c documents (host NaN propagation order). With
               FPSCR.DN set it cannot happen, and Advanced SIMD always runs
               with DN set.
  both-undef   both refused, or both raised an exception
  we-refuse    S5LBox refused an encoding Unicorn executed (UNPREDICTABLE
               forms refused on purpose, the flush-to-zero boundary case)
  we-fault     S5LBox took an exception where Unicorn executed (alignment,
               which QEMU 5 does not check for these forms)
  we-accept    S5LBox executed an encoding Unicorn refused. Worth reading.
  unmapped     Unicorn touched memory outside its 1 MB; no verdict

Usage:
  pip install unicorn capstone
  cmake --build build --target arm_diff_probe
  python tools/unicorn_neon_diff.py [--count N] [--seed S] [--show N]
                                    [--groups a,b] [--state arm|thumb|both]
"""
import argparse
import collections
import math
import os
import random
import struct
import subprocess
import sys

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_THUMB,
                     UC_ERR_READ_UNMAPPED, UC_ERR_WRITE_UNMAPPED,
                     UC_ERR_FETCH_UNMAPPED)
from unicorn import arm_const as A

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unicorn_thumb2_diff import (PATTERN, PATTERN_BYTES, PROBE_CANDIDATES,  # noqa: E402
                                 RAM_SIZE, REPO, SCR, fnv1a)

try:
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM as CS_ARM, CS_MODE_THUMB as CS_THUMB
    _CS = {False: Cs(CS_ARCH_ARM, CS_ARM), True: Cs(CS_ARCH_ARM, CS_THUMB)}
except ImportError:  # disassembly is only for the report
    _CS = None

ARM_ARCH_V7_A8 = 2
CODE = 0x1000
REGS = [getattr(A, "UC_ARM_REG_R%d" % i) for i in range(15)]
DREGS = [getattr(A, "UC_ARM_REG_D%d" % i) for i in range(32)]
FPSCR_V7_MASK = 0xfff7009f


def disasm(code, thumb):
    if _CS is None:
        return code.hex()
    out = ["%s %s" % (i.mnemonic, i.op_str) for i in _CS[thumb].disasm(code, CODE)]
    return "; ".join(out) if out else "(capstone: none) " + code.hex()


# --- values ----------------------------------------------------------------

def rand_f32(rng):
    k = rng.randrange(12)
    sign = rng.getrandbits(1) << 31
    if k == 0:
        return sign
    if k == 1:
        return sign | rng.randrange(1, 1 << 23)                   # denormal
    if k == 2:
        return sign | 0x00800000 | rng.choice([0, 1, rng.getrandbits(23)])
    if k == 3:
        return sign | 0x7f800000                                  # infinity
    if k == 4:
        return sign | 0x7fc00000 | rng.getrandbits(22)            # quiet NaN
    if k == 5:
        return sign | 0x7f800000 | rng.randrange(1, 1 << 22)      # signalling
    if k == 6:
        return struct.unpack("<I", struct.pack("<f", float(rng.randrange(-70000, 70000))))[0]
    if k == 7:
        return struct.unpack("<I", struct.pack("<f", rng.randrange(-1 << 20, 1 << 20) / 256.0))[0]
    if k == 8:
        return rng.getrandbits(32)
    return sign | (rng.randrange(100, 155) << 23) | rng.getrandbits(23)


def rand_f64(rng):
    k = rng.randrange(12)
    sign = rng.getrandbits(1) << 63
    if k == 0:
        return sign
    if k == 1:
        return sign | rng.randrange(1, 1 << 52)
    if k == 2:
        return sign | (1 << 52) | rng.choice([0, 1, rng.getrandbits(52)])
    if k == 3:
        return sign | (0x7ff << 52)
    if k == 4:
        return sign | (0xfff << 51) | rng.getrandbits(51)
    if k == 5:
        return sign | (0x7ff << 52) | rng.randrange(1, 1 << 51)
    if k == 6:
        return struct.unpack("<Q", struct.pack("<d", float(rng.randrange(-5000000000, 5000000000))))[0]
    if k == 7:
        return struct.unpack("<Q", struct.pack("<d", rng.randrange(-1 << 40, 1 << 40) / 65536.0))[0]
    if k == 8:
        return rng.getrandbits(64)
    if k == 9:  # a double that is exactly a float, so conversions are exact
        f = struct.unpack("<f", struct.pack("<I", rand_f32(rng)))[0]
        return struct.unpack("<Q", struct.pack("<d", f))[0]
    return sign | (rng.randrange(900, 1150) << 52) | rng.getrandbits(52)


def rand_int_lanes(rng):
    esize = rng.choice([8, 16, 32])
    v = 0
    for i in range(64 // esize):
        lane = rng.choice([0, 1, (1 << esize) - 1, 1 << (esize - 1),
                           (1 << (esize - 1)) - 1, rng.getrandbits(esize),
                           rng.getrandbits(3), (1 << esize) - rng.getrandbits(3) - 1])
        v |= (lane & ((1 << esize) - 1)) << (i * esize)
    return v


def rand_dreg(rng):
    k = rng.randrange(5)
    if k == 0:
        return rand_f32(rng) | (rand_f32(rng) << 32)
    if k == 1:
        return rand_f64(rng)
    if k == 2:
        return rand_int_lanes(rng)
    if k == 3:
        return rng.getrandbits(64)
    return rng.choice([0, (1 << 64) - 1, 0x8000000080000000, 0x7fffffff7fffffff])


def rand_fpscr(rng):
    v = rng.getrandbits(32) & 0xff00009f          # flags, QC, AHP, DN, FZ, RMode, cumulative
    if rng.random() < 0.6:
        v &= ~(3 << 22)                           # round to nearest, mostly
    if rng.random() < 0.1:                        # a short vector, stride 1
        # Stride 0b11 (two) is legal too, but Unicorn 2.1.4 is QEMU 5.0,
        # whose translator steps FPSCR.Stride + 1 registers, so 0b11 walks
        # four and every such case would compare against a QEMU bug.
        v |= rng.randrange(1, 4) << 16
    return v & FPSCR_V7_MASK


def rand_core_regs(rng):
    regs = []
    addr_like = rng.random() < 0.6
    for i in range(15):
        if i == 13:
            regs.append(rng.randrange(0x2000, 0xe000) & ~7)
        elif addr_like:
            regs.append(rng.randrange(0x2000, 0xe000) & ~(0 if rng.random() < 0.2 else 7))
        else:
            regs.append(rng.choice([rng.getrandbits(32), rng.getrandbits(8), 0,
                                    0xffffffff, 0x80000000, 0x7fffffff,
                                    rng.getrandbits(16), rand_f32(rng)]))
    return regs


# --- encodings (ARM form; Thumb is derived) --------------------------------

def cond(rng):
    return 0xe if rng.random() < 0.85 else rng.randrange(15)


def g_vfp_dp(rng):
    """CDP on cp10/cp11 with everything but the fixed bits random."""
    r = rng.getrandbits
    return (cond(rng) << 28) | 0x0e000a00 | (r(4) << 20) | (r(4) << 16) \
        | (r(4) << 12) | (r(1) << 8) | (r(3) << 5) | r(4)


def g_vfp_other(rng):
    """The opc1 = 1x11, opc3<0> = 1 group, where conversions live."""
    r = rng.getrandbits
    return (cond(rng) << 28) | 0x0eb00a40 | (r(1) << 22) | (r(4) << 16) \
        | (r(4) << 12) | (r(1) << 8) | (r(1) << 7) | (r(1) << 5) | r(4)


def g_vmov_imm(rng):
    r = rng.getrandbits
    extra = 0xa0 if rng.random() < 0.05 else 0
    return (cond(rng) << 28) | 0x0eb00a00 | (r(1) << 22) | (r(4) << 16) \
        | (r(4) << 12) | (r(1) << 8) | r(4) | (extra & rng.choice([0x80, 0x20]))


def g_vcvt_fixed(rng):
    r = rng.getrandbits
    sx = r(1)
    # imm5 above 16 with a 16-bit value is UNPREDICTABLE; keep most in range.
    imm5 = r(5) if sx or rng.random() < 0.1 else rng.randrange(17)
    return (cond(rng) << 28) | 0x0eba0a40 | (r(1) << 22) | (r(1) << 18) \
        | (r(1) << 16) | (r(4) << 12) | (r(1) << 8) | (sx << 7) \
        | ((imm5 & 1) << 5) | (imm5 >> 1)


def g_vfp_ldst(rng):
    r = rng.getrandbits
    puw = rng.choice([0b010, 0b011, 0b101, 0b100, 0b110, 0b111, 0b000])
    rn = rng.choice([rng.randrange(13), 13, 13, 15])
    dbl = r(1)
    if puw & 0b100 and not puw & 0b001:       # VLDR/VSTR
        imm8 = r(8) if rng.random() < 0.3 else r(4)
    else:
        imm8 = rng.randrange(0, 34)
    return (cond(rng) << 28) | 0x0c000a00 | (((puw >> 2) & 1) << 24) \
        | (((puw >> 1) & 1) << 23) | (r(1) << 22) | ((puw & 1) << 21) \
        | (r(1) << 20) | (rn << 16) | (r(4) << 12) | (dbl << 8) | imm8


def g_vfp_xfer32(rng):
    r = rng.getrandbits
    opc1 = rng.choice([0, 0, 1, 7, 7, r(3)])
    if opc1 == 7:
        vn = rng.choice([0, 1, 1, 1, 6, 7, 8, r(4)])
        cp = 10
    else:
        vn = r(4)
        cp = rng.choice([10, 11])
    rt = rng.choice([r(4), rng.randrange(13)])
    low = 0 if rng.random() < 0.9 else r(2) << 5
    return (cond(rng) << 28) | 0x0e000010 | (opc1 << 21) | (r(1) << 20) \
        | (vn << 16) | (rt << 12) | (cp << 8) | (r(1) << 7) | low


def g_vfp_xfer64(rng):
    r = rng.getrandbits
    return (cond(rng) << 28) | 0x0c400a10 | (r(1) << 20) | (rng.randrange(15) << 16) \
        | (rng.randrange(15) << 12) | (r(1) << 8) | (r(1) << 5) | r(4) \
        | ((r(2) << 6) if rng.random() < 0.05 else 0)


def g_simd_scalar(rng):
    """cp11 MCR/MRC: VMOV to and from a scalar, VDUP from a core register."""
    r = rng.getrandbits
    rt = rng.randrange(15) if rng.random() < 0.95 else 15
    low = 0 if rng.random() < 0.95 else r(4)
    return (cond(rng) << 28) | 0x0e000b10 | (r(3) << 21) | (r(1) << 20) \
        | (r(4) << 16) | (rt << 12) | (r(3) << 5) | low


GROUPS = {
    "vfp dp": g_vfp_dp,
    "vfp convert": g_vfp_other,
    "vmov imm": g_vmov_imm,
    "vcvt fixed": g_vcvt_fixed,
    "vfp ld/st": g_vfp_ldst,
    "vfp xfer32": g_vfp_xfer32,
    "vfp xfer64": g_vfp_xfer64,
    "simd scalar": g_simd_scalar,
}


def thumb_word(w):
    """The Thumb-2 encoding of an ARM coprocessor or Advanced SIMD word, or
    None when there is none (a conditional VFP instruction outside an IT)."""
    if (w >> 25) == 0x79:                         # 1111 001U: SIMD data processing
        u = (w >> 24) & 1
        return 0xef000000 | (u << 28) | (w & 0x00ffffff)
    if (w >> 24) == 0xf4:                         # SIMD element load/store
        return 0xf9000000 | (w & 0x00ffffff)
    if (w >> 28) == 0xe:
        return w
    return None


# --- the two engines -------------------------------------------------------

def new_a8(thumb):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB if thumb else UC_MODE_ARM)
    uc.ctl_set_cpu_model(A.UC_CPU_ARM_CORTEX_A8)
    uc.reg_write(A.UC_ARM_REG_CP_REG, SCR + (0,))
    uc.reg_write(A.UC_ARM_REG_C1_C0_2, 0x00f00000)
    uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
    return uc


def run_unicorn(thumb, regs, cpsr, fpscr, dregs, pc, code):
    uc = new_a8(thumb)
    uc.mem_map(0, RAM_SIZE)
    uc.mem_write(0, PATTERN)
    uc.mem_write(pc, code)
    uc.reg_write(A.UC_ARM_REG_CPSR, cpsr)
    for reg, val in zip(REGS, regs):
        uc.reg_write(reg, val)
    uc.reg_write(A.UC_ARM_REG_FPSCR, fpscr)
    for reg, val in zip(DREGS, dregs):
        uc.reg_write(reg, val)
    try:
        uc.emu_start(pc | (1 if thumb else 0), RAM_SIZE, count=1)
    except UcError as e:
        return None, e
    out = [uc.reg_read(r) for r in REGS]
    out.append(uc.reg_read(A.UC_ARM_REG_R15))
    out.append(uc.reg_read(A.UC_ARM_REG_CPSR))
    out.append(fnv1a(uc.mem_read(0, PATTERN_BYTES)))
    out.append(uc.reg_read(A.UC_ARM_REG_FPSCR))
    for reg in DREGS:
        d = uc.reg_read(reg)
        out += [d & 0xffffffff, d >> 32]
    return out, None


def run_probe(proc, regs, cpsr, fpscr, dregs, pc, code):
    pad = -len(code) % 4
    code = code + PATTERN[pc + len(code):pc + len(code) + pad]
    words = struct.unpack("<%dI" % (len(code) // 4), code)
    line = "v " + " ".join("%x" % v for v in regs)
    line += " %x %x %x %x %x" % (cpsr, pc, words[0], ARM_ARCH_V7_A8, 1)
    line += " %x" % fpscr
    for d in dregs:
        line += " %x %x" % (d & 0xffffffff, d >> 32)
    line += "".join(" %x" % w for w in words[1:]) + "\n"
    proc.stdin.write(line)
    proc.stdin.flush()
    reply = proc.stdout.readline().split()
    if not reply or reply[0] == "ERR":
        raise RuntimeError("probe returned %r for %r" % (reply, line))
    return [int(x, 16) for x in reply[1:]], int(reply[0])


def known_qemu_bug(w, thumb, fpscr):
    """A reason when this case exercises a known Unicorn 2.1.4 defect, else
    None. Each was isolated with a directed case before being listed here."""
    arm = w if not thumb else w  # VFP words are the same in both states
    if (fpscr >> 16) & 7 and (arm & 0x0fb00e50) == 0x0eb00a40 \
            and (arm >> 16) & 0xf in (0, 1) and arm & 0x100:
        # Double-precision VMOV/VABS/VNEG/VSQRT as a short vector: QEMU 5.0's
        # two-operand double path writes element 1 to Dm+1 from Dm (vadd.f64
        # and the single-precision forms step correctly).
        return "dp two-operand short vector"
    return None


def took_exception(vals, cpsr_in):
    return vals[15] < 0x20 and (vals[16] & 0x1f) != (cpsr_in & 0x1f)


# Index of d0's low word in a result: r0-r14, pc, cpsr, ram hash, fpscr.
FP0 = 19


def is_nan32(w):
    return (w & 0x7f800000) == 0x7f800000 and (w & 0x7fffff) != 0


def nan_payload_only(u, p):
    """Every difference is in FP registers, and each differing word pair (or
    double) is two NaNs."""
    if u[:FP0] != p[:FP0]:
        return False
    for i in range(32):
        uw, pw = u[FP0 + 2 * i:FP0 + 2 + 2 * i], p[FP0 + 2 * i:FP0 + 2 + 2 * i]
        if uw == pw:
            continue
        ud, pd = uw[0] | (uw[1] << 32), pw[0] | (pw[1] << 32)
        both64 = all((d >> 52) & 0x7ff == 0x7ff and d & ((1 << 52) - 1) for d in (ud, pd))
        if both64:
            continue
        for a, b in zip(uw, pw):
            if a != b and not (is_nan32(a) and is_nan32(b)):
                return False
    return True


LABELS = (["r%d" % i for i in range(15)] + ["pc", "cpsr", "ram", "fpscr"]
          + ["s%d/d%d%s" % (2 * i + h, i, ".hi" if h else ".lo")
             for i in range(32) for h in (0, 1)])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=2000, help="cases per group and state")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--show", type=int, default=4)
    ap.add_argument("--groups", default=",".join(GROUPS))
    ap.add_argument("--state", choices=["arm", "thumb", "both"], default="both")
    ap.add_argument("--verbose", action="store_true",
                    help="print every input D register of a DIFF")
    args = ap.parse_args()

    built = [p for p in PROBE_CANDIDATES if os.path.exists(p)]
    if not built:
        sys.exit("arm_diff_probe not built -- run: "
                 "cmake --build build --target arm_diff_probe")
    probe = max(built, key=os.path.getmtime)
    print("probe: %s" % os.path.relpath(probe, REPO))
    proc = subprocess.Popen([probe], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, bufsize=1)
    rng = random.Random(args.seed)
    tally = collections.defaultdict(collections.Counter)
    shown = collections.Counter()
    states = {"arm": [False], "thumb": [True], "both": [False, True]}[args.state]
    groups = [g for g in args.groups.split(",") if g]

    rows = []
    for group in groups:
        gen = GROUPS[group]
        for thumb in states:
            row = "%s %s" % (group, "T" if thumb else "A")
            rows.append(row)
            done = 0
            while done < args.count:
                w = gen(rng)
                if thumb:
                    w = thumb_word(w)
                    if w is None:
                        continue
                    code = struct.pack("<HH", w >> 16, w & 0xffff)
                else:
                    code = struct.pack("<I", w)
                done += 1
                regs = rand_core_regs(rng)
                mode = 0x1f if rng.random() < 0.85 else 0x10
                cpsr = (rng.getrandbits(5) << 27) | (rng.getrandbits(4) << 16) \
                    | 0xc0 | mode | (0x20 if thumb else 0)
                fpscr = rand_fpscr(rng)
                dregs = [rand_dreg(rng) for _ in range(32)]
                pc = CODE + 4 * rng.randrange(0, 0x100)

                u_out, u_err = run_unicorn(thumb, regs, cpsr, fpscr, dregs, pc, code)
                p_out, p_status = run_probe(proc, regs, cpsr, fpscr, dregs, pc, code)
                p_ok = p_status == 0 and not took_exception(p_out, cpsr)

                unmapped = u_err is not None and u_err.errno in (
                    UC_ERR_READ_UNMAPPED, UC_ERR_WRITE_UNMAPPED, UC_ERR_FETCH_UNMAPPED)
                if unmapped:
                    bucket = "unmapped"
                elif u_err is not None and not p_ok:
                    bucket = "both-undef"
                elif u_err is not None:
                    bucket = "we-accept"
                elif p_status != 0:
                    bucket = "we-refuse"
                elif not p_ok:
                    bucket = "we-fault"
                elif u_out == p_out:
                    bucket = "agree"
                elif known_qemu_bug(w, thumb, fpscr):
                    bucket = "qemu-bug"
                elif nan_payload_only(u_out, p_out):
                    bucket = "nan-payload"
                else:
                    bucket = "DIFF"
                tally[row][bucket] += 1
                if bucket in ("agree", "both-undef", "unmapped", "qemu-bug"):
                    continue
                key = (row, bucket)
                if shown[key] >= args.show:
                    continue
                shown[key] += 1
                print("%-10s %-16s %08x  [%s]" % (bucket, row, w, disasm(code, thumb)))
                if bucket == "we-accept":
                    print("           unicorn: %s" % u_err)
                if bucket == "we-refuse":
                    print("           s5lbox status %d" % p_status)
                if bucket in ("DIFF", "nan-payload"):
                    print("           cpsr_in=%08x fpscr_in=%08x" % (cpsr, fpscr))
                    if args.verbose:
                        print("           regs_in=%s" % " ".join("%x" % r for r in regs))
                        for i in range(0, 32, 4):
                            print("           " + "  ".join("d%-2d=%016x" % (i + k, dregs[i + k])
                                                          for k in range(4)))
                    for label, uv, pv in zip(LABELS, u_out, p_out):
                        if uv != pv:
                            print("           %-10s unicorn=%08x s5lbox=%08x" % (label, uv, pv))

    proc.stdin.close()
    proc.wait(timeout=5)

    print()
    cols = ["agree", "DIFF", "qemu-bug", "nan-payload", "both-undef", "we-refuse",
            "we-fault", "we-accept", "unmapped"]
    print("%-18s" % "group" + "".join("%12s" % c for c in cols))
    total = collections.Counter()
    for row in rows:
        print("%-18s" % row + "".join("%12d" % tally[row][c] for c in cols))
        total.update(tally[row])
    print("%-18s" % "total" + "".join("%12d" % total[c] for c in cols))
    sys.exit(1 if total["DIFF"] else 0)


if __name__ == "__main__":
    main()
