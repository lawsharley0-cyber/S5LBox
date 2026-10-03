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
 *     in (memory, clocks, /pram, /vram, the NVRAM image, the memory map),
 *     boot_args with iBoot's framebuffer, and the IOP un-matched, because
 *     nothing emulates that coprocessor.
 *
 *   - Optionally a root filesystem: a block device the kernel sees as the
 *     memory disk /dev/md0, published as the RAMDisk memory-map entry at a
 *     synthetic physical address (N88_MD_TOKEN_PA) and served by the
 *     memory-disk bridge (md_bridge.h). The kernel's strategy routine must
 *     have been patched to trap into the bridge; which bytes that takes is
 *     per kernel build and lives outside the core (tools/ios6_kernel_patch.c).
 *     With a root attached, the root node's "secure-root-prefix" is struck
 *     out, because nothing here answers the secure-root check it asks for.
 *
 *   - spi0 and the 1 MiB NOR flash on it, with the GPIO pin that selects it.
 *
 * What is not: the display, touch, buttons, audio, the IOP, sleep. It boots
 * the kernel as far as that allows, and prints what the kernel prints.
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
#include "cdma.h"
#include "m2clcd.h"
#include "s5l_sha1.h"
#include "s5l8920_i2c.h"
#include "s5l8920_dart.h"
#include "s5l8920_dsim.h"
#include "md_bridge.h"
#include "soc.h"
#include "spi_nor.h"
#include "vm_block.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define N88_DRAM_BASE   UINT32_C(0x40000000)
#define N88_DRAM_SIZE   UINT32_C(0x10000000)    /* 256 MB */
#define N88_VIRT_BASE   UINT32_C(0x80000000)
/*
 * The top of DRAM is boot-owned, laid out as this firmware's iBoot leaves it
 * (iPhone2,1 10B500), and boot_args.memSize stops below it:
 *
 *   - /pram, the last 16 KiB, where the kernel keeps its panic log
 *     (iBoot 0x4ff0fed0; the platform expert refuses less, 0x8027baac).
 *   - /vram below it: the display's framebuffers. iBoot's panel entry "n88"
 *     is 320 x 480 (0x4ff2b244), its colour space defaults to RGB888, which
 *     it lays out as 32 bits per pixel, 1280-octet rows, and it reserves
 *     three page-rounded buffers directly below /pram (0x4ff09348). /vram is
 *     that whole pool and Boot_Video describes the first buffer, at its base
 *     (0x4ff11f6c, 0x4ff0ff0e).
 */
#define N88_PRAM_SIZE   UINT32_C(0x00004000)
#define N88_PRAM_PA     (N88_DRAM_BASE + N88_DRAM_SIZE - N88_PRAM_SIZE)
#define N88_FB_WIDTH    320u
#define N88_FB_HEIGHT   480u
#define N88_FB_DEPTH    32u                       /* bits per pixel          */
#define N88_FB_STRIDE   (N88_FB_WIDTH * N88_FB_DEPTH / 8u)
#define N88_FB_BYTES    ((N88_FB_STRIDE * N88_FB_HEIGHT + 0xfffu) & ~0xfffu)
#define N88_FB_COUNT    3u
#define N88_VRAM_SIZE   (N88_FB_COUNT * N88_FB_BYTES)
#define N88_VRAM_PA     (N88_PRAM_PA - N88_VRAM_SIZE)
#define N88_TOP_RESERVE (N88_VRAM_SIZE + N88_PRAM_SIZE)

