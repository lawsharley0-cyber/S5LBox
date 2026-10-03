/*
 * S5LBox — guest pc sampling and dyld shared cache symbolization.
 * See core/include/guest_profile.h.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "guest_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ histogram --- */

bool gprof_init(gprof_t *p, unsigned cap_log2) {
    if (!p) return false;
    memset(p, 0, sizeof *p);
    if (cap_log2 < 4u || cap_log2 > 22u) return false;
    p->cap = 1u << cap_log2;
    p->slot = calloc(p->cap, sizeof *p->slot);
    if (!p->slot) { p->cap = 0; return false; }
    p->nproc = 1u;                       /* proc[0]: not attributed */
    return true;
}

void gprof_free(gprof_t *p) {
    if (!p) return;
    free(p->slot);
    free(p->stack);
    memset(p, 0, sizeof *p);
}

bool gprof_init_stacks(gprof_t *p, unsigned cap_log2) {
    if (!p || p->stack || cap_log2 < 4u || cap_log2 > 20u) return false;
    p->stack = calloc((size_t)1 << cap_log2, sizeof *p->stack);
    if (!p->stack) return false;
    p->stack_cap = 1u << cap_log2;
    return true;
}

void gprof_reset(gprof_t *p) {
    if (!p || !p->slot) return;
    memset(p->slot, 0, (size_t)p->cap * sizeof *p->slot);
    p->used = 0;
    p->samples = p->user = p->dropped = 0;
    memset(p->proc, 0, sizeof p->proc);
    p->nproc = 1u;
    p->proc_full = 0;
    p->last_ttbr0 = 0;
    p->last_proc = 0;
    p->last_age = 0;
    p->last_valid = false;
    if (p->stack) memset(p->stack, 0, (size_t)p->stack_cap * sizeof *p->stack);
    p->stack_used = 0;
    p->stack_samples = p->stack_dropped = 0;
}

static uint32_t hash_key(uint32_t pc, uint16_t proc) {
    uint32_t h = (pc ^ (uint32_t)proc << 20) * 0x9e3779b1u;
    return h ^ (h >> 15);
}

void gprof_note_in(gprof_t *p, uint32_t pc, bool user, uint16_t proc) {
    if (!p || !p->slot) return;
    if (proc >= p->nproc) proc = 0;
    p->samples++;
    if (user) p->user++;
    gprof_proc_t *e = &p->proc[proc];
    if (!e->samples) e->first = p->samples;
    e->last = p->samples;
    e->samples++;
    if (user) e->user++;
    pc &= ~1u;
    const uint32_t mask = p->cap - 1u;
    uint32_t i = hash_key(pc, proc) & mask;
    for (uint32_t probe = 0; probe < p->cap; probe++, i = (i + 1u) & mask) {
        gprof_slot_t *s = &p->slot[i];
        if (s->count && s->pc == pc && s->proc == proc) { s->count++; return; }
        if (!s->count) {
            /* Keep a quarter free so probes stay short. */
            if (p->used >= p->cap - p->cap / 4u) break;
            s->pc = pc;
            s->proc = proc;
            s->count = 1;
            p->used++;
            return;
        }
    }
    p->dropped++;
}

void gprof_note(gprof_t *p, uint32_t pc, bool user) {
    gprof_note_in(p, pc, user, 0);
}

void gprof_note_stack(gprof_t *p, const uint32_t *frames, unsigned depth, uint16_t proc) {
    if (!p || !p->stack || !frames || !depth) return;
    if (depth > GPROF_STACK_MAX) depth = GPROF_STACK_MAX;
    if (proc >= p->nproc) proc = 0;
    p->stack_samples++;
    uint32_t h = 0x811c9dc5u ^ proc ^ depth << 16;
    for (unsigned i = 0; i < depth; i++) h = (h ^ (frames[i] & ~1u)) * 0x01000193u;
    h ^= h >> 15;
    const uint32_t mask = p->stack_cap - 1u;
    for (uint32_t probe = 0, i = h & mask; probe < p->stack_cap; probe++, i = (i + 1u) & mask) {
        gprof_stack_t *e = &p->stack[i];
        if (!e->count) {
            if (p->stack_used >= p->stack_cap - p->stack_cap / 4u) break;
            memset(e, 0, sizeof *e);
            for (unsigned k = 0; k < depth; k++) e->frame[k] = frames[k] & ~1u;
            e->depth = (uint8_t)depth;
            e->proc = proc;
            e->count = 1;
            p->stack_used++;
            return;
        }
        if (e->proc != proc || e->depth != depth) continue;
        unsigned k = 0;
        while (k < depth && e->frame[k] == (frames[k] & ~1u)) k++;
        if (k == depth) { e->count++; return; }
    }
    p->stack_dropped++;
}

