"""Differential Thumb-2 test: S5LBox's ARMv7 arm_step() vs Unicorn's Cortex-A8.

The ARMv7 counterpart of tools/unicorn_diff.py, and like it a standalone
research script: Unicorn is not a build dependency and nothing imports this.
It drives core/arm_diff_probe in its extended form (see the header of
tools/arm_diff_probe.c) with arch = ARM_ARCH_V7_A8, and Unicorn with the
Cortex-A8 CPU model, from the same registers, flags, code and RAM, and
compares r0-r15, CPSR and a hash of RAM.

Cases are random encodings drawn from each Thumb-2 encoding group, random
16-bit encodings, and IT blocks (an IT followed by the instructions it
covers, run as one sequence). Registers are either random or small
addresses inside mapped RAM, so both the data paths and the address paths
get exercised.

Each case lands in one of five buckets:
  agree      both executed and every register, CPSR and RAM match
  DIFF       both executed and something differs  <- a bug in one of them
  both-undef both refused (or both raised an exception)
  we-refuse  S5LBox refused an encoding Unicorn executed. Expected for the
             UNPREDICTABLE forms this core refuses on purpose; each one is
             listed so it can be checked against that rule.
  we-fault   S5LBox took an exception (an alignment fault, say) where
             Unicorn executed. Unicorn 2 is QEMU 5, which does not enforce
             ARMv7's word alignment for LDM/STM/LDRD/STRD/LDREX; S5LBox does.
  we-accept  S5LBox executed an encoding Unicorn refused. Worth reading.
  unmapped   Unicorn touched memory outside its 1 MB (the probe wraps); no
             verdict.

Usage:
  pip install unicorn capstone
  cmake --build build --target arm_diff_probe
  python tools/unicorn_thumb2_diff.py [--count N] [--seed S] [--show N]
"""
import argparse
import collections
import os
import random
import struct
import subprocess
import sys

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB,
                     UC_ERR_INSN_INVALID, UC_ERR_READ_UNMAPPED,
                     UC_ERR_WRITE_UNMAPPED, UC_ERR_FETCH_UNMAPPED)
from unicorn import arm_const as A

try:
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_V8
    _CS = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
except ImportError:  # disassembly is only for the report
    _CS = None

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROBE_CANDIDATES = [
    os.path.join(REPO, "build", d, "core", "arm_diff_probe" + ext)
    for d in ("", "strict", "ninja-release", "ninja-debug")
    for ext in (".exe", "")
]

ARM_ARCH_V7_A8 = 2
RAM_SIZE = 1 << 20
PATTERN_BYTES = 0x10000
CODE = 0x1000
REGS = [getattr(A, "UC_ARM_REG_R%d" % i) for i in range(15)]


def pattern():
    """tools/arm_diff_probe.c's pattern_byte(), for the same initial RAM."""
    return bytes(((i * 167 + 13) ^ (i >> 8)) & 0xff for i in range(PATTERN_BYTES))


PATTERN = pattern()


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xffffffff
    return h


def disasm(code):
    if _CS is None:
        return code.hex()
    out = ["%s %s" % (i.mnemonic, i.op_str) for i in _CS.disasm(code, CODE)]
    return "; ".join(out) if out else "(capstone: none) " + code.hex()


# --- case generation -------------------------------------------------------

def t32(hw1, hw2):
    return struct.pack("<HH", hw1, hw2)