/*
 * The clock frequencies the device tree carries as zero and iBoot fills in,
 * as this firmware's boot loaders leave them (iPhone2,1 10B500). LLB programs
 * the PMGR PLLs and the 25 per-clock source/divider registers from constants
 * (LLB 0x840086a8); iBoot's platform init recomputes every clock from those
 * registers (iBoot 0x4ff13b40, PLLs at 0x4ff13c48) and its device-tree pass
 * writes the results (0x4ff13618). From the 24 MHz reference:
 *   PLL0  24 MHz x 150 / 6       = 600 MHz   the CPU
 *   PLL1  24 MHz x  81 / 6 / 2   = 162 MHz
 *   PLL2  24 MHz x 100 / 6 / 2   = 200 MHz
 * The CPU agrees with the iPhone 3GS's documented 600 MHz. The per-clock
 * table, /arm-io's clock-frequencies, is n88_clock_frequencies (n88.c); the
 * cpu0 values below are entries of it, as iBoot reads them. The timer model
 * ticks at the timebase.
 */
#define N88_CPU_HZ    600000000u    /* clock 15: PLL0 / 1                    */
#define N88_BUS_HZ    100000000u    /* clock 1:  PLL2 / 2                    */
#define N88_MEM_HZ    200000000u    /* clock 25: the CPU clock / 3           */
#define N88_PRF_HZ    100000000u    /* clock 2:  PLL2 / 2                    */
#define N88_FIX_HZ     24000000u    /* clock 19: the reference / 1           */
#define N88_TB_HZ      24000000u    /* clock 26: the reference               */
#define N88_USBPHY_HZ  24000000u    /* clock 27, /arm-io:usbphy-frequency    */
#define N88_NCOREF_HZ 162000000u    /* PLL1, audio-complex:ncoref-frequency  */
#define N88_CLOCK_COUNT 28u         /* clock-frequencies words iBoot writes  */
#define N88_CYCLES_PER_TICK (N88_CPU_HZ / N88_TB_HZ)

#define N88_UART0_PA    UINT32_C(0x82500000)
#define N88_TIMER_PA    UINT32_C(0xbf100200)
#define N88_VIC_PA      UINT32_C(0xbf200000)
#define N88_VIC_COUNT   3u
#define N88_TIMER_LINE  6u

/*
 * The GPIO pad controller (/arm-io/gpio, `gpio,s5l8920x`, child 0x03000000 ->
 * physical 0x83000000). One 32-bit register per pin, packing function,
 * direction, pull and value; the driver reads a pin's register, ORs in its
 * configuration, and writes it back (AppleS5L8920X+0x17ba/0x17c2). It is
 * modelled as exactly that register file: a write is stored and a read
 * returns what was written, reset zero. That is faithful for the pins the CPU
 * DRIVES -- every access the 10B500 boot makes here is this config
 * read-modify-write -- and it is why the always-zero stub was wrong: it made
 * each read-modify-write read back 0 and lose the pin's own prior bits.
 *
 * It fabricates nothing: with no external device wired to an INPUT pin, a read
 * returns the last value written (0 at reset), not an invented line level. A
 * pin driven by a real peripheral (a chip-select observed by the SPI NOR, a
 * sensor's data line) needs that peripheral wired to this block, which is a
 * later step; until then such a pin reads back its own register, which is the
 * honest "nothing is driving it" answer.
 */
#define N88_GPIO_PA     UINT32_C(0x83000000)
#define N88_GPIO_SIZE   UINT32_C(0x1000)        /* backs offsets 0..0x818 */
#define N88_GPIO_REGS   (N88_GPIO_SIZE / 4u)

/*
 * spi0 and the NOR flash on it (/arm-io/spi0, `spi-1,samsung`, `spi-version
 * 1`, child 0x02000000 -> physical 0x82000000; `interrupts` 29 on /arm-io/vic,
 * which is VIC0 line 29). The controller is the core's Samsung SPI model at
 * version 1 (soc.h); the flash is spi_nor.h's, at its only chip select.
 *
 * The select is not the controller's: spi0's `function-spi_cs0` names GPIO
 * pin 0x1204 (group 0x12, pin 4), whose register is 0x250 in the pad block.
 * AppleSamsungSPI writes 0x12 there before it fills the transmit FIFO and
 * 0x13 after the transfer (10B500), so bit 0 is the pin's level and the
 * select is active low. A store to that register is the flash's select edge.
 *
 * The flash is 1 MiB, erased (all 0xFF) when the machine is made, and it
 * keeps its contents across n88_boot as a real one does across a reboot;
 * n88_nor() gives the caller the array to load or save.
 */