bool gprof_copy(gprof_t *to, const gprof_t *from) {
    if (!to || !from || !to->slot || !from->slot || to->cap != from->cap ||
        to->stack_cap != from->stack_cap)
        return false;
    if (from->stack)
        memcpy(to->stack, from->stack, (size_t)from->stack_cap * sizeof *from->stack);
    to->stack_used = from->stack_used;
    to->stack_samples = from->stack_samples;
    to->stack_dropped = from->stack_dropped;
    memcpy(to->slot, from->slot, (size_t)from->cap * sizeof *from->slot);
    to->used = from->used;
    to->samples = from->samples;
    to->user = from->user;
    to->dropped = from->dropped;
    memcpy(to->proc, from->proc, sizeof to->proc);
    to->nproc = from->nproc;
    to->proc_full = from->proc_full;
    to->last_ttbr0 = from->last_ttbr0;
    to->last_proc = from->last_proc;
    to->last_age = from->last_age;
    to->last_valid = from->last_valid;
    return true;
}

bool gprof_proc_cached(gprof_t *p, uint32_t ttbr0, uint16_t reread, uint16_t *proc) {
    if (!p || !proc || !p->last_valid || p->last_ttbr0 != ttbr0 || p->last_age >= reread)
        return false;
    p->last_age++;
    *proc = p->last_proc;
    return true;
}

uint16_t gprof_proc_intern(gprof_t *p, uint32_t ttbr0, const char *name) {
    if (!p) return 0;
    if (!name) name = "";
    uint16_t found = 0;
    for (uint32_t i = 1; i < p->nproc; i++) {
        if (p->proc[i].ttbr0 == ttbr0 &&
            !strncmp(p->proc[i].name, name, GPROF_NAME_MAX - 1u)) {
            found = (uint16_t)i;
            break;
        }
    }
    if (!found && p->nproc < GPROF_MAX_PROCS) {
        gprof_proc_t *e = &p->proc[p->nproc];
        e->ttbr0 = ttbr0;
        snprintf(e->name, sizeof e->name, "%s", name);
        e->samples = e->user = 0;
        e->first = e->last = 0;
        found = (uint16_t)p->nproc++;
    }
    if (!found) p->proc_full++;
    p->last_ttbr0 = ttbr0;
    p->last_proc = found;
    p->last_age = 0;
    p->last_valid = true;
    return found;
}

/* ------------------------------------------------- reading the guest RAM --- */

static bool ram_word(const gprof_ram_t *m, uint32_t pa, uint32_t *w) {
    if (!m || !m->ram || (pa & 3u) || pa < m->base || m->size < 4u ||
        pa - m->base > m->size - 4u)
        return false;
    const uint8_t *b = m->ram + (pa - m->base);
    *w = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return true;
}