def rand_t32(rng, group):
    """A random 32-bit Thumb encoding from one encoding group."""
    r = rng.getrandbits
    if group == "ldm/stm":
        hw1 = 0xe800 | (rng.choice([1, 2]) << 7) | (r(1) << 5) | (r(1) << 4) | r(4)
        return hw1, r(16) & ~0x2000
    if group == "ld/st dual,excl,tb":
        return 0xe840 | (r(2) << 7) | (r(1) << 5) | (r(1) << 4) | r(4), r(16)
    if group == "dp shifted reg":
        return 0xea00 | r(9), r(16) & 0x7fff
    if group == "dp modified imm":
        return 0xf000 | (r(1) << 10) | r(9), r(16) & 0x7fff
    if group == "dp plain imm":
        return 0xf200 | (r(1) << 10) | r(9), r(16) & 0x7fff
    if group == "branch":
        # B.W, BL, BLX and B<c>.W with S = 0 and J1 = J2 = 1 (so I1 = I2 = 0)
        # and a small imm10/imm6, which keeps every target inside RAM.
        kind = rng.randrange(4)
        if kind == 3:                                   # B<c>.W
            cond = rng.randrange(14)
            return 0xf000 | (cond << 6) | r(3), 0x8000 | r(11)
        op1 = (1, 4, 5)[kind]                           # B.W, BLX, BL
        return 0xf000 | r(6), 0x8000 | (op1 << 12) | 0x2800 | r(11)
    if group == "system":
        # Well-formed MRS, MSR, CPS, hints, CLREX and barriers: a random
        # encoding in this space almost always breaks a fixed bit, and both
        # sides refuse it, which tests nothing.
        kind = rng.randrange(5)
        if kind == 0:                                   # MRS APSR / SPSR
            return 0xf3ef | (r(1) << 4), 0x8000 | (rng.randrange(13) << 8)
        if kind == 1:                                   # MSR, any mask
            return 0xf380 | (r(1) << 4) | rng.randrange(13), 0x8000 | (r(4) << 8)
        if kind == 2:                                   # CPS
            return 0xf3af, 0x8000 | (rng.choice([2, 3]) << 9) | (r(1) << 8) \
                | (r(3) << 5) | rng.choice([0x10, 0x11, 0x12, 0x13, 0x17,
                                            0x1b, 0x1f])
        if kind == 3:                                   # hints NOP..SEV
            return 0xf3af, 0x8000 | rng.choice([0, 4])
        return 0xf3bf, 0x8f00 | (rng.choice([2, 4, 5, 6]) << 4) | r(4)
    if group == "ld/st single":
        return 0xf800 | r(9), r(16)
    if group == "dp register":
        return 0xfa00 | r(8), 0xf000 | r(12)
    if group == "multiply":
        return 0xfb00 | r(7), r(16)
    if group == "multiply long":
        return 0xfb80 | r(7), r(16)
    raise ValueError(group)


GROUPS_32 = ["ldm/stm", "ld/st dual,excl,tb", "dp shifted reg",
             "dp modified imm", "dp plain imm", "branch", "system",
             "ld/st single", "dp register", "multiply", "multiply long"]


def rand_t16(rng):
    """A random 16-bit encoding, less SVC/BKPT/UDF (they trap by design) and
    IT, which Unicorn cannot single-step (its count runs the IT and the next
    instruction as one); the IT-block group covers IT."""
    while True:
        h = rng.getrandbits(16)
        if h >= 0xe800:
            continue
        if (h & 0xff00) in (0xde00, 0xdf00, 0xbe00):
            continue
        if (h & 0xff00) == 0xbf00 and (h & 0xf):
            continue
        return h


def valid_alone(code):
    """Unicorn decodes it. An IT body must be valid on its own: whether an
    UNDEFINED encoding whose condition fails still traps is IMPLEMENTATION
    DEFINED, so a body that tests that would compare two legal answers."""
    uc = new_cortex_a8()
    uc.mem_map(0, RAM_SIZE)
    uc.mem_write(CODE, code)
    uc.reg_write(A.UC_ARM_REG_CPSR, 0xff)
    for reg in REGS:
        uc.reg_write(reg, 0x8000)
    try:
        uc.emu_start(CODE | 1, CODE + len(code), count=1)
    except UcError as e:
        return e.errno != UC_ERR_INSN_INVALID
    return True


def rand_straight_line(rng):
    """One instruction that does not branch, for the body of an IT block."""
    while True:
        code = _rand_straight_line(rng)
        if valid_alone(code):
            return code


def _rand_straight_line(rng):
    while True:
        if rng.random() < 0.5:
            h = rand_t16(rng)
            top = h >> 12
            if top in (0xd, 0xe) or (h & 0xff87) == 0x4487 or (h & 0xfe00) == 0xbc00 \
               or (h & 0xf500) == 0xb100 or (h & 0xff00) == 0xbf00 \
               or (h & 0xff78) == 0x4478 or (h & 0xff78) == 0x4678 \
               or (h & 0xff00) == 0x4700:
                continue
            return struct.pack("<H", h)
        g = rng.choice(["dp shifted reg", "dp modified imm", "dp plain imm",
                        "dp register", "multiply", "multiply long"])
        hw1, hw2 = rand_t32(rng, g)
        if (hw2 >> 8) & 0xf == 15 and g != "multiply long":
            continue  # Rd = PC: a branch or a test, keep the body simple
        return t32(hw1, hw2)