/*
 * The CDMA engine and its AES contexts (cdma.h): /arm-io/cdma's two reg
 * ranges through /arm-io's ranges (+0x80000000). Channel n (1..27) raises
 * the tree's interrupts[n - 1], line 0x2a + n.
 */
#define N88_CDMA_PA       UINT32_C(0x87000000)
#define N88_CDMA_AES_PA   UINT32_C(0x87800000)
#define N88_CDMA_LINE0    0x2au

/*
 * The display controller (m2clcd.h): /arm-io/clcd at 0x85400000, interrupt
 * 0x25. It starts a frame every 1/60 s of guest time; at boot it shows the
 * framebuffer iBoot left (N88_VRAM_PA), and n88_framebuffer() follows its
 * window registers from then on.
 */
#define N88_LCD_PANEL_ID  UINT32_C(0x4e454f4e)   /* "NEON": no real panel's  */

/*
 * The DARTs (s5l8920_dart.h): dart0 at 0xBFE00000 translates for the
 * display controller, dart1 at 0xBFF00000 for the camera blocks. Once the
 * display driver owns the controller its window holds an I/O address
 * (3.1.3: 0x3C0D8000), which n88_framebuffer() follows through dart0.
 */
#define N88_DSIM_PA       UINT32_C(0x89000000)   /* s5l8920_dsim.h         */
#define N88_DART0_PA      UINT32_C(0xbfe00000)
#define N88_DART1_PA      UINT32_C(0xbff00000)
#define N88_CLCD_PA       UINT32_C(0x85400000)
#define N88_CLCD_LINE     0x25u
#define N88_FRAME_HZ      60u

/*
 * The SHA-1 engine (s5l_sha1.h): /arm-io/sha1 at 0x80100000. It takes data
 * only from CDMA, as a peripheral request whose FIFO address is its +0xA0;
 * no other device here takes peripheral requests.
 */
#define N88_SHA1_PA       UINT32_C(0x80100000)

/*
 * The I2C controllers (s5l8920_i2c.h): /arm-io/i2c0 at 0x83200000, line
 * 0x13, and /arm-io/i2c2 at 0x83400000, line 0x11. On i2c0, as the tree
 * places them: the LIS331DL accelerometer (0x1D) and the D1755 PMU (0x74).
 * The other devices there (compass, CS42L61 codec, Mikey, tethered) and
 * i2c2's light sensor do not acknowledge.
 */
#define N88_I2C0_PA       UINT32_C(0x83200000)
#define N88_I2C2_PA       UINT32_C(0x83400000)
#define N88_I2C0_LINE     0x13u
#define N88_I2C2_LINE     0x11u
#define N88_ACCEL_ADDR    0x1du
#define N88_PMU_ADDR      0x74u

#define N88_SPI0_PA       UINT32_C(0x82000000)
#define N88_SPI0_SIZE     UINT32_C(0x1000)
#define N88_SPI0_LINE     29u
#define N88_SPI0_CS_GPIO  UINT32_C(0x250)      /* offset in the pad block  */
#define N88_NOR_SIZE      UINT32_C(0x100000)

