"""Differential ARMv7 page-table test: S5LBox's Cortex-A8 MMU vs Unicorn's.

The translation-table counterpart of tools/unicorn_thumb2_diff.py, and like
it a standalone research script: Unicorn is not a build dependency and
nothing imports this. It drives core/arm_diff_probe in its MMU form (see the
header of tools/arm_diff_probe.c) with arch = ARM_ARCH_V7_A8.

Each case builds a random short-descriptor translation for one virtual
address, in RAM that both sides share: a first-level entry that is a fault,
the reserved type 3, a section, a supersection or a page table, and in the
last case a second-level entry that is a fault, a large page or a small page,
with random access permissions (AP, APX), domain, XN, memory attributes and
the bits that should not matter. TTBCR.N, TTBR0/TTBR1, DACR and SCTLR (AFE,
TRE and some read-as-zero bits) are random too. VA 0-1 MB is an identity
section in domain 0, so the code and the exception vectors stay mapped.

The code, in ARM state and SVC mode, then turns the MMU on through MCR, runs
all four ATS1C* operations on the address (privileged/user x read/write),
reading PAR after each, reads TTBCR back, and finally makes one real access
to the address (LDR, STR, LDRT or STRT), which may abort.

Compared: r0-r12, the four PAR values and TTBCR among them, and whether the
access aborted. Without an abort, also r13-r15, CPSR and a hash of the first
64 KB of RAM. Unicorn does not take a data abort: it reports it to an
interrupt hook (exception 4) before any fault register is written, at the
faulting instruction. So when both abort, the fault itself is checked
against Unicorn's own answer for that access kind, the ATS PAR it produced:
DFSR's status must be PAR's, DFSR.WnR must say whether it was a store, and
DFAR must be the address.

The SCTLR value written always has the read-as-one bits set (XP and U among
them), and is not compared on read back. Unicorn stores SCTLR as written, and
with XP clear it walks the ARMv5 table format (subpage AP bits, no APX, no
XN), which ARMv7 does not have: XP is read-as-one there. This emulator
enforces the read-as-one and read-as-zero bits (test_armv7.c checks that);
Unicorn is not a reference for it.

Buckets:
  agree      everything matches
  DIFF       something differs                       <- a bug in one of them
  qemu-bug   a difference known_qemu_bug() names: a place where Unicorn 2.1.4
             (QEMU 5.0) departs from the ARMv7-A ARM
  unmapped   the access reached physical memory outside Unicorn's 1 MB (the
             probe wraps); everything before it, the PARs included, is still
             compared

PAR's NS bit (9) is not compared: Unicorn's Cortex-A8 has no EL3, so QEMU
treats every access as Non-secure and sets it; this emulator models no
Security Extensions and leaves the bit zero, the architecture's value when
they are absent.

Usage:
  pip install unicorn
  cmake --build build --target arm_diff_probe
  python tools/unicorn_vmsa_diff.py [--count N] [--seed S] [--show N]
"""
import argparse
import collections
import os
import random
import struct
import subprocess
import sys

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_INTR,
                     UC_ERR_READ_UNMAPPED, UC_ERR_WRITE_UNMAPPED,
                     UC_ERR_FETCH_UNMAPPED)
from unicorn import arm_const as A

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unicorn_thumb2_diff import (PATTERN, PATTERN_BYTES, PROBE_CANDIDATES,  # noqa: E402
                                 RAM_SIZE, REPO, fnv1a)

ARM_ARCH_V7_A8 = 2
CODE = 0x1000
L1_TTBR0 = 0x40000          # 16 KB aligned, so valid for every TTBCR.N
L1_TTBR1 = 0x44000
L2_TABLE = 0x48000
REGS = [getattr(A, "UC_ARM_REG_R%d" % i) for i in range(15)]
PAR_NS = 1 << 9
SCTLR_RAO = 0x00c50078
EXCP_DATA_ABORT = 4


def mcr(opc1, crn, crm, opc2, rt):
    return 0xee000f10 | (opc1 << 21) | (crn << 16) | (rt << 12) | (opc2 << 5) | crm


def mrc(opc1, crn, crm, opc2, rt):
    return mcr(opc1, crn, crm, opc2, rt) | (1 << 20)


ACCESSES = {
    "ldr":  0xe596c000,     # ldr  r12, [r6]
    "str":  0xe5860000,     # str  r0, [r6]
    "ldrt": 0xe4b6c000,     # ldrt r12, [r6], #0
    "strt": 0xe4a60000,     # strt r0, [r6], #0
}


