/*
 * NEON — the iPhone 3GS (N88AP, S5L8920) machine, as far as iOS 6 has asked.
 *
 * This is the machine tools/boot3gs.c discovered, moved into the core so the
 * app and the desktop harness run the same code. It is deliberately small:
 * every device in it is one the iOS 6.1.6 kernel was seen to use, modelled
 * from that kernel's own code (docs/IOS6_READINESS.md, step 4), and every
 * other device address answers zero and is counted, so what the kernel asks
 * for next shows up as a number rather than a silent wrong answer.
 *
 * What is here:
 *   - 256 MB of DRAM at 0x40000000; the kernel is linked at 0x80000000 and
 *     /arm-io maps devices at physical 0x80000000 and up.
 *   - UART0 (Samsung UART, 0x82500000) as a polled console: transmitted bytes
 *     go to a ring the caller drains, and the status register always reports
 *     an empty transmitter.
 *   - The PMGR timer (0xbf100200): a 64-bit count, a decrementer and an
 *     enable/acknowledge control, raising VIC0 line 6.
 *   - Three PL192 VICs (0xbf200000 + n * 0x10000), the S5L8900's model.
 *   - The Cortex-A8 CPU profile (ARM_ARCH_V7_A8), optionally on the cached
 *     interpreter.
 *   - Bring-up as iBoot would do it: segments mapped, the device tree filled
 *     in (memory, clocks, /pram, the NVRAM image, the memory map), boot_args,
 *     and the IOP un-matched, because nothing emulates that coprocessor.
 *
 * What is not: storage (the kernel waits for its root device), the display,
 * touch, buttons, audio, the IOP, sleep. It boots the kernel as far as that
 * allows, and prints what the kernel prints.
 *
 * Time: the count advances at N88_TB_HZ against one retired instruction per
 * cycle at N88_CPU_HZ. When the guest waits for an interrupt, time jumps to
 * the decrementer's expiry instead of the host spinning, so an idle guest's
 * clock runs ahead of the wall clock.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef NEON_N88_H
#define NEON_N88_H

#include "arm.h"
#include "arm_ci.h"
#include "soc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define N88_DRAM_BASE   UINT32_C(0x40000000)
#define N88_DRAM_SIZE   UINT32_C(0x10000000)    /* 256 MB */
#define N88_VIRT_BASE   UINT32_C(0x80000000)
/*
 * The top of DRAM is boot-owned, as iBoot leaves it: boot_args.memSize stops
 * below it. It holds /pram, where the kernel keeps its panic log; the
 * platform expert refuses anything under 16 KB (0x8027baac in 10B500). The
 * size is PROVISIONAL, the 3GS's own is not known here.
 */
#define N88_TOP_RESERVE UINT32_C(0x00100000)
#define N88_PRAM_SIZE   UINT32_C(0x00010000)

/*
 * Clock frequencies the device tree carries as zero and iBoot fills in. CPU is
 * the iPhone 3GS's documented 600 MHz. The rest are PROVISIONAL: the timebase
 * only has to agree with the timer model below, and the others only have to be
 * non-zero and in sane ratios. Replace them if an iPhone2,1 IORegistry dump
 * turns up.
 */
#define N88_CPU_HZ  600000000u
#define N88_BUS_HZ  100000000u
#define N88_MEM_HZ  200000000u
#define N88_PRF_HZ   50000000u
#define N88_FIX_HZ   24000000u
#define N88_TB_HZ    24000000u
#define N88_CYCLES_PER_TICK (N88_CPU_HZ / N88_TB_HZ)

#define N88_UART0_PA    UINT32_C(0x82500000)
#define N88_TIMER_PA    UINT32_C(0xbf100200)
#define N88_VIC_PA      UINT32_C(0xbf200000)
#define N88_VIC_COUNT   3u
#define N88_TIMER_LINE  6u

#define N88_CONSOLE_CAPACITY 65536u
#define N88_DEFAULT_CMDLINE  "debug=0x8 serial=3 -v"

/* Optional: every access to an address that is neither DRAM nor a modelled
 * register, as the harness logs them. `pc` is the instruction's. */