bool gprof_va_to_pa(const gprof_ram_t *ram, uint32_t ttbr0, uint32_t ttbr1,
                    uint32_t ttbcr, uint32_t va, uint32_t *pa) {
    const uint32_t n = ttbcr & 7u;
    uint32_t table;
    if (n && (va >> (32u - n)))
        table = ttbr1 & 0xffffc000u;
    else
        table = ttbr0 & (0xffffffffu << (14u - n));
    uint32_t d1;
    if (!ram_word(ram, table + ((va >> 20) << 2), &d1)) return false;
    switch (d1 & 3u) {
    case 1u: {                                           /* coarse table   */
        uint32_t d2;
        if (!ram_word(ram, (d1 & 0xfffffc00u) + (((va >> 12) & 0xffu) << 2), &d2))
            return false;
        if ((d2 & 3u) == 0u) return false;
        if ((d2 & 3u) == 1u) *pa = (d2 & 0xffff0000u) | (va & 0xffffu);  /* 64 KiB */
        else                 *pa = (d2 & 0xfffff000u) | (va & 0xfffu);   /* 4 KiB  */
        return true;
    }
    case 2u:                                             /* section        */
        if (d1 & (1u << 18)) *pa = (d1 & 0xff000000u) | (va & 0x00ffffffu);
        else                 *pa = (d1 & 0xfff00000u) | (va & 0x000fffffu);
        return true;
    default:
        return false;
    }
}

/* A word at a virtual address, with a one-page translation cache. */
typedef struct {
    const gprof_ram_t *ram;
    uint32_t ttbr0, ttbr1, ttbcr;
    uint32_t page_va, page_pa;
    bool     valid;
} va_reader_t;

static bool va_word(va_reader_t *r, uint32_t va, uint32_t *w) {
    if (va & 3u) return false;
    const uint32_t page = va & ~0xfffu;
    if (!r->valid || r->page_va != page) {
        uint32_t pa;
        if (!gprof_va_to_pa(r->ram, r->ttbr0, r->ttbr1, r->ttbcr, page, &pa)) {
            r->valid = false;
            return false;
        }
        r->page_va = page;
        r->page_pa = pa;
        r->valid = true;
    }
    return ram_word(r->ram, r->page_pa | (va & 0xfffu), w);
}

unsigned gprof_backtrace(const gprof_ram_t *ram, uint32_t ttbr0, uint32_t ttbr1,
                         uint32_t ttbcr, uint32_t pc, uint32_t lr, uint32_t fp,
                         uint32_t *frames, unsigned max) {
    if (!frames || !max) return 0;
    unsigned n = 0;
    frames[n++] = pc & ~1u;
    va_reader_t r = { ram, ttbr0, ttbr1, ttbcr, 0, 0, false };
    uint32_t saved_fp = 0, saved_lr = 0;
    bool have = fp && !(fp & 3u) && ram &&
                va_word(&r, fp, &saved_fp) && va_word(&r, fp + 4u, &saved_lr);
    if (n < max && lr && (!have || (lr & ~1u) != (saved_lr & ~1u)))
        frames[n++] = lr & ~1u;
    while (have && n < max && saved_lr) {
        frames[n++] = saved_lr & ~1u;
        if (!saved_fp || (saved_fp & 3u) || saved_fp <= fp || saved_fp - fp >= 0x100000u)
            break;
        fp = saved_fp;
        have = va_word(&r, fp, &saved_fp) && va_word(&r, fp + 4u, &saved_lr);
    }
    return n;
}

/* One page of guest memory, or NULL. */
static const uint8_t *user_page(const gprof_ram_t *ram, uint32_t ttbr0, uint32_t ttbr1,
                                uint32_t ttbcr, uint32_t va) {
    uint32_t pa;
    if (!gprof_va_to_pa(ram, ttbr0, ttbr1, ttbcr, va & ~0xfffu, &pa)) return NULL;
    if (ram->size < 0x1000u || pa < ram->base || pa - ram->base > ram->size - 0x1000u)
        return NULL;
    return ram->ram + (pa - ram->base);
}