def program(access):
    return [
        mcr(0, 2, 0, 0, 1), mcr(0, 2, 0, 1, 2), mcr(0, 2, 0, 2, 3),
        mcr(0, 3, 0, 0, 4), mcr(0, 1, 0, 0, 5),
        mcr(0, 7, 8, 0, 6), mrc(0, 7, 4, 0, 7),
        mcr(0, 7, 8, 1, 6), mrc(0, 7, 4, 0, 8),
        mcr(0, 7, 8, 2, 6), mrc(0, 7, 4, 0, 9),
        mcr(0, 7, 8, 3, 6), mrc(0, 7, 4, 0, 10),
        mrc(0, 2, 0, 2, 11),
        ACCESSES[access],
    ]


# --- case generation -------------------------------------------------------

def rand_base(rng, bits_low):
    """A page or section base: half the time inside the 1 MB both sides map,
    so a successful access can be compared, and half anywhere."""
    if rng.random() < 0.5:
        base = rng.randrange(0x60000, RAM_SIZE)
    else:
        base = rng.getrandbits(32)
    return base & ~((1 << bits_low) - 1) & 0xffffffff


def rand_case(rng):
    r = rng.getrandbits
    n = rng.randrange(8)
    va = rng.getrandbits(32)
    while (va >> 20) == 0:
        va = rng.getrandbits(32)
    if rng.random() < 0.8:
        va &= ~3
    use_ttbr0 = n == 0 or (va >> (32 - n)) == 0
    pokes = {}
    # The identity section for code and vectors: domain 0, AP = 01 (with
    # AFE, AF set and privileged read/write), XN clear.
    pokes[L1_TTBR0] = 0x00000402
    l1_addr = (L1_TTBR0 + (((va >> 20) & ((1 << (12 - n)) - 1)) << 2)) if use_ttbr0 \
        else L1_TTBR1 + ((va >> 20) << 2)

    kind = rng.choices(["fault", "type3", "section", "super", "table"],
                       [10, 5, 35, 15, 35])[0]
    detail = {"kind": kind}
    if kind == "fault":
        l1 = r(32) & ~3
    elif kind == "type3":
        l1 = r(32) | 3
    elif kind == "section":
        l1 = (rand_base(rng, 20) | (r(20) & ~0x40003)) | 2        # bit 18 clear
    elif kind == "super":
        l1 = (rand_base(rng, 24) | 0x40002 |
              (r(18) & 0x3fffc & ~0x00f00000 & ~0x1e0))            # no ext PA
    else:
        table = L2_TABLE + rng.randrange(16) * 0x400
        l1 = table | (r(10) & 0x3fc & ~0x10) | 1
        l2_addr = table + (((va >> 12) & 0xff) << 2)
        l2kind = rng.choices(["fault", "large", "small"], [15, 25, 60])[0]
        detail["l2"] = l2kind
        if l2kind == "fault":
            l2 = r(32) & ~3
        elif l2kind == "large":
            l2 = rand_base(rng, 16) | (r(16) & 0xfffc) | 1
        else:
            l2 = rand_base(rng, 12) | (r(12) & 0xffc) | 2 | r(1)
        pokes[l2_addr] = l2
        detail["l2d"] = l2
    if l1_addr != L1_TTBR0:
        pokes[l1_addr] = l1
    detail["l1d"] = l1

    dacr = r(32)
    dacr = (dacr & ~3) | rng.choice([1, 3])                         # code domain
    sctlr = 1 | SCTLR_RAO
    if rng.random() < 0.3:
        sctlr |= 1 << 29                                            # AFE
    if rng.random() < 0.3:
        sctlr |= 1 << 28                                            # TRE
    if rng.random() < 0.3:
        sctlr |= rng.choice([1 << 8, 1 << 9, 3 << 8])               # S, R
    # PD1 disables TTBR1 walks. PD0 is not drawn: it would unmap the code.
    ttbcr = n | (rng.choice([0x20, 0x80000000]) if rng.random() < 0.3 else 0)
    regs = [r(32) for _ in range(15)]
    regs[1] = L1_TTBR0 | r(7)
    regs[2] = L1_TTBR1 | r(7)
    regs[3] = ttbcr
    regs[4] = dacr
    regs[5] = sctlr
    regs[6] = va
    regs[13] = 0x80000
    # The domain the walk checks, and AP[0] (the access flag under AFE).
    if kind in ("section", "table"):
        domain = (l1 >> 5) & 0xf
    else:
        domain = 0
    af = (l1 >> 10) & 1 if kind in ("section", "super") else \
        ((detail["l2d"] >> 4) & 1 if kind == "table" else None)
    detail.update(n=n, va=va, dacr=dacr, sctlr=sctlr, use_ttbr0=use_ttbr0,
                  dac=(dacr >> (2 * domain)) & 3, af=af,
                  pd1=bool(ttbcr & 0x20) and not use_ttbr0)
    access = rng.choice(sorted(ACCESSES))
    return regs, sorted(pokes.items()), access, detail