/*
 * The touch controller (/arm-io/spi1/multi-touch, "multi-touch,n88", an N1 on
 * spi1 chip select 0) and the board around it, as iPhone OS 3.1.3's
 * AppleMultitouchN1SPI drives it. The device is mtz2.c's: the N1 speaks the
 * Z2's HBPP bootloader and report protocol (one kext, AppleMultitouchSPI,
 * drives both). Its pins, decoded from the tree's function-* entries with the
 * same rule as spi0's select ((group << 3 | pin) << 2 into the pad block):
 * select 0x1300 (pad 0x260, spi1's function-spi_cs0, active low) and reset
 * 0x1401 (pad 0x284). A pad's bits 1:0 = 1x drive bit 0 out; 0x make it an
 * input the board pulls high, which is how the driver releases the reset
 * (0x12, then 0x10). Power is a PMU LDO, taken as on. Its attention line is
 * GPIO interrupt 0xB4 (pin 180, pad 0x2D0). spi1's DMA is CDMA channel 18
 * into TXDATA (+0x10) and 19 out of RXDATA (+0x20).
 *
 * The GPIO interrupt controller is part of the pad block: status at +0x800 +
 * 4g for groups g = 0..6 (interrupt 32g + bit), write one to clear, read and
 * acknowledged that way by AppleS5L8920XGPIOIC (0xc0673b94), which leaves a
 * level-triggered pin's bit alone ((pad & 0xC) == 4). Every pad is set to
 * 0x10 at its start; that bit masks the pin's interrupt (the touch driver
 * writes pad 0x2D0 = 0x21A, then 0x20A to enable it). One VIC line, 0x5E.
 */
#define N88_SPI1_PA          UINT32_C(0x82100000)
#define N88_SPI1_LINE        28u
#define N88_TOUCH_CS_PAD     UINT32_C(0x260)
#define N88_TOUCH_RESET_PAD  UINT32_C(0x284)
#define N88_TOUCH_ATN_IRQ    0xb4u
#define N88_GPIOIC_STATUS    UINT32_C(0x800)
#define N88_GPIOIC_GROUPS    7u
#define N88_GPIOIC_LINE      0x5eu
#define N88_GPIO_IRQ_MASKED  0x10u

/*
 * A pad's bits 3:1 are its mode, as AppleS5L8920XGPIO's configure (0xc0674164)
 * and interrupt setup (0xc0673ff0) write them: 000 input, 001 and 111 output
 * (bit 0 the level driven), 010 interrupt while high, 011 while low, 100 on a
 * rising edge, 101 on a falling one, 110 on both. Bit 0 of an input pad reads
 * the level on the pin (the pin read, 0xc06736f4). The touch attention is
 * active low: pad 0x2D0 = 0x20A is a falling edge.
 *
 * The buttons and the ringer switch (/buttons, AppleM68Buttons) are pins too,
 * from function-button_<name> = <gpio 'GPIO' pin flags>, each interrupting on
 * its pad index (pad / 4). The function reads a pin as is when flags bit 8 is
 * set and inverted when it is clear (0xc06737a8):
 *
 *   hold      pin 0x1607  pad 0x2DC  interrupt 0xB7  pressed = high
 *   menu      pin 0x1606  pad 0x2D8  interrupt 0xB6  pressed = high
 *   volup     pin 0x1600  pad 0x2C0  interrupt 0xB0  pressed = low
 *   voldown   pin 0x1601  pad 0x2C4  interrupt 0xB1  pressed = low
 *   ringerab  pin 0x1403  pad 0x28C  interrupt 0xA3  silent  = low
 *
 * The driver sets all five to interrupt on both edges (0x20C), and on any of
 * those interrupts reads every button (0xc068a1a4), reporting each change as
 * a HID consumer usage (0x30 power, 0x40 menu, 0xE9 and 0xEA volume) or, for
 * the ringer, telephony usage 0x2E, with the function's value inverted once
 * more. While these pins read back their own registers (bit 0 clear) both
 * volume buttons read as held from the first poll, and SpringBoard showed its
 * "Ringer" volume display over the home screen.
 */
#define N88_GPIO_MODE(pad)   (((pad) >> 1) & 7u)