bool gprof_exec_path(const gprof_ram_t *ram, uint32_t ttbr0, uint32_t ttbr1,
                     uint32_t ttbcr, uint32_t stack_top, char *out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    if (!ram || !ram->ram || stack_top < 0x2000u || (stack_top & 0xfffu)) return false;
    /* The two pages as one buffer, low page first, so a string may cross. */
    uint8_t buf[0x2000];
    const uint8_t *lo = user_page(ram, ttbr0, ttbr1, ttbcr, stack_top - 0x2000u);
    const uint8_t *hi = user_page(ram, ttbr0, ttbr1, ttbcr, stack_top - 0x1000u);
    if (!hi) return false;
    size_t len = 0;
    if (lo) { memcpy(buf, lo, 0x1000u); len = 0x1000u; }
    memcpy(buf + len, hi, 0x1000u);
    len += 0x1000u;
    /*
     * exec copies the path, then argv and envp, as one run of NUL-terminated
     * strings ending just below the stack top; the pointer array beneath
     * them holds 0x2fffxxxx words, whose 0xff bytes end the run. Everything
     * lower is the process's own stack, where any path a function has in a
     * local buffer can sit, so the run is found from the top down and the
     * path is the lowest complete string in it.
     */
    size_t r = len;
    while (r > 0 && (buf[r - 1u] == 0 || (buf[r - 1u] >= 0x20u && buf[r - 1u] < 0x7fu))) r--;
    static const char kPrefix[] = "executable_path=";
    for (size_t i = r; i < len; i++) {
        /* A string starts after a NUL; one at the run's own start is complete
         * only if the byte below it is visible (and so not part of it). */
        if (i > r ? buf[i - 1u] != 0 : i == 0) continue;
        if (buf[i] == 0) continue;
        size_t s = i;
        if (len - s > sizeof kPrefix - 1u &&
            !memcmp(buf + s, kPrefix, sizeof kPrefix - 1u))
            s += sizeof kPrefix - 1u;
        const uint8_t *nul = memchr(buf + i, 0, len - i);
        if (!nul) return false;                       /* runs off the top */
        const size_t e = (size_t)(nul - buf);
        if (s >= e || buf[s] != '/' || e - s < 2u) { i = e; continue; }
        size_t n = e - s < cap - 1u ? e - s : cap - 1u;
        memcpy(out, buf + s, n);
        out[n] = 0;
        return true;
    }
    return false;
}

/* ------------------------------------------------ dyld shared cache file --- */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* The bytes [off, off+n) of the file, if the buffer holds them. */
static const uint8_t *at(const gprof_cache_t *c, uint64_t off, uint64_t n) {
    if (off > c->len || n > c->len - off) return NULL;
    return c->buf + off;
}

static bool va_to_off(const gprof_cache_t *c, uint64_t va, uint64_t *off) {
    for (unsigned i = 0; i < c->nmap; i++) {
        const gprof_mapping_t *m = &c->map[i];
        if (va >= m->address && va - m->address < m->size) {
            *off = m->file_offset + (va - m->address);
            return true;
        }
    }
    return false;
}

/* A NUL-terminated string at file offset `off`, bounded by the buffer and
 * 1 KiB, or NULL. */
static const char *str_at(const gprof_cache_t *c, uint64_t off) {
    if (off >= c->len) return NULL;
    const size_t room = c->len - (size_t)off;
    const size_t limit = room < 1024u ? room : 1024u;
    const char *s = (const char *)c->buf + off;
    return memchr(s, '\0', limit) ? s : NULL;
}

static int image_cmp(const void *a, const void *b) {
    const gprof_image_t *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start ? 1 : 0;
}

#define MH_MAGIC_32   0xfeedfaceu
#define LC_SEGMENT_32 0x1u
#define LC_SYMTAB_32  0x2u

/* The __TEXT vmsize from the image's own mach header, or 0. */
static uint32_t text_size(const gprof_cache_t *c, uint32_t header_off) {
    const uint8_t *h = at(c, header_off, 28u);
    if (!h || le32(h) != MH_MAGIC_32) return 0;
    const uint32_t ncmds = le32(h + 16), sizeofcmds = le32(h + 20);
    const uint8_t *lc = at(c, (uint64_t)header_off + 28u, sizeofcmds);
    if (!lc) return 0;
    uint32_t off = 0;
    for (uint32_t i = 0; i < ncmds && off + 8u <= sizeofcmds; i++) {
        const uint32_t cmd = le32(lc + off), size = le32(lc + off + 4);
        if (size < 8u || size > sizeofcmds - off) break;
        if (cmd == LC_SEGMENT_32 && size >= 56u &&
            memcmp(lc + off + 8, "__TEXT", 7) == 0)
            return le32(lc + off + 28);
        off += size;
    }
    return 0;
}

