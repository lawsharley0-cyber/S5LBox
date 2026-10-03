"""Census of the CP15 registers an ARMv7 kernelcache reads and writes.

Scans the code of a decrypted, decompressed ARMv7 kernelcache (a Mach-O) for
every MCR/MRC to coprocessor 15, in ARM state (word-aligned words) and in
Thumb-2 (every halfword), and prints one row per register and direction:
how many sites, in the kernel proper or in the prelinked kexts, and one
example address. This is how exec_cp15_v7() in core/src/arm/arm_interp.c was
scoped to what iOS 6 actually uses (docs/IOS6_READINESS.md, step 2).

A linear scan, so read the output with that in mind. ARM-state hits (only
unconditional ones are counted) are mostly reliable: 0xE in the condition
field and the fixed MCR/MRC bits rarely occur by accident, though Thumb code
read as ARM still produces a few unnamed rows. Thumb hits are less so: a halfword pair inside ARM code or data can
look like a Thumb MCR/MRC, and the unnamed (?) Thumb rows are mostly that.
Check a hit by disassembling around it before relying on it.

Extract the kernelcache first (IMG3 decrypt with the device's key, then
LZSS; see tools/img3.py and core/src/lzss.c). Ships no Apple data and
writes nothing.

Usage:
  python tools/kcp15.py kernelcache.macho
"""
import collections
import struct
import sys

NAMES = {
    (0, 0, 0, 0): "MIDR", (0, 0, 0, 1): "CTR", (0, 0, 0, 2): "TCMTR",
    (0, 0, 0, 3): "TLBTR", (0, 0, 0, 5): "MPIDR",
    (0, 0, 1, 0): "ID_PFR0", (0, 0, 1, 1): "ID_PFR1", (0, 0, 1, 2): "ID_DFR0",
    (0, 0, 1, 3): "ID_AFR0", (0, 0, 1, 4): "ID_MMFR0", (0, 0, 1, 5): "ID_MMFR1",
    (0, 0, 1, 6): "ID_MMFR2", (0, 0, 1, 7): "ID_MMFR3",
    (0, 0, 2, 0): "ID_ISAR0", (0, 0, 2, 1): "ID_ISAR1", (0, 0, 2, 2): "ID_ISAR2",
    (0, 0, 2, 3): "ID_ISAR3", (0, 0, 2, 4): "ID_ISAR4", (0, 0, 2, 5): "ID_ISAR5",
    (1, 0, 0, 0): "CCSIDR", (1, 0, 0, 1): "CLIDR", (1, 0, 0, 7): "AIDR",
    (2, 0, 0, 0): "CSSELR",
    (0, 1, 0, 0): "SCTLR", (0, 1, 0, 1): "ACTLR", (0, 1, 0, 2): "CPACR",
    (0, 1, 1, 0): "SCR", (0, 1, 1, 1): "SDER", (0, 1, 1, 2): "NSACR",
    (0, 2, 0, 0): "TTBR0", (0, 2, 0, 1): "TTBR1", (0, 2, 0, 2): "TTBCR",
    (0, 3, 0, 0): "DACR",
    (0, 5, 0, 0): "DFSR", (0, 5, 0, 1): "IFSR", (0, 5, 1, 0): "ADFSR",
    (0, 5, 1, 1): "AIFSR", (0, 6, 0, 0): "DFAR", (0, 6, 0, 2): "IFAR",
    (0, 7, 0, 4): "WFI (ARMv6)", (0, 7, 1, 0): "ICIALLUIS", (0, 7, 1, 6): "BPIALLIS",
    (0, 7, 4, 0): "PAR", (0, 7, 5, 0): "ICIALLU", (0, 7, 5, 1): "ICIMVAU",
    (0, 7, 5, 4): "CP15ISB", (0, 7, 5, 6): "BPIALL", (0, 7, 5, 7): "BPIMVA",
    (0, 7, 6, 0): "inv. D cache (ARMv6)", (0, 7, 6, 1): "DCIMVAC",
    (0, 7, 6, 2): "DCISW", (0, 7, 7, 0): "inv. both caches (ARMv6)",
    (0, 7, 8, 0): "ATS1CPR", (0, 7, 8, 1): "ATS1CPW", (0, 7, 8, 2): "ATS1CUR",
    (0, 7, 8, 3): "ATS1CUW", (0, 7, 10, 1): "DCCMVAC", (0, 7, 10, 2): "DCCSW",
    (0, 7, 10, 4): "CP15DSB", (0, 7, 10, 5): "CP15DMB", (0, 7, 11, 1): "DCCMVAU",
    (0, 7, 14, 1): "DCCIMVAC", (0, 7, 14, 2): "DCCISW",
    (0, 8, 3, 0): "TLBIALLIS", (0, 8, 3, 1): "TLBIMVAIS", (0, 8, 3, 2): "TLBIASIDIS",
    (0, 8, 5, 0): "ITLBIALL", (0, 8, 5, 1): "ITLBIMVA", (0, 8, 5, 2): "ITLBIASID",
    (0, 8, 6, 0): "DTLBIALL", (0, 8, 6, 1): "DTLBIMVA", (0, 8, 6, 2): "DTLBIASID",
    (0, 8, 7, 0): "TLBIALL", (0, 8, 7, 1): "TLBIMVA", (0, 8, 7, 2): "TLBIASID",
    (0, 8, 7, 3): "TLBIMVAA",
    (1, 9, 0, 0): "L2 lockdown (A8)", (1, 9, 0, 2): "L2 aux control (A8)",
    (0, 9, 12, 0): "PMCR", (0, 9, 13, 0): "PMCCNTR", (0, 9, 14, 0): "PMUSERENR",
    (0, 10, 2, 0): "PRRR", (0, 10, 2, 1): "NMRR",
    (0, 12, 0, 0): "VBAR", (0, 12, 0, 1): "MVBAR",
    (0, 13, 0, 0): "FCSEIDR", (0, 13, 0, 1): "CONTEXTIDR",
    (0, 13, 0, 2): "TPIDRURW", (0, 13, 0, 3): "TPIDRURO", (0, 13, 0, 4): "TPIDRPRW",
}


