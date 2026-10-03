"""Read an iOS 5/6 armv7 dyld shared cache: symbols, references, callers.

The iPhone OS 3 tools (dscsym.py, dscmap.py, dscxref.py) read the armv6 cache
layout, where every symbol sits in its image. From iOS 5 on, the cache moves
the local (non-exported) symbols into one region of their own
(dyld_cache_local_symbols_info), and armv7 code is Thumb-2 that builds
addresses with movw/movt + add pc rather than literal pools. This reads both.
It was written to answer docs/IOS6_GRAPHICS.md's first question against the
iPhone 3GS iOS 6.1.6 cache.

Extract the cache from a decrypted root filesystem first:
  python tools/hfsx_extract.py rootfs.hfs dyld_shared_cache_armv7 dsc_armv7

Usage:
  python tools/dsc6.py syms    <cache> <image-substring> <regex>
  python tools/dsc6.py refs    <cache> <image> <target>...
        target: a C string in the image's __cstring ("CA_ENABLE_OGL"),
                sym:<mangled name> (and the address 8 past it, for vtables),
                or addr:<hex>
  python tools/dsc6.py callers <cache> <image> <hex address>
  python tools/dsc6.py dis     <cache> <hex address> <byte count>

<image> is matched against the end of the install path ("QuartzCore").
Names are printed mangled; pipe through c++filt. Needs capstone for refs,
callers and dis. Ships no Apple data and writes nothing.
"""
import bisect
import re
import struct
import sys


class Cache:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if not d.startswith(b"dyld_v1"):
            sys.exit("not a dyld shared cache")
        mo, mc, io_, ic = struct.unpack_from("<IIII", d, 16)
        self.lso, self.lss = struct.unpack_from("<QQ", d, 0x48)
        self.maps = [struct.unpack_from("<QQQII", d, mo + 32 * i) for i in range(mc)]
        self.images = []                        # (address, header offset, path)
        for i in range(ic):
            addr, _, _, pathoff, _ = struct.unpack_from("<QQQII", d, io_ + 32 * i)
            path = d[pathoff:d.find(b"\0", pathoff)].decode()
            self.images.append((addr, self.a2o(addr), path))

    def a2o(self, a):
        for addr, size, off, _, _ in self.maps:
            if addr <= a < addr + size:
                return a - addr + off
        return None

    def cstr(self, off):
        return self.data[off:self.data.find(b"\0", off)].decode("latin1")

    def image(self, name):
        hits = [im for im in self.images if im[2].endswith("/" + name) or
                (name.lower() in im[2].lower() and "/" not in name)]
        exact = [im for im in hits if im[2].endswith("/" + name)]
        return (exact or hits or [None])[0]

    def local_symbols(self, header_off=None):
        """(value, name, image path) from the local-symbols region."""
        if not self.lso:
            return
        d, lso = self.data, self.lso
        nlo, nlc, so, ss, eo, ec = struct.unpack_from("<6I", d, lso)
        by_off = {im[1]: im[2] for im in self.images}
        for k in range(ec):
            dylib_off, start, count = struct.unpack_from("<3I", d, lso + eo + 12 * k)
            if header_off is not None and dylib_off != header_off:
                continue
            for j in range(start, start + count):
                strx, _, _, _, value = struct.unpack_from("<IBBHI", d, lso + nlo + 12 * j)
                yield value, self.cstr(lso + so + strx), by_off.get(dylib_off, "?")

    def sections(self, header_off):
        d = self.data
        _, _, _, _, ncmds, _, _ = struct.unpack_from("<7I", d, header_off)
        off, out = header_off + 28, {}
        for _ in range(ncmds):
            cmd, csize = struct.unpack_from("<II", d, off)
            if cmd == 1:                                    # LC_SEGMENT
                nsects = struct.unpack_from("<I", d, off + 48)[0]
                for k in range(nsects):
                    s = off + 56 + 68 * k
                    sect = d[s:s + 16].split(b"\0")[0].decode()
                    seg = d[s + 16:s + 32].split(b"\0")[0].decode()
                    out[(seg, sect)] = struct.unpack_from("<II", d, s + 32)
            off += csize
        return out


def code_of(cache, image_name):
    im = cache.image(image_name)
    if not im:
        sys.exit("no image matching %r" % image_name)
    text_addr, text_size = cache.sections(im[1])[("__TEXT", "__text")]
    syms = sorted((v & ~1, n) for v, n, _ in cache.local_symbols(im[1])
                  if text_addr <= v < text_addr + text_size)
    starts = [s[0] for s in syms]

    def fn(a):
        i = bisect.bisect_right(starts, a) - 1
        return syms[i][1] if i >= 0 else "?"
    return im, text_addr, text_size, fn


