/*
 * S5LBox — where does the guest spend its instructions?
 *
 * A sampling profile of guest program counters, and the symbolization that
 * turns them into "which library, which function". The emulator samples the
 * pc at a fixed instruction interval (the app: every 100,000 retired
 * instructions), so a sample count is a share of retired guest instructions.
 *
 * User-space code on iPhone OS 3 lives almost entirely in the dyld shared
 * cache (every framework and libSystem, mapped at the same address in every
 * process). gprof_cache_* read that cache FILE -- as bytes the caller has
 * loaded, e.g. from the pristine root filesystem image -- for its image list
 * and each image's own LC_SYMTAB, so an address resolves to
 * "QuartzCore  _CALayerSetNeedsDisplay +0x1c". Kernel addresses are named by
 * ksyms.h from the kernelcache.
 *
 * Nothing here reads guest memory or touches a machine; it is plain parsing
 * over caller-owned buffers, bounded and fail-soft: a truncated or unexpected
 * cache yields fewer names, never a crash or a guess.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_GUEST_PROFILE_H
#define S5LBOX_GUEST_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------ histogram --- */

typedef struct {
    uint32_t pc;       /* Thumb bit cleared                                  */
    uint16_t proc;     /* index into gprof_t::proc; 0 = not attributed       */
    uint16_t pad;
    uint32_t count;    /* 0 = empty slot                                     */
} gprof_slot_t;

/*
 * Which process a sample ran in. A process is an address space: the guest's
 * TTBR0 while the sample was taken (XNU loads each task's own first-level
 * table there), named by the executable path the kernel copied to the top of
 * that task's user stack at exec. A TTBR0 is reused once its task exits, so
 * a process is the PAIR (TTBR0, name); an address space whose name cannot be
 * read (a kernel thread, a page not yet present) keeps an empty name and is
 * still counted apart from the others.
 */
/* 256: a boot and a few app launches start well over 64 programs (launchctl,
 * fsck, mtmergeprops, IQAgent, ReportCrash...), and a table that fills early
 * leaves everything after it unattributed. ~31 KB per profile. */
#define GPROF_MAX_PROCS 256u
#define GPROF_NAME_MAX  96u

typedef struct {
    uint32_t ttbr0;
    char     name[GPROF_NAME_MAX];   /* exec path, or "" if unreadable        */
    uint64_t samples;
    uint64_t user;
    /* The profile's sample count (gprof_t::samples, 1-based) at this
     * process's first and latest sample: where in the window it ran, and
     * whether it was still running at the end. 0 until it is sampled. */
    uint64_t first, last;
} gprof_proc_t;

/*
 * A sampled call stack: the pc, then return addresses from the frame-pointer
 * chain (iPhone OS code keeps r7 pointing at {saved r7, saved lr} in ARM and
 * Thumb alike), outermost last. Identical stacks share one entry.
 */
#define GPROF_STACK_MAX 12u

typedef struct {
    uint32_t frame[GPROF_STACK_MAX];   /* Thumb bits cleared                  */
    uint32_t count;                    /* 0 = empty slot                      */
    uint16_t proc;
    uint8_t  depth;
    uint8_t  pad;
} gprof_stack_t;

typedef struct {
    gprof_slot_t *slot;     /* open addressing, power-of-two capacity         */
    uint32_t      cap;
    uint32_t      used;
    uint64_t      samples;  /* every sample, dropped ones included            */
    uint64_t      user;     /* taken in User mode                             */
    uint64_t      dropped;  /* the table was full                              */
    /* proc[0] is the "not attributed" row; proc[1..nproc-1] are processes.  */
    gprof_proc_t  proc[GPROF_MAX_PROCS];
    uint32_t      nproc;
    uint64_t      proc_full;     /* samples of processes past GPROF_MAX_PROCS */
    /* The last lookup, so a name is read once per switch, not per sample. */
    uint32_t      last_ttbr0;
    uint16_t      last_proc;     /* may be 0: the table was full              */
    uint16_t      last_age;      /* samples since that name was last read     */
    bool          last_valid;
    /* Optional (gprof_init_stacks): distinct sampled call stacks.            */
    gprof_stack_t *stack;
    uint32_t      stack_cap;
    uint32_t      stack_used;
    uint64_t      stack_samples;
    uint64_t      stack_dropped;
} gprof_t;

/* 1 << cap_log2 distinct (pc, process) pairs (cap_log2 4..22). false on
 * allocation failure. */
bool gprof_init(gprof_t *p, unsigned cap_log2);
void gprof_free(gprof_t *p);
void gprof_reset(gprof_t *p);
void gprof_note(gprof_t *p, uint32_t pc, bool user);
/* The same, attributed to process `proc` (from gprof_proc_intern). */
void gprof_note_in(gprof_t *p, uint32_t pc, bool user, uint16_t proc);
/* Copy `from` into an initialised `to` of the same capacity (and the same
 * stack capacity, if either keeps stacks). */
bool gprof_copy(gprof_t *to, const gprof_t *from);

/* Also keep call stacks, 1 << cap_log2 distinct ones (4..20). */
bool gprof_init_stacks(gprof_t *p, unsigned cap_log2);
/* Count one stack of `depth` frames (clamped to GPROF_STACK_MAX; 0 ignored). */
void gprof_note_stack(gprof_t *p, const uint32_t *frames, unsigned depth, uint16_t proc);