def rand_it(rng):
    first = rng.randrange(0, 14)
    n = rng.randrange(1, 5)
    # mask: for each later instruction a then/else bit, then a terminating 1
    bits = [rng.getrandbits(1) for _ in range(n - 1)]
    mask = 0
    for i, b in enumerate(bits):
        mask |= ((first & 1) ^ (0 if b else 1)) << (3 - i)
    mask |= 1 << (4 - n)
    it = 0xbf00 | (first << 4) | mask
    body = b"".join(rand_straight_line(rng) for _ in range(n))
    return struct.pack("<H", it) + body, 1 + n


def rand_regs(rng):
    regs = []
    addr_like = rng.random() < 0.6
    for i in range(15):
        if i == 13:                     # SP stays word aligned, as ABIs keep it
            regs.append(rng.randrange(0x2000, 0xe000) & ~3)
        elif addr_like:
            regs.append(rng.randrange(0x2000, 0xe000) & ~(0 if rng.random() < 0.3 else 3))
        else:
            regs.append(rng.choice([rng.getrandbits(32), rng.getrandbits(8),
                                    0, 0xffffffff, 0x80000000, 0x7fffffff,
                                    rng.getrandbits(16)]))
    return regs


# --- the two engines -------------------------------------------------------

SCR = (15, 0, 0, 1, 1, 0, 0)   # CP15 c1, c1, 0 as UC_ARM_REG_CP_REG names it


def new_cortex_a8():
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    uc.ctl_set_cpu_model(A.UC_CPU_ARM_CORTEX_A8)
    # Unicorn starts the A8 in the non-secure world (SCR.NS = 1), where QEMU
    # ignores writes to CPSR.A and .F unless SCR.AW/FW allow them. S5LBox has
    # no security extensions, which behaves as the secure world does.
    uc.reg_write(A.UC_ARM_REG_CP_REG, SCR + (0,))
    return uc


def run_unicorn(regs, cpsr, pc, code, steps):
    uc = new_cortex_a8()
    uc.mem_map(0, RAM_SIZE)
    uc.mem_write(0, PATTERN)
    uc.mem_write(pc, code)
    uc.reg_write(A.UC_ARM_REG_CPSR, cpsr)
    for reg, val in zip(REGS, regs):
        uc.reg_write(reg, val)
    try:
        if steps == 1:
            uc.emu_start(pc | 1, RAM_SIZE, count=1)
        else:
            # An IT block is straight-line code: run it to its end. Unicorn's
            # count skips the instructions whose condition fails, so a count
            # would run on past the block.
            uc.emu_start(pc | 1, pc + len(code))
    except UcError as e:
        return None, e
    out = [uc.reg_read(r) for r in REGS]
    out.append(uc.reg_read(A.UC_ARM_REG_R15))
    out.append(uc.reg_read(A.UC_ARM_REG_CPSR))
    out.append(fnv1a(uc.mem_read(0, PATTERN_BYTES)))
    return out, None