# --- the two sides ---------------------------------------------------------

def run_unicorn(regs, pokes, code):
    """Registers, RAM hash, and the exception number Unicorn stopped on (or
    None). It stops at the faulting instruction without vectoring."""
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.ctl_set_cpu_model(A.UC_CPU_ARM_CORTEX_A8)
    uc.mem_map(0, RAM_SIZE)
    uc.mem_write(0, PATTERN)
    for addr, word in pokes:
        uc.mem_write(addr, struct.pack("<I", word))
    blob = b"".join(struct.pack("<I", w) for w in code)
    uc.mem_write(CODE, blob)
    uc.reg_write(A.UC_ARM_REG_CPSR, 0x1d3)                          # SVC, A I F
    for i, v in enumerate(regs):
        uc.reg_write(REGS[i], v)
    excp = []

    def on_exception(u, intno, _):
        excp.append(intno)
        u.emu_stop()
    uc.hook_add(UC_HOOK_INTR, on_exception)
    err = None
    try:
        uc.emu_start(CODE, CODE + len(blob), count=len(code))
    except UcError as e:
        err = e
    out = [uc.reg_read(REGS[i]) for i in range(15)]
    out += [uc.reg_read(A.UC_ARM_REG_PC), uc.reg_read(A.UC_ARM_REG_CPSR)]
    out.append(fnv1a(bytes(uc.mem_read(0, PATTERN_BYTES))))
    return out, err, (excp[0] if excp else None)


def run_probe(proc, regs, pokes, code):
    toks = ["m", str(len(pokes))]
    for addr, word in pokes:
        toks += ["%x" % addr, "%x" % word]
    toks += ["%x" % v for v in regs]
    toks += ["1d3", "%x" % CODE, "%x" % code[0], "%x" % ARM_ARCH_V7_A8,
             "%x" % len(code)]
    toks += ["%x" % w for w in code[1:]]
    proc.stdin.write(" ".join(toks) + "\n")
    proc.stdin.flush()
    reply = proc.stdout.readline().split()
    if len(reply) != 1 + 17 + 1 + 5:
        raise RuntimeError("probe returned %r" % reply)
    status = int(reply[0])
    vals = [int(x, 16) for x in reply[1:]]
    return status, vals[:18] + vals[18:20]                           # dfsr, dfar


NAMES = ["r%d" % i for i in range(15)] + ["pc", "cpsr", "ram", "dfsr", "dfar"]
PAR_REG = {"ldr": 7, "str": 8, "ldrt": 9, "strt": 10}   # the matching ATS


def compare(p, u, excp, access, va):
    """Names of what differs; see the header for what is compared when."""
    bad = []
    for i in range(13):
        a, b = p[i], u[i]
        if i in PAR_REG.values():
            a, b = a & ~PAR_NS, b & ~PAR_NS
        if a != b:
            bad.append(NAMES[i])
    ours = p[15] == 0x10 and (p[16] & 0x1f) == 0x17
    theirs = excp == EXCP_DATA_ABORT
    if ours != theirs:
        return bad + ["abort (ours %s, unicorn %s)" % (ours, excp)]
    if ours:
        par = u[PAR_REG[access]]
        dfsr, dfar = p[18], p[19]
        if not par & 1:
            bad.append("abort but unicorn's PAR %08x says mapped" % par)
        elif (dfsr & 0xf) | ((dfsr >> 10 & 1) << 4) != (par >> 1 & 0x1f):
            bad.append("dfsr")
        if bool(dfsr & (1 << 11)) != access.startswith("st"):
            bad.append("dfsr.WnR")
        if dfar != va:
            bad.append("dfar")
        return bad
    for i in range(13, 18):
        if p[i] != u[i]:
            bad.append(NAMES[i])
    return bad


def outside_ram(par):
    """A successful PAR naming a physical page outside Unicorn's 1 MB."""
    return not par & 1 and (par & 0xfffff000) >= RAM_SIZE


def par_fault(fs):
    return 1 | ((fs & 0xf) << 1) | ((fs >> 4 & 1) << 5)