bool gprof_cache_open(gprof_cache_t *c, const uint8_t *buf, size_t len) {
    if (!c) return false;
    memset(c, 0, sizeof *c);
    c->buf = buf;
    c->len = len;
    const uint8_t *h = at(c, 0, 0x28u);
    if (!h || memcmp(h, "dyld_v1 ", 8) != 0) {
        snprintf(c->detail, sizeof c->detail, "not a dyld shared cache (magic)");
        return false;
    }
    const uint32_t map_off = le32(h + 16), map_cnt = le32(h + 20);
    const uint32_t img_off = le32(h + 24), img_cnt = le32(h + 28);
    if (!map_cnt || map_cnt > GPROF_CACHE_MAX_MAPPINGS ||
        !at(c, map_off, (uint64_t)map_cnt * 32u) || !img_cnt || img_cnt > 4096u ||
        !at(c, img_off, (uint64_t)img_cnt * 32u)) {
        snprintf(c->detail, sizeof c->detail,
                 "unexpected cache header (%u mappings, %u images)", map_cnt, img_cnt);
        return false;
    }
    for (uint32_t i = 0; i < map_cnt; i++) {
        const uint8_t *m = c->buf + map_off + i * 32u;
        c->map[i].address = le64(m);
        c->map[i].size = le64(m + 8);
        c->map[i].file_offset = le64(m + 16);
    }
    c->nmap = map_cnt;
    c->image = calloc(img_cnt, sizeof *c->image);
    if (!c->image) {
        snprintf(c->detail, sizeof c->detail, "out of memory");
        return false;
    }
    for (uint32_t i = 0; i < img_cnt; i++) {
        const uint8_t *e = c->buf + img_off + i * 32u;
        const uint64_t addr = le64(e);
        if (addr > UINT32_MAX) continue;
        gprof_image_t *img = &c->image[c->nimage];
        img->start = (uint32_t)addr;
        img->path = str_at(c, le32(e + 24));
        if (!img->path) img->path = "?";
        uint64_t hoff;
        img->header_off = va_to_off(c, addr, &hoff) && hoff <= UINT32_MAX
                        ? (uint32_t)hoff : UINT32_MAX;
        c->nimage++;
    }
    qsort(c->image, c->nimage, sizeof *c->image, image_cmp);
    unsigned named = 0;
    for (unsigned i = 0; i < c->nimage; i++) {
        gprof_image_t *img = &c->image[i];
        const uint32_t size = img->header_off != UINT32_MAX
                            ? text_size(c, img->header_off) : 0u;
        /* Without its header the image is taken to run to the next one:
         * shared-cache text segments are laid out back to back. */
        uint32_t end = i + 1u < c->nimage ? c->image[i + 1u].start : img->start;
        if (size) {
            end = img->start + size;
            named++;
        } else if (i + 1u == c->nimage) {
            for (unsigned m = 0; m < c->nmap; m++)
                if (img->start >= c->map[m].address &&
                    img->start - c->map[m].address < c->map[m].size)
                    end = (uint32_t)(c->map[m].address + c->map[m].size);
        }
        img->end = end;
    }
    if (named < c->nimage)
        snprintf(c->detail, sizeof c->detail,
                 "%u of %u image headers were outside the bytes read",
                 c->nimage - named, c->nimage);
    return true;
}

void gprof_cache_close(gprof_cache_t *c) {
    if (!c) return;
    for (unsigned i = 0; i < c->nimage; i++) {
        free(c->image[i].sym_value);
        free((void *)c->image[i].sym_name);
    }
    free(c->image);
    memset(c, 0, sizeof *c);
}

gprof_image_t *gprof_cache_image_at(const gprof_cache_t *c, uint32_t pc) {
    if (!c || !c->nimage) return NULL;
    unsigned lo = 0, hi = c->nimage;
    while (lo < hi) {                         /* last image with start <= pc */
        const unsigned mid = lo + (hi - lo) / 2u;
        if (c->image[mid].start <= pc) lo = mid + 1u;
        else hi = mid;
    }
    if (!lo) return NULL;
    gprof_image_t *img = &c->image[lo - 1u];
    return pc < img->end ? img : NULL;
}