typedef enum {
    N88_INPUT_HOLD,             /* the sleep/wake button, pressed           */
    N88_INPUT_MENU,             /* the home button, pressed                 */
    N88_INPUT_VOLUP,            /* volume up, pressed                       */
    N88_INPUT_VOLDOWN,          /* volume down, pressed                     */
    N88_INPUT_SILENT,           /* the ringer switch at silent              */
    N88_INPUT_COUNT
} n88_input_t;

/*
 * The D1755's real-time clock: a 32-bit count of seconds at PMU registers
 * 0x4C..0x4F, least significant first, which AppleD1755PMU reads (four
 * octets, twice, until both agree: 0xc038aacc); setting the time stores the
 * difference from the count at 0x64..0x67 instead (0xc038ab00). The count
 * runs with guest time from whatever the caller sets (n88_set_rtc); 0 is the
 * Unix epoch, which iPhone OS shows as 31 December 1969 west of Greenwich.
 */
#define N88_PMU_RTC          0x4cu

/*
 * Sleep. iPhone OS 3.1.3 sleeps the whole system a while after the display
 * goes off: IOPMrootDomain -> IOCPUSleepKernel -> cpu_sleep (0xc00606a4),
 * which writes start_cpu's physical address into the exception-vector page
 * at the bottom of DRAM (+0x24; the reset vector there jumps through it with
 * r0 = +0x28) and the eight octets "XSOMPSUS" at +0x80, cleans the caches,
 * and parks the CPU on a branch to itself with interrupts masked
 * (ml_arm_sleep, 0xc00603cc) for the PMU to cut the power. On a 3GS the PMU
 * powers the SoC up again for the hold or menu button, and the boot loader,
 * finding the mark, resumes the kernel through that vector.
 *
 * The model: a CPU parked that way (a branch to itself, IRQ and FIQ masked)
 * is asleep, and n88_run retires nothing while time passes. Pressing hold or
 * menu wakes it: the PMU latches the reason in register 0x01 (bit 1 hold,
 * bit 0 menu: AppleD1755PMU reads 0x01..0x04 into a cache on wake, 0xc0386d66,
 * and AppleM68Buttons reports a press of the button whose
 * function-wake_button_* -- 'STAT' 0x181 and 0x180, cached byte 0, bits 1 and
 * 0 (0xc0385c90) -- reads set), and, if the mark is there, the CPU alone is
 * reset and started at the vector page, its cycle count (time) kept. The
 * devices keep their state, as if retained; the drivers set up what they need
 * on wake. Without the mark the CPU stays parked. The reason is cleared at the
 * next sleep. Nothing else wakes it (not the PMU's alarm).
 *
 * The touch controller's power is a PMU LDO, register 0x11 bit 6: on at
 * reset, as the boot loader leaves it; AppleMultitouchN1SPI turns it off and
 * on around its probe, off when the display goes off and at sleep, and on
 * again at wake, when the flashless part must be bootloaded again.
 */
#define N88_PMU_WAKE_REASON  0x01u
#define N88_PMU_LDO          0x11u
#define N88_PMU_LDO_TOUCH    0x40u
#define N88_SUSPEND_MARK_OFF 0x80u

#define N88_CONSOLE_CAPACITY 65536u
#define N88_DEFAULT_CMDLINE  "debug=0x8 serial=3 -v"
#define N88_ROOT_CMDLINE     "rd=md0 debug=0x8 serial=3 -v"

/*
 * Where the kernel believes the memory disk is: a physical address nothing
 * answers, because the bridge serves every copy from or to it. Just past
 * DRAM, so any disk up to 2.75 GB stays inside 32 bits.
 */