def thumb():
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    md.skipdata = True
    return md


def cmd_syms(cache, image_name, regex):
    im = cache.image(image_name)
    pat = re.compile(regex)
    for value, name, path in cache.local_symbols(im[1] if im else None):
        if pat.search(name):
            print("%08x %s  [%s]" % (value, name, path.split("/")[-1]))


def cmd_refs(cache, image_name, wants):
    from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG
    im, text_addr, text_size, fn = code_of(cache, image_name)
    cs_addr, cs_size = cache.sections(im[1])[("__TEXT", "__cstring")]
    cstrings = cache.data[cache.a2o(cs_addr):cache.a2o(cs_addr) + cs_size]
    targets = {}
    for want in wants:
        if want.startswith("addr:"):
            targets[int(want[5:], 16)] = want
        elif want.startswith("sym:"):
            for value, name, _ in cache.local_symbols(im[1]):
                if name == want[4:]:
                    targets[value] = want
                    targets[value + 8] = want + "+8"
        else:
            i = cstrings.find(want.encode() + b"\0")
            while i > 0 and cstrings[i - 1] != 0:
                i = cstrings.find(want.encode() + b"\0", i + 1)
            if i >= 0:
                targets[cs_addr + i] = want
    print("targets: %s" % ", ".join("%x=%s" % kv for kv in sorted(targets.items())))
    md = thumb()
    md.detail = True
    d = cache.data
    code = d[cache.a2o(text_addr):cache.a2o(text_addr) + text_size]
    val = {}
    for ins in md.disasm(code, text_addr):
        m = ins.mnemonic
        try:
            ops = ins.operands
        except Exception:                                   # skipdata
            continue
        hit = None
        if m.startswith("movw") and len(ops) == 2 and ops[1].type == ARM_OP_IMM:
            val[ops[0].reg] = ops[1].imm & 0xffff
        elif m.startswith("movt") and len(ops) == 2 and ops[1].type == ARM_OP_IMM \
                and ops[0].reg in val:
            val[ops[0].reg] = (val[ops[0].reg] & 0xffff) | ((ops[1].imm & 0xffff) << 16)
            hit = val[ops[0].reg]
        elif m.startswith("add") and len(ops) == 2 and ops[1].type == ARM_OP_REG \
                and ins.reg_name(ops[1].reg) == "pc" and ops[0].reg in val:
            hit = (val[ops[0].reg] + ins.address + 4) & 0xffffffff
        elif m.startswith("ldr") and len(ops) == 2 and ops[1].type == ARM_OP_MEM \
                and ins.reg_name(ops[1].mem.base) == "pc":
            o = cache.a2o(((ins.address + 4) & ~3) + ops[1].mem.disp)
            if o is not None:
                hit = struct.unpack_from("<I", d, o)[0]
                val[ops[0].reg] = hit                       # maybe an add-pc offset
        if hit in targets:
            print("%08x  %-28s in %s" % (ins.address, targets[hit], fn(ins.address)))


def cmd_callers(cache, image_name, target):
    _, text_addr, text_size, fn = code_of(cache, image_name)
    code = cache.data[cache.a2o(text_addr):cache.a2o(text_addr) + text_size]
    for ins in thumb().disasm(code, text_addr):
        if ins.mnemonic in ("bl", "blx", "b", "b.w") and ins.op_str.startswith("#") \
                and int(ins.op_str[1:], 16) == target:
            print("%08x %-4s from %s" % (ins.address, ins.mnemonic, fn(ins.address)))


def cmd_dis(cache, start, count):
    o = cache.a2o(start)
    for ins in thumb().disasm(cache.data[o:o + count], start):
        print("%08x  %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, cache = sys.argv[1], Cache(sys.argv[2])
    args = sys.argv[3:]
    if cmd == "syms" and len(args) == 2:
        cmd_syms(cache, *args)
    elif cmd == "refs" and len(args) >= 2:
        cmd_refs(cache, args[0], args[1:])
    elif cmd == "callers" and len(args) == 2:
        cmd_callers(cache, args[0], int(args[1], 16))
    elif cmd == "dis" and len(args) == 2:
        cmd_dis(cache, int(args[0], 16), int(args[1], 0))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