def code_regions(data):
    """(name, file offset, size, vm address) for the kernel's code sections
    and the whole prelinked-kext text segment."""
    magic, _, _, _, ncmds = struct.unpack_from("<5I", data, 0)
    if magic != 0xfeedface:
        sys.exit("not a 32-bit Mach-O (decrypt and decompress it first)")
    off, out = 28, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == 1:                                            # LC_SEGMENT
            seg = data[off + 8:off + 24].split(b"\0")[0].decode()
            vm, _, fileoff, filesize = struct.unpack_from("<4I", data, off + 24)
            nsects = struct.unpack_from("<I", data, off + 48)[0]
            if seg == "__PRELINK_TEXT":
                out.append(("kexts", fileoff, filesize, vm))
            for k in range(nsects):
                s = off + 56 + 68 * k
                sect = data[s:s + 16].split(b"\0")[0].decode()
                addr, sz, soff = struct.unpack_from("<3I", data, s + 32)
                flags = struct.unpack_from("<I", data, s + 64)[0]
                # By flag, or by name: iOS 6's kernelcache zeroes the flags.
                if seg in ("__TEXT", "__KLD") and (flags & 0x80000400 or
                                                   sect in ("__text", "initcode")):
                    out.append(("kernel", soff, sz, addr))
        off += size
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    hits = collections.defaultdict(list)
    for where, lo, size, va in code_regions(data):
        for o in range(lo, lo + size - 3, 2):
            hw1, hw2 = struct.unpack_from("<HH", data, o)
            if (hw1 & 0xff00) == 0xee00 and (hw2 & 0x0f10) == 0x0f10:
                key = ("Thumb", (hw1 >> 4) & 1, (hw1 >> 5) & 7, hw1 & 15, hw2 & 15,
                       (hw2 >> 5) & 7)
                hits[key].append((where, va + o - lo))
            if o % 4 == 0:
                w = struct.unpack_from("<I", data, o)[0]
                # AL only: a conditional one is almost always Thumb code
                # read as ARM, and kernels do not make CP15 accesses
                # conditional.
                if (w & 0x0f000f10) == 0x0e000f10 and (w >> 28) == 0xe:
                    key = ("ARM", (w >> 20) & 1, (w >> 21) & 7, (w >> 16) & 15, w & 15,
                           (w >> 5) & 7)
                    hits[key].append((where, va + o - lo))
    rows = sorted(hits.items(), key=lambda kv: (kv[0][3], kv[0][2], kv[0][4], kv[0][5],
                                                kv[0][1], kv[0][0]))
    for (state, load, opc1, crn, crm, opc2), where in rows:
        name = NAMES.get((opc1, crn, crm, opc2), "?")
        count = collections.Counter(w for w, _ in where)
        print("%-5s %s p15,%d,c%-2d,c%-2d,%d  %-22s kernel %-4d kexts %-4d e.g. %08x" %
              (state, "MRC" if load else "MCR", opc1, crn, crm, opc2, name,
               count["kernel"], count["kexts"], where[0][1]))


if __name__ == "__main__":
    main()