#define N88_MD_TOKEN_PA   UINT32_C(0x50000000)
#define N88_SVC_MD_READ   0xdfe1u       /* SVC #0xe1: copy from the disk */
#define N88_SVC_MD_WRITE  0xdfe2u       /* SVC #0xe2: copy to the disk   */

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
    uint32_t  gpio[N88_GPIO_REGS];  /* the GPIO pad controller's register file */
    s5l_spi_t spi0;
    s5l_spi_t spi1;
    s5l_mtz2_t touch;               /* the N1 on spi1 (mtz2.c)                */
    uint32_t  gpioic_status[N88_GPIOIC_GROUPS];
    bool      input[N88_INPUT_COUNT];   /* buttons pressed, switch at silent */
    /* Each wired pin's level when last sampled, for edges: the touch
     * attention, then the inputs. */
    bool      pin_last[1 + N88_INPUT_COUNT];
    uint32_t  rtc_base;             /* the RTC count at guest time zero       */
    bool      asleep;               /* the CPU parked for the PMU (see above) */
    uint32_t  park_pc;              /* where the last run ended               */
    uint64_t  sleeps, wakes;
    spi_nor_t nor;
    uint8_t  *nor_mem;              /* N88_NOR_SIZE octets, the flash's array */
    cdma_t    cdma;                 /* the DMA engine and AES contexts        */
    m2clcd_t  clcd;                 /* the display controller                 */
    s5l_sha1_t sha1;                /* the SHA-1 engine                       */
    s5l8920_i2c_t i2c0, i2c2;       /* the I2C controllers                    */
    i2c_regfile_t accel;            /* LIS331DL on i2c0                       */
    i2c_regfile_t pmu;              /* D1755 on i2c0                          */
    s5l8920_dart_t dart0, dart1;    /* I/O address translation                */
    s5l8920_dsim_t dsim;            /* the MIPI-DSI master                    */
    uint8_t  *scanout;              /* the screen gathered through dart0      */
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

    /* The root filesystem, when one was attached. */
    bool        has_root;
    uint64_t    root_size;
    md_bridge_t md;

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
    N88_ERR_LAYOUT,             /* the pieces do not fit in DRAM            */
    N88_ERR_ROOT                /* the root filesystem cannot be attached   */
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
    /* Optional root filesystem, whole 4 KiB pages. With it the default
     * boot-args are N88_ROOT_CMDLINE, and the two sites are the kernel
     * virtual addresses where the caller's patch put N88_SVC_MD_READ and
     * N88_SVC_MD_WRITE in place of the strategy routine's physical copies. */
    const vm_block_t  *root;
    uint32_t           md_read_site_pc;
    uint32_t           md_write_site_pc;
    /* boot_args revision byte; 0 means 5, what iOS 6 expects. */
    uint8_t            boot_args_version;
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
 * only on a non-OK CPU status, which `status` carries. Asleep, it retires
 * none and lets `max_steps` cycles of time pass. */
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

/* /arm-io's clock-frequencies as iBoot writes it (see n88.c); for tests. */
extern const uint32_t n88_clock_frequencies[N88_CLOCK_COUNT];

/* The NOR flash's array (N88_NOR_SIZE octets), to load before a boot or save
 * after one. NULL if the machine has none. */
uint8_t *n88_nor(n88_t *m);

/* The framebuffer Boot_Video describes: N88_FB_HEIGHT rows of N88_FB_STRIDE
 * octets at N88_VRAM_PA, 32 bits per pixel. NULL before a boot. */
const uint8_t *n88_framebuffer(const n88_t *m);

/* Press or release a button, or move the ringer switch (on = silent): the
 * pin's level changes, and its interrupt follows the pad's mode. */
void n88_set_input(n88_t *m, n88_input_t in, bool on);

/* True while the system sleeps (see above). */
bool n88_asleep(const n88_t *m);

/* Set the RTC to `seconds` since the Unix epoch, from now on in guest time. */
void     n88_set_rtc(n88_t *m, uint32_t seconds);
uint32_t n88_rtc(const n88_t *m);

/* The bus, for tests and the harness: the same routing the CPU sees. */
uint32_t n88_read32(n88_t *m, uint32_t pa);
void     n88_write32(n88_t *m, uint32_t pa, uint32_t v);

#endif /* NEON_N88_H */