typedef void (*n88_trace_fn)(void *ctx, uint32_t pa, unsigned size, bool write,
                             uint32_t value, uint32_t pc);

typedef struct n88 {
    arm_cpu_t cpu;
    arm_bus_t bus;
    uint8_t  *ram;                  /* N88_DRAM_SIZE bytes at N88_DRAM_BASE   */
    arm_ci_t *ci;                   /* NULL: every instruction on arm_step    */
    bool      level_dirty;          /* a device was touched (arm_ci stops)    */

    s5l_vic_t vic[N88_VIC_COUNT];
    struct {
        uint64_t start;             /* count when the decrementer was written */
        uint32_t interval;
        bool     armed, pending;
        uint32_t ctrl;
        uint64_t fired;
    } timer;

    char     console[N88_CONSOLE_CAPACITY];   /* UART0 output, a ring       */
    size_t   console_head, console_len;
    uint64_t console_total, console_dropped;

    uint64_t mmio;                  /* modelled register accesses             */
    uint64_t unmodelled;            /* accesses nothing answers               */
    uint64_t wfi;                   /* waits for interrupt                    */
    n88_trace_fn trace;
    void        *trace_ctx;

    /* What bring-up did, for the caller to report. */
    bool     booted;
    uint32_t entry_pa, boot_args_pa, devicetree_pa, devicetree_size, tokd_pa;
} n88_t;

typedef enum {
    N88_OK = 0,
    N88_ERR_ARGUMENT,
    N88_ERR_MEMORY,
    N88_ERR_KERNEL,             /* not an ARM Mach-O executable we can map  */
    N88_ERR_DEVICETREE,         /* not a complete Apple flat tree, or a
                                   property bring-up needs is missing       */
    N88_ERR_LAYOUT              /* the pieces do not fit in DRAM            */
} n88_status_t;

typedef struct {
    const uint8_t *kernel;      /* decrypted, decompressed kernelcache      */
    size_t         kernel_size;
    const uint8_t *devicetree;  /* decrypted flat device tree               */
    size_t         devicetree_size;
    const char    *cmdline;     /* NULL: N88_DEFAULT_CMDLINE                */
    /* Device-tree nodes to un-match (an 'x' over the first byte of their
     * compatible). NULL selects the default, "arm-io/iop"; set
     * unmatch_count to 0 with a non-NULL list to un-match nothing. */
    const char *const *unmatch;
    unsigned           unmatch_count;
} n88_boot_t;

/* Allocate DRAM, wire the bus, reset the CPU. `cached_engine` puts the CPU on
 * the cached interpreter. False only if memory ran out. */
bool n88_init(n88_t *m, bool cached_engine);
void n88_free(n88_t *m);

/* Load the kernel and device tree and point the CPU at the entry. `detail`
 * (may be NULL) gets one sentence on failure. */
n88_status_t n88_boot(n88_t *m, const n88_boot_t *req, char *detail,
                      size_t detail_capacity);
const char *n88_strerror(n88_status_t st);

/* Run up to `max_steps` instructions; returns how many retired. Stops early
 * only on a non-OK CPU status, which `status` carries. */
unsigned n88_run(n88_t *m, unsigned max_steps, arm_status_t *status);

/* Move up to `capacity` bytes of console output (oldest first) into `out`;
 * returns the count. What was taken is gone. */
size_t n88_console_take(n88_t *m, char *out, size_t capacity);

uint64_t n88_timer_count(const n88_t *m);
double   n88_guest_seconds(const n88_t *m);

/* True for an iPhone 3GS device tree (root compatible "N88AP"). The app uses
 * it to tell 3GS firmware from iPhone OS 3 firmware without trusting names. */
bool n88_devicetree_is_3gs(const uint8_t *dt, size_t len);

/* The smallest NVRAM image IODTNVRAM parses (see n88.c); exposed for tests. */
void n88_nvram_image(uint8_t *img, uint32_t len);

/* The bus, for tests and the harness: the same routing the CPU sees. */
uint32_t n88_read32(n88_t *m, uint32_t pa);
void     n88_write32(n88_t *m, uint32_t pa, uint32_t v);

#endif /* NEON_N88_H */
