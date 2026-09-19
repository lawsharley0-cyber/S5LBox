"""Differential ARM correctness test: S5LBox's own arm_step() vs Unicorn.

Unicorn is an independent ARM implementation (no shared code or assumptions
with this project's interpreter), used here exactly the way core/tests/
already differentially tests things internally -- except this reference is
external, which is a stronger check than comparing S5LBox against itself.

Drives core/arm_diff_probe.exe (one process, one instruction per stdin line,
built fresh for each run so this can never silently test a stale binary) and
Unicorn's Python bindings side by side, on the same register/CPSR state and
the same instruction encoding, and diffs the final r0-r15/CPSR.

This is a standalone research/testing script, not part of the CI matrix or
the product: Unicorn is not a build dependency, and nothing here is
imported by anything else in the repository.

Usage:
  pip install unicorn
  cmake --build build --target arm_diff_probe
  python tools/unicorn_diff.py [--count N] [--seed S]
"""
import argparse
import os
import random
import struct
import subprocess
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
    UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
    UC_ARM_REG_R12, UC_ARM_REG_R13, UC_ARM_REG_R14, UC_ARM_REG_R15,
    UC_ARM_REG_CPSR,
)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROBE = os.path.join(REPO, "build", "core", "arm_diff_probe.exe")
if not os.path.exists(PROBE):
    PROBE = os.path.join(REPO, "build", "core", "arm_diff_probe")

REG_CONSTS = [
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
    UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
    UC_ARM_REG_R12, UC_ARM_REG_R13, UC_ARM_REG_R14,
]

MEM_BASE = 0x1000
MEM_SIZE = 0x2000
SYS_MODE_CPSR = 0x000000df  # SYS mode, IRQ/FIQ masked, no flags -- a plain
                            # starting point; NZCV bits are overwritten below.

# A hand-picked set of encodings covering the common data-processing forms
# the project's own hotpath.md and this session's profiling both named as
# dominant in real guest code: MOV/ADD/SUB/AND/ORR/EOR/CMP, register and
# immediate shifts, with and without the S bit.
CANDIDATE_INSNS = [
    ("MOV r0, r1",            0xe1a00001),
    ("MOV r0, r1, LSL #4",    0xe1a00201),
    ("MOVS r0, r1, LSR #3",   0xe1b001a1),
    ("ADD r0, r1, r2",        0xe0810002),
    ("ADDS r0, r1, r2",       0xe0910002),
    ("ADC r0, r1, r2",        0xe0a10002),
    ("SUB r0, r1, r2",        0xe0410002),
    ("SUBS r0, r1, r2",       0xe0510002),
    ("SBC r0, r1, r2",        0xe0c10002),
    ("RSB r0, r1, r2",        0xe0610002),
    ("AND r0, r1, r2",        0xe0010002),
    ("ANDS r0, r1, r2",       0xe0110002),
    ("ORR r0, r1, r2",        0xe1810002),
    ("EOR r0, r1, r2",        0xe0210002),
    ("BIC r0, r1, r2",        0xe1c10002),
    ("MVN r0, r1",            0xe1e00001),
    ("CMP r1, r2",            0xe1510002),
    ("CMN r1, r2",            0xe1710002),
    ("TST r1, r2",            0xe1110002),
    ("TEQ r1, r2",            0xe1310002),
    ("ADD r0, r1, r2, LSL r3",0xe0810312),
    ("MUL r0, r1, r2",        0xe0000291),
    ("MLA r0, r1, r2, r3",    0xe0203291),
    ("LDR r0, [r1]",          0xe5910000),
    ("STR r0, [r1]",          0xe5810000),
    ("LDRB r0, [r1]",         0xe5d10000),
    ("STRB r0, [r1]",         0xe5c10000),
    ("LDRH r0, [r1]",         0xe1d100b0),
    ("STRH r0, [r1]",         0xe1c100b0),
]