def run_probe(proc, regs, cpsr, pc, code, steps):
    # The probe stores whole words, so pad with the bytes RAM already holds.
    pad = -len(code) % 4
    code = code + PATTERN[pc + len(code):pc + len(code) + pad]
    words = struct.unpack("<%dI" % (len(code) // 4), code)
    line = " ".join("%x" % v for v in regs)
    line += " %x %x %x %x %x" % (cpsr, pc, words[0], ARM_ARCH_V7_A8, steps)
    line += "".join(" %x" % w for w in words[1:]) + "\n"
    proc.stdin.write(line)
    proc.stdin.flush()
    reply = proc.stdout.readline().split()
    if not reply or reply[0] == "ERR":
        raise RuntimeError("probe returned %r for %r" % (reply, line))
    status = int(reply[0])
    vals = [int(x, 16) for x in reply[1:]]
    return vals, status


def took_exception(vals, cpsr_in):
    """The probe ran an exception entry: PC in the vector table, mode changed."""
    return vals[15] < 0x20 and (vals[16] & 0x1f) != (cpsr_in & 0x1f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=2000, help="cases per group")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--show", type=int, default=4,
                    help="examples to print per group and bucket")
    args = ap.parse_args()

    # The newest build wins: an old build directory's probe predates the
    # extended protocol and would test stale code.
    built = [p for p in PROBE_CANDIDATES if os.path.exists(p)]
    if not built:
        sys.exit("arm_diff_probe not built -- run: "
                 "cmake --build build --target arm_diff_probe")
    probe = max(built, key=os.path.getmtime)
    print("probe: %s" % os.path.relpath(probe, REPO))
    proc = subprocess.Popen([probe], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, text=True, bufsize=1)
    rng = random.Random(args.seed)
    tally = collections.defaultdict(collections.Counter)
    shown = collections.Counter()
    labels = ["r%d" % i for i in range(15)] + ["pc", "cpsr", "ram"]

    groups = GROUPS_32 + ["16-bit", "CBZ/hints", "IT block"]
    for group in groups:
        for _ in range(args.count):
            if group == "16-bit":
                code, steps = struct.pack("<H", rand_t16(rng)), 1
            elif group == "CBZ/hints":
                h = rng.choice([0xb100 | (rng.getrandbits(1) << 11)
                                | (rng.getrandbits(1) << 9) | rng.getrandbits(8),
                                0xbf00 | rng.choice([0x00, 0x40, 0x50, 0x60])])
                code, steps = struct.pack("<H", h), 1
            elif group == "IT block":
                code, steps = rand_it(rng)
            else:
                code, steps = t32(*rand_t32(rng, group)), 1
            regs = rand_regs(rng)
            flags = (rng.getrandbits(5) << 27) | (rng.getrandbits(4) << 16)
            cpsr = flags | 0xff            # SYS mode, IRQ/FIQ masked, Thumb
            pc = CODE + 2 * rng.randrange(0, 0x200)

            u_out, u_err = run_unicorn(regs, cpsr, pc, code, steps)
            p_out, p_status = run_probe(proc, regs, cpsr, pc, code, steps)
            p_ok = p_status == 0 and not took_exception(p_out, cpsr)

            unmapped = u_err is not None and u_err.errno in (
                UC_ERR_READ_UNMAPPED, UC_ERR_WRITE_UNMAPPED, UC_ERR_FETCH_UNMAPPED)
            if unmapped:
                bucket = "unmapped"     # outside Unicorn's RAM; the probe wraps
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
            else:
                bucket = "DIFF"
            tally[group][bucket] += 1
            if bucket in ("agree", "both-undef", "unmapped"):
                continue
            key = (group, bucket)
            if shown[key] >= args.show:
                continue
            shown[key] += 1
            print("%-9s %-18s %s  [%s]" % (bucket, group, code.hex(), disasm(code)))
            if bucket == "we-accept":
                print("          unicorn: %s" % u_err)
            if bucket == "we-refuse":
                print("          s5lbox status %d" % p_status)
            if bucket == "DIFF":
                print("          cpsr_in=%08x regs_in=%s"
                      % (cpsr, " ".join("%x" % r for r in regs)))
                for label, uv, pv in zip(labels, u_out, p_out):
                    if uv != pv:
                        print("          %-4s unicorn=%08x s5lbox=%08x" % (label, uv, pv))

    proc.stdin.close()
    proc.wait(timeout=5)

    print()
    cols = ["agree", "DIFF", "both-undef", "we-refuse", "we-fault",
            "we-accept", "unmapped"]
    print("%-20s" % "group" + "".join("%11s" % c for c in cols))
    total = collections.Counter()
    for group in groups:
        print("%-20s" % group + "".join("%11d" % tally[group][c] for c in cols))
        total.update(tally[group])
    print("%-20s" % "total" + "".join("%11d" % total[c] for c in cols))
    sys.exit(1 if total["DIFF"] else 0)


if __name__ == "__main__":
    main()