/*
 * true, with *proc set, if the last lookup was of the same TTBR0 and its name
 * was read within the last `reread` samples; false means "read the name and
 * call gprof_proc_intern". Counts this call toward that age. *proc may be 0
 * (the table was full), and that answer is cached like any other.
 */
bool gprof_proc_cached(gprof_t *p, uint32_t ttbr0, uint16_t reread, uint16_t *proc);
/* The index of (ttbr0, name), adding it if new; 0 when the table is full
 * (the sample is then counted in proc_full and proc[0]). */
uint16_t gprof_proc_intern(gprof_t *p, uint32_t ttbr0, const char *name);

/* ------------------------------------------------- reading the guest RAM --- */

typedef struct {
    const uint8_t *ram;     /* guest DRAM                                     */
    uint32_t       base;    /* its physical address                           */
    uint32_t       size;
} gprof_ram_t;

/*
 * ARMv6 short-descriptor walk (sections, supersections, coarse tables with
 * large and small pages) of `va` through tables in `ram`. TTBCR.N picks
 * TTBR1 for the high addresses exactly as the MMU does. Reads only; false
 * on a fault or a table outside `ram`.
 */
bool gprof_va_to_pa(const gprof_ram_t *ram, uint32_t ttbr0, uint32_t ttbr1,
                    uint32_t ttbcr, uint32_t va, uint32_t *pa);

/*
 * The call stack at a sample, into frames[0..max): frames[0] = pc, then `lr`
 * unless it equals the first saved lr (a leaf that has not pushed a frame
 * returns there, so without it the immediate caller would be missing), then
 * the saved lr of each frame reached from `fp` (r7) through `ram`'s tables.
 * The walk stops at a zero or misaligned fp, a zero lr, an unmapped read, or a
 * next fp that is not above the current one by less than 1 MiB -- stacks grow
 * down, so a sane chain only climbs. Returns the number of frames (>= 1).
 */
unsigned gprof_backtrace(const gprof_ram_t *ram, uint32_t ttbr0, uint32_t ttbr1,
                         uint32_t ttbcr, uint32_t pc, uint32_t lr, uint32_t fp,
                         uint32_t *frames, unsigned max);

/*
 * The executable path of the task whose tables are `ttbr0`. exec copies the
 * path, argv and envp as one run of NUL-terminated strings ending just below
 * `stack_top` (iPhone OS 3 puts the main stack's top at 0x30000000), above
 * the argv/envp pointer array. The run is taken from the top of the two pages
 * below `stack_top` down to the first byte that is neither printable nor NUL,
 * and the path is its lowest complete string that starts with '/' and is at
 * least two bytes long. A leading "executable_path=" is dropped. Strings below
 * the run -- the process's own stack, where a function may hold any path -- are
 * never taken. false, with out[0] = 0, when the top page is not mapped in RAM
 * or the run holds no such string.
 */
bool gprof_exec_path(const gprof_ram_t *ram, uint32_t ttbr0, uint32_t ttbr1,
                     uint32_t ttbcr, uint32_t stack_top, char *out, size_t cap);

/* ------------------------------------------------ dyld shared cache file --- */

#define GPROF_CACHE_MAX_MAPPINGS 8u

typedef struct {
    uint64_t address, size, file_offset;
} gprof_mapping_t;

typedef struct {
    uint32_t    start;       /* __TEXT vmaddr                                  */
    uint32_t    end;         /* exclusive: __TEXT end, or the next image       */
    const char *path;        /* into the cache buffer, NUL-terminated          */
    uint32_t    header_off;  /* file offset of its mach_header, or UINT32_MAX  */
    /* Lazily built by gprof_cache_symbolize(): sorted N_SECT symbols. */
    bool        symbols_tried;
    uint32_t   *sym_value;
    const char **sym_name;
    uint32_t    nsym;
} gprof_image_t;

typedef struct {
    const uint8_t  *buf;     /* the cache file's first `len` bytes (or all)    */
    size_t          len;
    gprof_mapping_t map[GPROF_CACHE_MAX_MAPPINGS];
    unsigned        nmap;
    gprof_image_t  *image;   /* sorted by start                                */
    unsigned        nimage;
    char            detail[160];   /* why fewer names than hoped, if so        */
} gprof_cache_t;

/*
 * Parse the header, mappings and image list of a dyld shared cache held in
 * `buf` (the file from offset 0; a prefix is accepted, and then images whose
 * headers or symbol tables lie past it simply stay unnamed). false with
 * `detail` set when this is not a cache this parser understands.
 */
bool gprof_cache_open(gprof_cache_t *c, const uint8_t *buf, size_t len);
void gprof_cache_close(gprof_cache_t *c);

/* The image whose text holds `pc`, or NULL. */
gprof_image_t *gprof_cache_image_at(const gprof_cache_t *c, uint32_t pc);

/*
 * The nearest preceding symbol of `pc` in `img` (at most `max_span` bytes
 * before it), or NULL. *offset receives pc - symbol. Builds the image's
 * sorted symbol table on first use.
 */
const char *gprof_cache_symbolize(gprof_cache_t *c, gprof_image_t *img,
                                  uint32_t pc, uint32_t max_span,
                                  uint32_t *offset);

/* The last path component, for printing. */
const char *gprof_basename(const char *path);

#endif /* S5LBOX_GUEST_PROFILE_H */