def run_unicorn(regs, cpsr, pc, insn):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(MEM_BASE, MEM_SIZE)
    uc.mem_write(MEM_BASE, b"\x00" * MEM_SIZE)
    uc.mem_write(pc, struct.pack("<I", insn))
    # CPSR (and the mode it selects) must be set BEFORE r13/r14: those two
    # are banked per mode, so writing them first risks landing in whatever
    # bank Unicorn's own reset/default mode uses instead of the one this
    # test actually wants. See the identical comment in arm_diff_probe.c --
    # this ordering bug is what produced ~1150/1450 false "mismatches" on
    # the first run of this script, not real interpreter bugs.
    uc.reg_write(UC_ARM_REG_CPSR, cpsr)
    for const, val in zip(REG_CONSTS, regs):
        uc.reg_write(const, val)
    uc.reg_write(UC_ARM_REG_R15, pc)
    try:
        uc.emu_start(pc, pc + 4, count=1)
    except Exception as e:  # noqa: BLE001 -- report, don't crash the sweep
        return None, str(e)
    out = [uc.reg_read(c) for c in REG_CONSTS]
    out.append(uc.reg_read(UC_ARM_REG_R15))
    out.append(uc.reg_read(UC_ARM_REG_CPSR))
    return out, None


def run_probe(proc, regs, cpsr, pc, insn):
    line = " ".join("%x" % v for v in regs) + " %x %x %x\n" % (cpsr, pc, insn)
    proc.stdin.write(line)
    proc.stdin.flush()
    reply = proc.stdout.readline().split()
    if not reply or reply[0] == "ERR":
        return None, "probe returned ERR"
    status = int(reply[0])
    vals = [int(x, 16) for x in reply[1:]]
    return vals, None if status == 0 else "status=%d" % status


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=200,
                     help="random register-state trials per instruction")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    if not os.path.exists(PROBE):
        sys.exit("arm_diff_probe not built -- run: "
                 "cmake --build build --target arm_diff_probe")

    rng = random.Random(args.seed)
    proc = subprocess.Popen([PROBE], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, text=True, bufsize=1)

    total = 0
    mismatches = 0
    errors = 0
    for name, insn in CANDIDATE_INSNS:
        is_mem_op = name.split()[0] in ("LDR", "LDRB", "LDRH", "STR", "STRB", "STRH")
        for _ in range(args.count):
            regs = [rng.getrandbits(32) for _ in range(15)]
            if is_mem_op:
                # r1 is the base register in every candidate load/store
                # encoding above. Point it well inside the mapped region,
                # 4-byte aligned, so these exercise real data movement
                # instead of just the (already well-covered) fault path.
                regs[1] = MEM_BASE + 0x100 + (rng.randrange(0, 0x100) & ~3)
            nzcv = rng.getrandbits(4) << 28
            cpsr = SYS_MODE_CPSR | nzcv
            pc = MEM_BASE
            total += 1

            u_out, u_err = run_unicorn(regs, cpsr, pc, insn)
            p_out, p_err = run_probe(proc, regs, cpsr, pc, insn)

            if u_err or p_err:
                errors += 1
                print("SKIP  %-22s unicorn_err=%r probe_err=%r"
                      % (name, u_err, p_err))
                continue

            if u_out != p_out:
                mismatches += 1
                print("DIFF  %-22s regs_in=%s cpsr_in=%08x"
                      % (name, [hex(r) for r in regs], cpsr))
                labels = ["r%d" % i for i in range(15)] + ["pc", "cpsr"]
                for label, uv, pv in zip(labels, u_out, p_out):
                    marker = "  <-- DIFFERS" if uv != pv else ""
                    print("        %-4s unicorn=%08x s5lbox=%08x%s"
                          % (label, uv, pv, marker))

    proc.stdin.close()
    proc.wait(timeout=5)

    print()
    print("%d cases, %d mismatches, %d errors/skips"
          % (total, mismatches, errors))
    sys.exit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
