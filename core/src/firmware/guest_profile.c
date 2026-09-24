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
    return true;
}

void gprof_free(gprof_t *p) {
    if (!p) return;
    free(p->slot);
    memset(p, 0, sizeof *p);
}

void gprof_reset(gprof_t *p) {
    if (!p || !p->slot) return;
    memset(p->slot, 0, (size_t)p->cap * sizeof *p->slot);
    p->used = 0;
    p->samples = p->user = p->dropped = 0;
}

static uint32_t hash_pc(uint32_t pc) {
    uint32_t h = pc * 0x9e3779b1u;
    return h ^ (h >> 15);
}

void gprof_note(gprof_t *p, uint32_t pc, bool user) {
    if (!p || !p->slot) return;
    p->samples++;
    if (user) p->user++;
    pc &= ~1u;
    const uint32_t mask = p->cap - 1u;
    uint32_t i = hash_pc(pc) & mask;
    for (uint32_t probe = 0; probe < p->cap; probe++, i = (i + 1u) & mask) {
        gprof_slot_t *s = &p->slot[i];
        if (s->count && s->pc == pc) { s->count++; return; }
        if (!s->count) {
            /* Keep a quarter free so probes stay short. */
            if (p->used >= p->cap - p->cap / 4u) break;
            s->pc = pc;
            s->count = 1;
            p->used++;
            return;
        }
    }
    p->dropped++;
}

bool gprof_copy(gprof_t *to, const gprof_t *from) {
    if (!to || !from || !to->slot || !from->slot || to->cap != from->cap) return false;
    memcpy(to->slot, from->slot, (size_t)from->cap * sizeof *from->slot);
    to->used = from->used;
    to->samples = from->samples;
    to->user = from->user;
    to->dropped = from->dropped;
    return true;
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