def known_qemu_bug(detail, p):
    """Why a difference is Unicorn's, or None.

    QEMU 5.0's get_phys_addr_v6() checks the domain as soon as it has the
    first-level descriptor, and checks the access flag only for a Client
    domain. The architecture orders the faults Translation, Access flag,
    Domain, Permission, and checks a page's domain only once its second-level
    descriptor has been read: ARM DDI 0100E Table 3-5 and Figure 3-9, and
    DDI 0406C's TranslationTableWalkSD(), as QEMU's own maintainers set out
    in the 2026 qemu-arm thread "target/arm: re-order ... domain fault"
    (https://ratatoskr.run/qemu-arm/2026/08/17460220/t), which moves QEMU's
    check. So three shapes of case differ, and each is accepted only if
    every one of our four PARs is the architecture's answer."""
    if detail["pd1"] or detail["kind"] in ("fault", "type3"):
        return None
    pars = p[7:11]
    afe = bool(detail["sctlr"] & (1 << 29))
    dac = detail["dac"]
    page = detail["kind"] == "table"
    if page and detail["l2"] == "fault" and dac in (0, 2):
        if all(v == par_fault(0x7) for v in pars):
            return ("page translation fault in a No access/reserved domain: "
                    "QEMU 5.0 reports the domain fault first")
        return None
    if afe and detail["af"] == 0:
        want = par_fault(0x6 if page else 0x3)
        if not all(v == want for v in pars):
            return None
        if dac in (0, 2):
            return ("access flag fault in a No access/reserved domain: "
                    "QEMU 5.0 reports the domain fault first")
        if dac == 3:
            return ("access flag fault in a Manager domain: QEMU 5.0 checks "
                    "the access flag only for Client domains")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--show", type=int, default=10)
    args = ap.parse_args()

    built = [p for p in PROBE_CANDIDATES if os.path.exists(p)]
    if not built:
        sys.exit("arm_diff_probe not built -- run: "
                 "cmake --build build --target arm_diff_probe")
    probe = max(built, key=os.path.getmtime)
    print("probe: %s" % os.path.relpath(probe, REPO))
    proc = subprocess.Popen([probe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    rng = random.Random(args.seed)
    buckets = collections.Counter()
    kinds = collections.Counter()
    shown = collections.Counter()
    why = collections.Counter()
    aborts = 0
    for _ in range(args.count):
        regs, pokes, access, detail = rand_case(rng)
        code = program(access)
        status, p = run_probe(proc, regs, pokes, code)
        u, err, excp = run_unicorn(regs, pokes, code)
        kinds[detail["kind"] + ("/" + detail["l2"] if "l2" in detail else "")] += 1
        if status != 0:
            bucket, bad = "DIFF", ["probe status %d" % status]
        elif err is not None and err.errno in (UC_ERR_READ_UNMAPPED, UC_ERR_WRITE_UNMAPPED,
                                               UC_ERR_FETCH_UNMAPPED):
            # r12 is the load's destination, which only the probe filled.
            bad = [n for n in compare(p, u, excp, access, detail["va"])
                   if n in NAMES[:12]]
            bucket = "unmapped" if not bad else "DIFF"
        elif err is not None:
            bucket, bad = "DIFF", ["unicorn: %s" % err]
        elif excp is None and u[15] == CODE + 4 * (len(code) - 1) and \
                outside_ram(p[PAR_REG[access]]):
            # A load from outside its RAM: Unicorn stops at it, silently.
            bad = [n for n in compare(p, u, excp, access, detail["va"])
                   if n in NAMES[:12]]
            bucket = "unmapped" if not bad else "DIFF"
        else:
            bad = compare(p, u, excp, access, detail["va"])
            bucket = "agree" if not bad else "DIFF"
        if bucket == "agree" and excp == EXCP_DATA_ABORT:
            aborts += 1
        if bucket == "DIFF":
            reason = known_qemu_bug(detail, p)
            if reason:
                bucket = "qemu-bug"
                why[reason] += 1
        buckets[bucket] += 1
        if bucket == "DIFF" and shown[bucket] < args.show:
            shown[bucket] += 1
            print("DIFF %s %s access=%s" % (bad, {k: (hex(v) if isinstance(v, int) and
                                                       not isinstance(v, bool) else v)
                                                   for k, v in detail.items()}, access))
            for name in bad:
                if name in NAMES and name not in ("dfsr", "dfar"):
                    i = NAMES.index(name)
                    print("   %-5s ours %08x  unicorn %08x" % (name, p[i], u[i]))
            print("   pc/cpsr ours %08x/%08x unicorn %08x/%08x; our dfsr/dfar %08x/%08x" %
                  (p[15], p[16], u[15], u[16], p[18], p[19]))
    print("cases: %d  %s" % (args.count, dict(sorted(buckets.items()))))
    print("agreeing cases whose access aborted on both sides: %d" % aborts)
    print("descriptor kinds: %s" % dict(sorted(kinds.items())))
    for reason, count in why.most_common():
        print("  qemu-bug x%d: %s" % (count, reason))
    return 1 if buckets["DIFF"] else 0


if __name__ == "__main__":
    sys.exit(main())