typedef struct { uint32_t value; const char *name; } sym_pair_t;

static int sym_cmp(const void *a, const void *b) {
    const sym_pair_t *x = a, *y = b;
    return x->value < y->value ? -1 : x->value > y->value ? 1 : 0;
}

static void build_symbols(gprof_cache_t *c, gprof_image_t *img) {
    img->symbols_tried = true;
    if (img->header_off == UINT32_MAX) return;
    const uint8_t *h = at(c, img->header_off, 28u);
    if (!h || le32(h) != MH_MAGIC_32) return;
    const uint32_t ncmds = le32(h + 16), sizeofcmds = le32(h + 20);
    const uint8_t *lc = at(c, (uint64_t)img->header_off + 28u, sizeofcmds);
    if (!lc) return;
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0, off = 0;
    for (uint32_t i = 0; i < ncmds && off + 8u <= sizeofcmds; i++) {
        const uint32_t cmd = le32(lc + off), size = le32(lc + off + 4);
        if (size < 8u || size > sizeofcmds - off) break;
        if (cmd == LC_SYMTAB_32 && size >= 24u) {
            symoff = le32(lc + off + 8);
            nsyms = le32(lc + off + 12);
            stroff = le32(lc + off + 16);
            strsize = le32(lc + off + 20);
        }
        off += size;
    }
    /* In the shared cache these are offsets into the cache file. */
    const uint8_t *nl = at(c, symoff, (uint64_t)nsyms * 12u);
    const uint8_t *strs = at(c, stroff, strsize);
    if (!nsyms || !nl || !strs) return;
    sym_pair_t *pairs = malloc((size_t)nsyms * sizeof *pairs);
    if (!pairs) return;
    uint32_t n = 0;
    for (uint32_t i = 0; i < nsyms; i++) {
        const uint8_t *e = nl + (size_t)i * 12u;
        const uint32_t strx = le32(e);
        const uint8_t type = e[4];
        const uint32_t value = le32(e + 8);
        if ((type & 0xe0u) != 0 || (type & 0x0eu) != 0x0eu) continue;  /* N_SECT, no stabs */
        if (value < img->start || value >= img->end) continue;           /* text only */
        if (strx >= strsize || !memchr(strs + strx, '\0', strsize - strx)) continue;
        pairs[n].value = value & ~1u;
        pairs[n].name = (const char *)strs + strx;
        n++;
    }
    if (!n) { free(pairs); return; }
    qsort(pairs, n, sizeof *pairs, sym_cmp);
    img->sym_value = malloc((size_t)n * sizeof *img->sym_value);
    img->sym_name = malloc((size_t)n * sizeof *img->sym_name);
    if (!img->sym_value || !img->sym_name) {
        free(img->sym_value); free((void *)img->sym_name);
        img->sym_value = NULL; img->sym_name = NULL;
        free(pairs);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        img->sym_value[i] = pairs[i].value;
        img->sym_name[i] = pairs[i].name;
    }
    img->nsym = n;
    free(pairs);
}

const char *gprof_cache_symbolize(gprof_cache_t *c, gprof_image_t *img,
                                  uint32_t pc, uint32_t max_span,
                                  uint32_t *offset) {
    if (!c || !img) return NULL;
    if (!img->symbols_tried) build_symbols(c, img);
    if (!img->nsym) return NULL;
    pc &= ~1u;
    unsigned lo = 0, hi = img->nsym;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2u;
        if (img->sym_value[mid] <= pc) lo = mid + 1u;
        else hi = mid;
    }
    if (!lo) return NULL;
    const uint32_t delta = pc - img->sym_value[lo - 1u];
    if (delta > max_span) return NULL;
    if (offset) *offset = delta;
    return img->sym_name[lo - 1u];
}

const char *gprof_basename(const char *path) {
    if (!path) return "?";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
