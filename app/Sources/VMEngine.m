//
//  S5LBox — the emulator run loop. See VMEngine.h.
//
//  THREADING
//
//  One thread owns the machine for its whole lifetime: it allocates it, steps
//  it, and frees it. Nothing else calls into core/ at all. Every few tens of
//  milliseconds that thread takes a mutex and publishes a snapshot — a copy of
//  the guest's framebuffer, whatever the guest printed to the UART, and a
//  couple of counters. The UI takes the same mutex and copies the snapshot out.
//  The lock is therefore held only for two memcpys of a 600 KB buffer, never
//  across interpretation, so the main thread cannot be blocked behind guest
//  execution no matter how slow the guest is.
//
//  MEMORY
//
//  Guest DRAM is 128 MB, matching the hardware. On a 2 GB device that sounds
//  alarming and is not, for one specific reason: core's s5l8900_init() gets it
//  from calloc(), and a request that size goes straight to mmap'd anonymous
//  memory that the kernel fills with zeroes lazily, one page at a time, on
//  first touch. Untouched guest RAM is address space, not footprint. This demo
//  guest touches its one code page and the 600 KB framebuffer, so the resident
//  cost is well under a megabyte of the 128.
//
//  That is a claim about the allocator, not a measurement, so the app measures
//  it: physFootprintBytes reads phys_footprint from TASK_VM_INFO, which is the
//  exact counter jetsam compares against the per-process limit, and the app
//  prints the before/after delta on screen. Do not trust the paragraph above
//  over the number on the phone.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMEngine.h"
#import "VMTouchQueue.h"
#import "VMButtonQueue.h"
#import <dispatch/dispatch.h>
#import "VMFirmwareBoot.h"
#import "VMInstancePaths.h"
#import "VMInstanceStore.h"
#import "VMSettings.h"
#import "VMFramePublication.h"
#import "VMAudioOutput.h"
#import "arm_ci.h"
#import "guest_profile.h"
#import "VMDriverDump.h"
#import "ksyms.h"
#import "rootfs_work.h"

#import <CommonCrypto/CommonDigest.h>
#import <mach/mach.h>
#import <pthread.h>
#import <time.h>
#import <unistd.h>
#import <stdio.h>
#import <stdlib.h>
#import <string.h>

// How many instructions to interpret between checks of the stop/pause flags.
// At a few million instructions a second this is roughly 15-30 ms, which keeps
// pause latency short without paying the flag check too often.
static const unsigned kVMChunkInstructions = 100000;
/* Guest profile tables: 128 Ki (pc, process) slots and 32 Ki distinct call
 * stacks (about 2 MiB); a quarter of each is kept free. */
static const unsigned kVMProfileSlotsLog2 = 17u;
static const unsigned kVMProfileStacksLog2 = 15u;

// Publish a snapshot at most this often. The UI redraws at 30 Hz; going faster
// would only copy the same pixels twice.
static const double kVMPublishInterval = 1.0 / 30.0;

// Cap on UART bytes held for the UI. The view controller drains this every
// frame and keeps the real scrollback; this bound only matters if the UI
// stops draining (backgrounded, say) while the guest keeps printing.
static const NSUInteger kVMConsoleLimit = 16000;

/*
 * Automatic resume must never serialize a UIKit press whose matching release
 * lives only in a host queue. Give the guest far more than the known physical
 * floors (0.5 s and 8 M retirements for Power) to consume the cancellation,
 * but fail the save instead of displaying "Saving" forever if its input driver
 * is genuinely wedged. Either independent bound may stop the attempt; the
 * retirement bound is the fallback if the monotonic host clock is unavailable.
 */
static const uint64_t kVMCheckpointInputTimeoutNS = UINT64_C(10000000000);
static const uint64_t kVMCheckpointInputTimeoutInstructions = UINT64_C(128000000);

/*
 * THE ONE PLACE THE APP'S TWO BUTTON ENUMS MEET.
 *
 * VMButton (VMEngine.h) is Objective-C and needs Foundation; VM_BUTTON_*
 * (VMButtonQueue.h) is plain C so a host CI runner can test the mapping table
 * without an Apple toolchain. They are two spellings of one order and they must
 * agree by value, or -setButton:pressed:'s cast quietly sends the wrong key.
 * Neither header can check that on its own. This file can, and does, here —
 * on the macOS build, which is the only build where both exist.
 */
_Static_assert((NSUInteger)VMButtonHome         == VM_BUTTON_HOME,          "VMButton order");
_Static_assert((NSUInteger)VMButtonPower        == VM_BUTTON_POWER,         "VMButton order");
_Static_assert((NSUInteger)VMButtonVolumeUp     == VM_BUTTON_VOLUME_UP,     "VMButton order");
_Static_assert((NSUInteger)VMButtonVolumeDown   == VM_BUTTON_VOLUME_DOWN,   "VMButton order");
_Static_assert((NSUInteger)VMButtonRingerSilent == VM_BUTTON_RINGER_SILENT, "VMButton order");
_Static_assert((NSUInteger)VMButtonCount        == VM_BUTTON_COUNT,         "VMButton count");

typedef NS_ENUM(uint8_t, VMEngineState) {
    VMEngineStateIdle = 0,
    VMEngineStateStarting,
    VMEngineStateRunning,
    VMEngineStateCheckpointing,
    VMEngineStateStopping,
};

static double vm_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t vm_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 ||
        (uint64_t)ts.tv_sec > UINT64_MAX / UINT64_C(1000000000))
        return 0u;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

// Declared up front so every call below is checked against a prototype.
@interface VMEngine ()
- (void)threadMain:(id)unused;
- (void)publishRetired:(uint64_t)retired
                  rate:(double)instantRate
                status:(arm_status_t)status;
- (void)appendConsole:(NSString *)text;
- (void)noteDiscardedInput;
- (void)noteDroppedTouch;
- (void)noteDroppedButton;
- (void)drainOneTouch_emulatorThread;
- (void)drainOneButton_emulatorThread;
- (void)publishDiagnostics_emulatorThread:(uint64_t)retired
                                    runNs:(uint64_t)runNs
                                   idleNs:(uint64_t)idleNs
                                 threadNs:(uint64_t)threadNs;
- (void)beginCheckpointInputQuiesce_emulatorThread;
- (BOOL)checkpointInputIsQuiescent_emulatorThread;
- (void)publishBlankSnapshotLocked;
- (BOOL)installGuestPayload;
- (void)provisionRootFilesystem:(id)unused;
- (BOOL)resolveFilesInto:(vm_instance_paths_t *)paths note:(NSString **)note;
- (NSUInteger)copyOptionValuesInto:(bool *)values capacity:(NSUInteger)capacity;
- (void)pushAudioWord_emulatorThread:(uint32_t)word;
@end

/*
 * A sampled signature of the frame, not a complete hash.
 *
 * Hashing all 460,800 bytes at up to 60 Hz would be ~27 MB/s of pure
 * measurement overhead on the very phone whose speed is in question, which
 * would make the counter change the number it reports. The 397-byte stride is
 * coprime with the 1,280-byte row pitch, so successive samples walk across
 * rows rather than re-reading one column of every row.
 *
 * Sampling can only ever MISS a change, never invent one, so this undercounts
 * frames that differ in fewer than ~1,160 sampled words -- a cursor-sized
 * change on an otherwise still screen. It is accurate for the animations the
 * 30fps target is about, and the direction of its error is known.
 */
static uint64_t vm_engine_fb_signature(const uint8_t *fb, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i + 4u <= n; i += 397u) {
        uint32_t w = (uint32_t)fb[i] | ((uint32_t)fb[i + 1] << 8) |
                     ((uint32_t)fb[i + 2] << 16) | ((uint32_t)fb[i + 3] << 24);
        h = (h ^ (uint64_t)w) * 1099511628211ull;
    }
    return h;
}

static double vm_engine_now_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void vm_audio_tx_callback(void *ctx, uint32_t word) {
    VMEngine *engine = (__bridge VMEngine *)ctx;
    [engine pushAudioWord_emulatorThread:word];
}

@implementation VMEngine {
    s5l8900_t        _machine;
    /* The provisioning copy's byte counters; see -rootFilesystemProgress. */
    uint64_t         _prepareDone;
    uint64_t         _prepareTotal;
    BOOL             _machineReady;

    /*
     * WHICH GUEST IS RUNNING, and why it is not the other one.
     *
     * The app has two payloads now: Apple's kernel, and the synthetic guest in
     * VMGuest.c. Which one it got is the first thing a user needs to know and
     * the easiest thing for a UI to be wrong about, so it is recorded here as
     * a string the engine sets from what actually happened rather than as a
     * flag the UI infers. -modeDescription is the only way to read it, and
     * there is no path that sets it to a firmware string without
     * vm_firmware_boot_start() having returned true.
     *
     * _bringUpNote carries the reason the firmware path was NOT taken, when
     * there was firmware to take it with. Empty means "nothing to explain".
     */
    vm_firmware_boot_t *_firmwareBoot;   // owns the work image + bridge storage
    NSString        *_mode;
    NSString        *_bringUpNote;
    BOOL             _preparingRootFS;
    /*
     * WHICH MACHINE THIS ENGINE IS. Set once at construction and never
     * changed: it names the directory this machine's writable root filesystem
     * lives in, and a machine that changed identity mid-run would be a machine
     * that changed disks mid-run. nil means nobody said, which is a
     * configuration this class refuses to guess about -- see -resolveFilesInto:.
     */
    NSString        *_instanceID;

    NSThread        *_thread;
    pthread_mutex_t  _lock;
    BOOL             _lockReady;

    // Everything below is guarded by _lock.
    uint8_t         *_snapshot;      // VM_FB_BYTES, last published frame
    BOOL             _snapshotFresh;
    BOOL             _snapshotARGB;  // byte order of the snapshot's pixels
    BOOL             _snapshotBlank;
    /* Frame rate, measured rather than assumed.
     *
     * The goal for this project is "30fps+", and until now the app could not
     * report it: the status line showed M insn/s, which is not a frame rate
     * and cannot be converted into one without knowing the per-frame cost.
     * Every fps figure quoted so far has been extrapolated on a desktop.
     *
     * What is counted is a CHANGED published frame, not a publish and not a
     * guest composite. That is deliberately the user-visible quantity: this
     * guest composites into a continuously scanned surface and never writes
     * CLCD_UPDATE, so there is no register edge to count, and publishing an
     * identical frame is not a frame anyone can see.
     */
    uint64_t         _fbSignature;
    BOOL             _fbSignatureValid;
    uint64_t         _fpsWindowFrames;
    double           _fpsWindowStart;
    double           _fps;
    /* The geometry the display controller was scanning out when the snapshot
     * was taken. Published with the pixels rather than assumed by the reader:
     * the buffer is a fixed VM_FB_BYTES, but what is IN it is whatever window
     * the guest enabled. */
    uint32_t         _snapshotWidth;
    uint32_t         _snapshotHeight;
    uint32_t         _snapshotStride;
    NSMutableString *_pending;
    uint64_t         _retired;
    double           _rate;          // instructions per second, smoothed
    NSString        *_status;
    VMEngineState    _state;
    BOOL             _stopRequested;
    VMEngineStopCompletion _stopCompletion;
    BOOL             _checkpointRequested;
    VMEngineCheckpointCompletion _checkpointCompletion;
    BOOL             _paused;
    NSString        *_pauseReason;
    uint64_t         _instructionCap; // 0: run until stopped or halted
    BOOL             _buttons[VMButtonCount];
    BOOL             _discardedInputLogged;
    BOOL             _droppedTouchLogged;
    /*
     * Touch reports waiting to be handed to the Z2.
     *
     * The UI thread must not touch the machine — it runs at whatever rate
     * UIKit delivers events, and s5l8900_run() is executing on the emulator
     * thread the whole time. So a report is enqueued here under _lock and
     * applied between chunks by threadMain, which is the only place the
     * machine is ever poked.
     *
     * The container and its drop policy live in VMTouchQueue.c, in plain C,
     * because "which report gets thrown away when the queue is full" is the
     * part of this that can be wrong without anything reporting an error —
     * and the part a host CI runner can test.
     */
    vm_touch_queue_t _touch;
    vm_touch_delivery_state_t _touchDelivery;
    uint64_t         _touchDelivered;
    /*
     * Button transitions waiting to be handed to the board, for exactly the
     * same reason and with a stricter rule: nothing here may ever be coalesced
     * away, because a press and its release are two edges and dropping either
     * leaves the guest holding a key nobody is pressing. See VMButtonQueue.h.
     */
    vm_button_queue_t _buttonQueue;
    uint64_t          _buttonDelivered;
    uint64_t          _buttonRefused;
    vm_button_power_hold_t _powerHold;
    vm_button_momentary_holds_t _momentaryHolds;
    BOOL              _droppedButtonLogged;
    VMAudioOutput    *_audioOutput;
    /*
     * Diagnostics copied by the emulator thread between chunks (the only time
     * it may read the machine) and formatted on request by the UI thread,
     * both under _lock: the cached interpreter's counters since this run
     * started, and the machine's recent unmodelled hardware accesses.
     */
    BOOL                    _diagCiActive;
    arm_ci_stats_t          _diagCiStats;
    uint64_t                _diagRetired;
    s5l_access_entry_t _diagUnmodelled[S5L_ACCESS_LOG];
    s5l_access_entry_t _diagMmio[S5L_ACCESS_LOG];
    /* Where the emulator thread's time went since this run started: inside
     * s5l8900_run, of which asleep in guest idle (paced WFI), and in total. */
    uint64_t                _diagRunNs, _diagIdleNs, _diagThreadNs;
    /* The machine's permanent record of kernel pcs that touched the audio
     * block (s5l8900_t::audio_pc_lo/hi), for -audioDriverExcerpt. */
    uint32_t                _diagAudioPcLo, _diagAudioPcHi;
    uint64_t                _diagAudioAccesses;
    /* The PCM output path the same way (s5l8900_t::pcm_*), plus the state of
     * the two DMA controllers and I2S windows that carry it. */
    s5l_access_entry_t      _diagPcm[S5L_ACCESS_LOG];
    uint32_t                _diagPcmPcLo, _diagPcmPcHi;
    uint64_t                _diagPcmAccesses;
    s5l_pl080_t             _diagDmac[S5L8900_DMAC_COUNT];
    s5l_i2s_t               _diagI2s[S5L8900_I2S_COUNT];
    s5l_wm8991_t            _diagCodec;
    NSString               *_diagBackendNote;  /* why the cached interpreter is off */
    /*
     * The guest profile (guest_profile.h): the guest pc after every full
     * chunk, i.e. one sample per kVMChunkInstructions retired. Its own lock,
     * so a sample never waits on the UI formatting the status line; taken by
     * the emulator thread a few thousand times a second, uncontended.
     * _profileShort counts chunks that ended early (guest idle, a stop) and
     * so were not sampled.
     */
    pthread_mutex_t         _profileLock;
    BOOL                    _profileReady;
    gprof_t                 _profile;
    uint64_t                _profileShort;
    uint64_t                _profileSinceNs;
}

+ (uint64_t)physFootprintBytes {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO,
                                 (task_info_t)&info, &count);
    if (kr != KERN_SUCCESS) return 0;
    return (uint64_t)info.phys_footprint;
}

- (instancetype)init {
    return [self initWithInstanceID:nil];
}

- (instancetype)initWithInstanceID:(NSString *)identifier {
    self = [super init];
    if (!self) return nil;
    if (pthread_mutex_init(&_lock, NULL) != 0) return nil;
    _lockReady = YES;
    _pending = [NSMutableString string];
    if (!_pending) return nil;
    _status  = @"idle";
    _mode    = @"";
    _bringUpNote = @"";
    _instanceID = [identifier copy];
    if (pthread_mutex_init(&_profileLock, NULL) == 0) {
        _profileReady = gprof_init(&_profile, kVMProfileSlotsLog2) &&
                        gprof_init_stacks(&_profile, kVMProfileStacksLog2);
        if (!_profileReady) {
            gprof_free(&_profile);
            pthread_mutex_destroy(&_profileLock);
        }
    }
    _profileSinceNs = vm_now_ns();
    return self;
}

- (void)dealloc {
    // Safe without any handshake: NSThread holds a strong reference to its
    // target for as long as the thread is alive, so -dealloc cannot possibly
    // run while -threadMain is still using the snapshot buffer or the lock.
    [_audioOutput stop];
    _audioOutput = nil;
    free(_snapshot);
    if (_profileReady) {
        gprof_free(&_profile);
        pthread_mutex_destroy(&_profileLock);
    }
    if (_lockReady) pthread_mutex_destroy(&_lock);
}

- (void)pushAudioWord_emulatorThread:(uint32_t)word {
    if (_audioOutput) {
        [_audioOutput pushSampleWord:word];
    }
}

#pragma mark - Choosing a guest

/*
 * THE ONE DECISION: Apple's kernel, or the built-in test guest.
 *
 * Called on the starting thread with the machine already allocated and nothing
 * else touching it. All the real work is in app/Sources/VMFirmwareBoot.c, which
 * is plain C and is tested by core/tests -- this method is only the policy of
 * WHEN to try, and what to say when it does not work.
 *
 * THE RULES IT ENFORCES, in order of how badly getting them wrong would hurt:
 *
 *  1. The firmware path is taken only when vm_firmware_boot_start() returns
 *     true. There is no other assignment of a firmware string to _mode.
 *  2. A firmware failure is NEVER silent. _bringUpNote gets the reason,
 *     -statusLine carries it, and the console gets the full detail. The demo
 *     guest then runs, and _mode says it is the demo guest.
 *  3. A failed bring-up may have written megabytes of kernel into DRAM, so the
 *     machine is torn down and rebuilt before the demo guest is installed
 *     rather than layered on top of a half-loaded kernel.
 *
 * WHERE IT LOOKS. Two directories, not one, and the split is what makes two
 * machines two machines: the three imported artefacts are shared and read-only
 * (Documents/firmware, where VMFirmwareImporter writes), and the writable work
 * image belongs to THIS machine alone (Machines/<id>). VMInstancePaths.c owns
 * that derivation, including what happens to the single shared work image an
 * older build left behind. An engine with no instance id refuses the firmware
 * path outright rather than guessing at a directory -- see -resolveFilesInto:.
 */
- (BOOL)installGuestPayload {
    vm_instance_paths_t paths;
    NSString *note = nil;
    if (![self resolveFilesInto:&paths note:&note]) {
        if (!vm_guest_install(&_machine)) return NO;
        pthread_mutex_lock(&_lock);
        _mode = @"built-in test guest";
        _bringUpNote = note ?: @"";
        pthread_mutex_unlock(&_lock);
        return YES;
    }

    vm_firmware_boot_paths_t boot_paths;
    vm_instance_paths_to_boot(&paths, &boot_paths);

    /*
     * ADOPTION, before the probe rather than after it: a machine that can take
     * over the pre-instance work image IS ready, and probing first would send
     * it down the 450 MB provisioning path with a perfectly good disk sitting
     * unused. The move is a rename, so it costs nothing and cannot run long
     * enough to matter on this thread.
     */
    NSString *adoptionProblem = nil;
    if (vm_instance_work_plan(&paths) == VM_INSTANCE_WORK_ADOPT) {
        char detail[VM_FW_BOOT_DETAIL_CAPACITY];
        BOOL adopted = vm_instance_work_adopt(&paths, detail, sizeof detail);
        NSString *said = [NSString stringWithUTF8String:detail];
        [self appendConsole:[NSString stringWithFormat:@"[vm] %@\n",
            said ?: (adopted ? @"adopted the shared root filesystem"
                             : @"could not adopt the shared root filesystem")]];
        /*
         * A FAILED ADOPTION IS NEVER SILENT, and it is kept separately from
         * `note` because the next thing that happens is the branch below
         * overwriting `note` with "preparing the root filesystem" -- which is
         * true, and which would replace "we could not move the 450 MB disk you
         * already had" with a sentence that sounds like everything is fine.
         */
        if (!adopted) adoptionProblem =
            said.length ? said
                        : @"The root filesystem this machine could have "
                          @"adopted could not be moved.";
    }

    /* The switches, resolved by the same C the tests run. */
    bool values[VM_BOOT_OPTION_MAX];
    NSUInteger count = [self copyOptionValuesInto:values
                                         capacity:VM_BOOT_OPTION_MAX];

    vm_firmware_boot_state_t state;
    vm_firmware_boot_probe(&boot_paths, &state);

    if (state.readiness == VM_FW_BOOT_READY) {
        vm_firmware_boot_destroy(&_firmwareBoot);
        _firmwareBoot = vm_firmware_boot_create();
        if (_firmwareBoot) {
            vm_firmware_boot_report_t report;
            if (vm_firmware_boot_start(_firmwareBoot, &_machine, &boot_paths,
                                       values, (unsigned)count, &report)) {
                [self appendConsole:[NSString stringWithFormat:
                    @"[vm] Apple firmware: kernel at pa 0x%08x, device tree at "
                    @"0x%08x (%u bytes), boot_args at 0x%08x\n"
                    @"[vm] topOfKernelData 0x%08x, /vram pool 0x%08x+0x%x, "
                    @"free page pool %.1f MB\n"
                    @"[vm] boot arguments: \"%s\"\n",
                    report.bringup.entry_pa, report.bringup.devicetree_pa,
                    report.bringup.devicetree_size, report.bringup.boot_args_pa,
                    report.bringup.top_of_kernel_data_pa,
                    report.bringup.framebuffer_pa, report.bringup.vram_bytes,
                    report.bringup.free_pool_bytes / 1048576.0,
                    report.bringup.cmdline]];
                /* -stringWithUTF8String: rather than the @() boxing syntax:
                 * these are fixed char arrays, and the explicit form has no
                 * decay question in it. It returns nil for bytes that are not
                 * valid UTF-8, so neither result is used unguarded. */
                NSString *summary =
                    [NSString stringWithUTF8String:report.summary];
                /*
                 * A SUCCESSFUL BOOT STILL OWES THE USER THIS. Twelve of the
                 * fourteen switches do not reach the request, and six of them
                 * disagree with what the machine did on an installation
                 * nobody has touched. Saying nothing here would be the exact
                 * failure this class was built to end -- the settings screen
                 * showing one configuration and the machine running another.
                 */
                NSString *switches =
                    [NSString stringWithUTF8String:report.options.summary];
                if (switches.length)
                    [self appendConsole:[NSString stringWithFormat:
                        @"[vm] settings: %@\n", switches]];
                /*
                 * WHICH NUBS THIS BOOT HID, by name and from the list bring-up
                 * was actually handed -- not from the settings table, which is
                 * what the user set rather than what the guest got.
                 *
                 * This exists because on 2026-07-29 a phone log could not
                 * answer "is this the build with the touchscreen fix?". The
                 * summary above lists only switches whose outcome DISAGREES
                 * with the request, so a nub working exactly as asked appeared
                 * nowhere at all, and an old binary and a new one printed the
                 * same lines. A boot that hides a device should say which.
                 */
                if (report.options.unmatch_count) {
                    NSMutableString *nubs = [NSMutableString string];
                    for (unsigned i = 0; i < report.options.unmatch_count; i++) {
                        const char *path = report.options.unmatch[i];
                        if (!path) continue;
                        if (nubs.length) [nubs appendString:@", "];
                        [nubs appendString:
                            [NSString stringWithUTF8String:path] ?: @"?"];
                    }
                    [self appendConsole:[NSString stringWithFormat:
                        @"[vm] device tree: %u nub(s) hidden from the guest: "
                        @"%@\n", report.options.unmatch_count, nubs]];
                }
                /* Actionable, not merely alarming: the settings screen is
                 * where each of these now says what happens instead. */
                NSString *said = switches.length
                    ? [switches stringByAppendingString:
                        @" Each one says what the machine does instead, under "
                        @"its own switch in Settings."]
                    : nil;
                pthread_mutex_lock(&_lock);
                _mode = summary ?: @"Apple firmware";
                _bringUpNote = said ?: (note ?: @"");
                pthread_mutex_unlock(&_lock);
                return YES;
            }
            note = [NSString stringWithUTF8String:report.detail]
                 ?: @"Apple's kernel could not be started.";
            [self appendConsole:[NSString stringWithFormat:
                @"[vm] FIRMWARE BOOT FAILED: %s\n"
                @"[vm] falling back to the built-in test guest\n",
                report.detail]];
            vm_firmware_boot_destroy(&_firmwareBoot);
            /* A partially-loaded kernel is not a blank machine. */
            s5l8900_free(&_machine);
            if (!s5l8900_init(&_machine, VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE))
                return NO;
            [self configureCpuBackend];
        } else {
            note = @"Not enough memory to start Apple's kernel.";
        }
    } else if (state.readiness == VM_FW_BOOT_NEEDS_WORK_IMAGE) {
        /*
         * The three imported files are here but the writable root filesystem
         * has not been made. That copy is ~450 MB and takes long enough to be
         * killed by the watchdog if it ran here, so it runs on its own thread
         * and this machine gets the demo guest. Reopening the machine after it
         * finishes boots the real kernel.
         */
        /* Says what the bar means and what happens next, and NOTHING MORE.
         * A first draft of this string promised "iPhone OS starts by itself
         * when the copy finishes"; it does not -- the completion path below
         * says "Reopen it to boot iPhone OS" -- and a message that tells a
         * user to wait for something that never happens is worse than the
         * vague one it replaced. */
        note = @"Preparing this machine's writable root filesystem — first "
               @"boot only. The bar on the screen is the real copy progress "
               @"and keeps going if you leave. Reopen this machine when it "
               @"reaches the end.";
        BOOL alreadyRunning;
        pthread_mutex_lock(&_lock);
        alreadyRunning = _preparingRootFS;
        _preparingRootFS = YES;
        pthread_mutex_unlock(&_lock);
        if (!alreadyRunning) {
            /* Two strings rather than the C struct: NSThread wants an object,
             * and the worker rebuilds the struct itself so nothing on the
             * emulator side has to outlive this stack frame.
             * -stringWithUTF8String: rather than @(): these are fixed char
             * arrays and the explicit form has no decay question in it, and it
             * returns nil for bytes that are not valid UTF-8 -- which a
             * dictionary literal would turn into a crash, so it is checked. */
            NSString *firmwareDir =
                [NSString stringWithUTF8String:boot_paths.firmware];
            NSString *workDir = [NSString stringWithUTF8String:boot_paths.work];
            NSDictionary *where = (firmwareDir && workDir)
                ? @{ @"firmware": firmwareDir, @"work": workDir } : nil;
            NSThread *worker = where ? [[NSThread alloc]
                initWithTarget:self
                      selector:@selector(provisionRootFilesystem:)
                        object:where] : nil;
            if (worker) {
                worker.name = @"S5LBox rootfs provisioning";
                worker.qualityOfService = NSQualityOfServiceUtility;
                [worker start];
            } else {
                pthread_mutex_lock(&_lock);
                _preparingRootFS = NO;
                pthread_mutex_unlock(&_lock);
                note = @"Could not start preparing the root filesystem.";
            }
        }
    } else {
        /* No firmware at all is the ordinary case, not a failure: the demo
         * guest is what this app does for anyone who has imported nothing. */
        note = @"";
    }

    if (!vm_guest_install(&_machine)) return NO;

    /* The adoption failure leads, because it is about a file the user already
     * had and everything else here is about one the app is making. */
    if (adoptionProblem.length)
        note = note.length
            ? [adoptionProblem stringByAppendingFormat:@" %@", note]
            : adoptionProblem;

    pthread_mutex_lock(&_lock);
    _mode = @"built-in test guest";
    _bringUpNote = note ?: @"";
    pthread_mutex_unlock(&_lock);
    return YES;
}

/*
 * The one slow step, on its own thread. rootfs_work_create() refuses to
 * replace an existing destination, so a second start while this is running
 * cannot corrupt the image being written -- it is the flag, not the file, that
 * keeps two of these from being launched.
 */
- (void)provisionRootFilesystem:(id)where {
    @autoreleasepool {
        NSDictionary *dirs = (NSDictionary *)where;
        char detail[VM_FW_BOOT_DETAIL_CAPACITY];
        vm_firmware_boot_paths_t paths;
        detail[0] = '\0';

        NSString *firmware = dirs[@"firmware"];
        NSString *work = dirs[@"work"];
        char firmwarePath[VM_FW_BOOT_PATH_CAPACITY];
        char workPath[VM_FW_BOOT_PATH_CAPACITY];
        BOOL havePaths =
            firmware.length && work.length &&
            [firmware getFileSystemRepresentation:firmwarePath
                                        maxLength:sizeof firmwarePath] &&
            [work getFileSystemRepresentation:workPath
                                    maxLength:sizeof workPath] &&
            vm_firmware_boot_paths_split(&paths, firmwarePath, workPath);

        /*
         * The switches are read HERE, not carried from the start, because this
         * is the moment their value is written into the image: the QuartzCore
         * software-renderer rewrite is a property of the file from now on, and
         * a value captured a second earlier would be no more accurate and much
         * easier to get wrong.
         */
        bool values[VM_BOOT_OPTION_MAX];
        NSUInteger count = [self copyOptionValuesInto:values
                                             capacity:VM_BOOT_OPTION_MAX];

        BOOL ok = havePaths
            ? vm_firmware_boot_provision(&paths, values, (unsigned)count,
                                         vm_engine_prepare_progress,
                                         (__bridge void *)self,
                                         detail, sizeof detail)
            : NO;
        if (!havePaths)
            (void)snprintf(detail, sizeof detail,
                           "This app has nowhere to prepare this machine's "
                           "root filesystem.");

        /*
         * A REFUSAL IS NOT ALWAYS A FAILURE. rootfs_work_create() will not
         * replace an existing destination, which is the behaviour that keeps a
         * second run from discarding the guest's writes -- but it also means
         * that a machine which was provisioned by another engine (a Reset
         * relaunches one, and its _preparingRootFS flag is a per-engine ivar)
         * gets told "could not prepare the root filesystem" about a filesystem
         * it now has. Ask the disk rather than the return value: if the work
         * image is there, this machine is ready, whoever made it.
         */
        if (!ok && havePaths) {
            vm_firmware_boot_state_t after;
            vm_firmware_boot_probe(&paths, &after);
            if (after.work_present) {
                ok = YES;
                (void)snprintf(detail, sizeof detail,
                               "The root filesystem was already prepared.");
            }
        }
        NSString *said = [NSString stringWithUTF8String:detail];
        pthread_mutex_lock(&_lock);
        _preparingRootFS = NO;
        _bringUpNote = ok
            ? [NSString stringWithFormat:
                @"This machine's root filesystem is ready. Reopen it to boot "
                @"iPhone OS. %@", said ?: @""]
            : (said ?: @"The root filesystem could not be prepared.");
        pthread_mutex_unlock(&_lock);
        /* Two literal format strings rather than one chosen by a ternary: a
         * non-literal format is unchecked by the compiler, and this one is
         * built from a fixed char array whose contents come from four
         * different refusal paths. */
        [self appendConsole:ok
            ? [NSString stringWithFormat:
                @"[vm] root filesystem prepared; reopen to boot iPhone OS "
                @"(%s)\n", detail]
            : [NSString stringWithFormat:
                @"[vm] could not prepare the root filesystem: %s\n", detail]];
    }
}

/*
 * WHICH FILES THIS MACHINE USES, or why it cannot have any.
 *
 * Everything that can be wrong here -- an identifier that is not sixteen hex
 * digits, a path too long to hold, the pre-instance work image -- is decided
 * in VMInstancePaths.c, which a host runner tests. This method is the two
 * lookups Objective-C is needed for and nothing else.
 *
 * Returns NO, with a sentence in `note`, when the firmware path is not
 * available. That is not always a failure: no instance id is the ordinary case
 * for a machine opened by something other than the machine list, and no
 * firmware imported is the ordinary case full stop.
 */
- (BOOL)resolveFilesInto:(vm_instance_paths_t *)paths note:(NSString **)note {
    if (note) *note = @"";
    if (!paths) return NO;

    NSString *firmware = [[VMSettings sharedSettings] firmwareDirectory];
    if (!firmware.length) {
        if (note) *note = @"This app has no documents directory to look for "
                          @"firmware in.";
        return NO;
    }

    if (!_instanceID.length) {
        /*
         * REFUSED RATHER THAN GUESSED. The alternative -- falling back to a
         * shared work image in the firmware directory -- is exactly the
         * behaviour that made every machine one machine, and it would come
         * back silently the first time something constructed an engine
         * without an identifier. The demo guest runs and says why.
         */
        if (note) *note = @"This machine has no identity, so it has no root "
                          @"filesystem of its own. Open it from the machine "
                          @"list to boot iPhone OS.";
        return NO;
    }

    /*
     * Called for its SIDE EFFECT as much as its value: it CREATES this
     * machine's directory, and C cannot portably mkdir, so both the adoption
     * rename and the provisioner depend on this having happened first.
     *
     * The C then rebuilds the same path from the container and the identifier.
     * That is one derivation stated twice, and it is deliberate: the C one is
     * the one that can be tested, and it is also the one that refuses an
     * identifier that is not sixteen hex digits. They agree because both are
     * <container>/<id> and the identifier contains no separator.
     */
    NSString *mine =
        [[VMInstanceStore sharedStore] directoryForInstanceWithID:_instanceID];
    NSString *machines = [[VMInstanceStore sharedStore] machinesDirectory];
    if (!mine.length) {
        if (note) *note = @"This machine has nowhere on disk to keep its root "
                          @"filesystem.";
        return NO;
    }

    char firmwarePath[VM_FW_BOOT_PATH_CAPACITY];
    char machinesPath[VM_FW_BOOT_PATH_CAPACITY];
    if (![firmware getFileSystemRepresentation:firmwarePath
                                     maxLength:sizeof firmwarePath] ||
        !machines.length ||
        ![machines getFileSystemRepresentation:machinesPath
                                     maxLength:sizeof machinesPath]) {
        if (note) *note = @"This machine's files are somewhere this app cannot "
                          @"name.";
        return NO;
    }

    vm_instance_paths_status_t s =
        vm_instance_paths_derive(firmwarePath, machinesPath,
                                 _instanceID.UTF8String, paths);
    if (s != VM_INSTANCE_PATHS_OK) {
        if (note)
            *note = [NSString stringWithFormat:
                @"This machine's files cannot be located: %s.",
                vm_instance_paths_status_text(s)];
        return NO;
    }
    return YES;
}

/*
 * The marker VMFirmwareBoot.c's boot() reads as `paths->work/engine.interpreter`
 * (VM_FW_BOOT_INTERPRETER_FILE). Resolved the same way resolveFilesInto: finds
 * this machine's own directory, since join_path() itself is private to that
 * file and this is a small enough join not to warrant exporting it.
 */
- (NSString *)forcedInterpreterMarkerPath {
    if (!_instanceID.length) return nil;
    NSString *mine =
        [[VMInstanceStore sharedStore] directoryForInstanceWithID:_instanceID];
    if (!mine.length) return nil;
    return [mine stringByAppendingPathComponent:@VM_FW_BOOT_INTERPRETER_FILE];
}

- (BOOL)isForcedInterpreterEnabled {
    NSString *path = [self forcedInterpreterMarkerPath];
    if (!path) return NO;
    NSDictionary *attrs =
        [[NSFileManager defaultManager] attributesOfItemAtPath:path error:NULL];
    /* file_size() on the C side treats "exists but empty" as OFF, so this
     * agrees with the reader rather than merely checking existence. */
    return ((NSNumber *)attrs[NSFileSize]).unsignedLongLongValue > 0ull;
}

- (BOOL)setForcedInterpreterEnabled:(BOOL)enabled {
    NSString *path = [self forcedInterpreterMarkerPath];
    if (!path) return NO;
    NSFileManager *fm = [NSFileManager defaultManager];
    if (!enabled) {
        if (![fm fileExistsAtPath:path]) return YES;
        return [fm removeItemAtPath:path error:NULL];
    }
    /* Content is never read, only the size -- one byte is enough to be
     * non-empty and cheap enough that this can never meaningfully fail for
     * being too large. */
    return [[NSData dataWithBytes:"1" length:1]
        writeToFile:path options:NSDataWritingAtomic error:NULL];
}

/*
 * The settings screen's values, in option-table order.
 *
 * READ FROM VMSettings, which is the store the settings screen writes.
 * VMInstanceStore also carries a per-instance option array, but nothing in the
 * app writes it yet, so reading it here would take the switches back to doing
 * nothing -- this time invisibly. When the settings screen becomes
 * per-machine, this is the one method that changes.
 */
- (NSUInteger)copyOptionValuesInto:(bool *)values capacity:(NSUInteger)capacity {
    if (!values || capacity == 0) return 0;
    NSUInteger count = vm_option_count();
    if (count > capacity) count = capacity;
    VMSettings *settings = [VMSettings sharedSettings];
    for (NSUInteger i = 0; i < count; i++)
        values[i] = [settings valueForOptionIndex:i] ? true : false;
    return count;
}

- (void)configureCpuBackend {
    /* Through VMSettings, so an unset preference gets its default there. */
    NSString *backendPref = [[VMSettings sharedSettings] cpuBackend];
    s5l8900_cpu_backend_t backend = S5L8900_CPU_BACKEND_INTERPRETER;
    BOOL forced = [self isForcedInterpreterEnabled];
    BOOL wantsCached = [backendPref isEqualToString:@"cached"] ||
                       [backendPref isEqualToString:@"block"] ||
                       [backendPref isEqualToString:@"cached-block"] ||
                       [backendPref isEqualToString:@"ir"] ||
                       [backendPref isEqualToString:@"micro-op"] ||
                       [backendPref isEqualToString:@"jit"];
    NSString *why;
    if (forced) {
        backend = S5L8900_CPU_BACKEND_INTERPRETER;
        why = wantsCached
            ? @"Force Interpreter is ON for this machine, so the Cached Interpreter "
              @"setting is ignored and the compact engine is off too: this is the "
              @"slowest configuration. Turn it off from the machine menu "
              @"(Force Interpreter) and restart."
            : @"Force Interpreter is ON for this machine: reference interpreter only, "
              @"compact engine off. Turn it off from the machine menu to restore speed.";
        [self appendConsole:[NSString stringWithFormat:@"[vm] %@\n", why]];
    } else if ([backendPref isEqualToString:@"cached"] || [backendPref isEqualToString:@"block"] || [backendPref isEqualToString:@"cached-block"]) {
        backend = S5L8900_CPU_BACKEND_CACHED_BLOCK;
    } else if ([backendPref isEqualToString:@"ir"] || [backendPref isEqualToString:@"micro-op"]) {
        backend = S5L8900_CPU_BACKEND_IR_OPTIMIZED;
    } else if ([backendPref isEqualToString:@"jit"]) {
        backend = S5L8900_CPU_BACKEND_JIT;
    }
    if (!forced)
        why = backend == S5L8900_CPU_BACKEND_INTERPRETER
            ? @"Standard backend selected in Settings (reference interpreter plus "
              @"the compact engine where built). The default, Cached Interpreter, "
              @"measured about twice as fast on device; choose it in "
              @"Settings > Diagnostics > CPU Execution Backend."
            : nil;
    s5l8900_set_cpu_backend(&_machine, backend);
    s5l8900_set_direct_ram_writes(&_machine, true);
    pthread_mutex_lock(&_lock);
    _diagBackendNote = why;
    pthread_mutex_unlock(&_lock);
    /* IR_OPTIMIZED and JIT are retired names kept for saved settings; the
     * core runs both on the cached interpreter (see soc.h). */
    const char *bname = (backend == S5L8900_CPU_BACKEND_INTERPRETER) ? "standard (reference interpreter + compact engine where built)" :
                        (backend == S5L8900_CPU_BACKEND_CACHED_BLOCK) ? "cached interpreter" :
                        "cached interpreter (older setting)";
    [self appendConsole:[NSString stringWithFormat:@"[vm] CPU execution backend: %s\n", bname]];
}

#pragma mark - Lifecycle

- (BOOL)start {
    /* start/stop are normally called by the main thread, but keeping the state
     * transition under the same lock as the worker flags prevents a retry from
     * racing the final free after a guest halt. */
    pthread_mutex_lock(&_lock);
    if (_state == VMEngineStateRunning) {
        pthread_mutex_unlock(&_lock);
        return YES;
    }
    if (_state != VMEngineStateIdle) {
        pthread_mutex_unlock(&_lock);
        return NO;
    }
    _state = VMEngineStateStarting;
    _stopRequested = NO;
    _stopCompletion = nil;
    _checkpointRequested = NO;
    _checkpointCompletion = nil;
    _paused = NO;
    _pauseReason = nil;
    _snapshotFresh = NO;
    _snapshotBlank = NO;
    _retired = 0;
    _rate = 0.0;
    /* A restart must not inherit the previous machine's pending finger, nor
     * its counters — the status line reads them as claims about THIS run. */
    vm_touch_queue_reset(&_touch);
    vm_touch_delivery_reset(&_touchDelivery);
    _touchDelivered = 0;
    _droppedTouchLogged = NO;
    /* And the buttons, for the same reason: a switch held when the last
     * machine went away is not held on this one, whose board resets with
     * every switch released. */
    vm_button_queue_reset(&_buttonQueue);
    memset(_buttons, 0, sizeof _buttons);
    _buttonDelivered = 0;
    _buttonRefused = 0;
    memset(&_powerHold, 0, sizeof _powerHold);
    memset(&_momentaryHolds, 0, sizeof _momentaryHolds);
    _droppedButtonLogged = NO;
    _status = @"starting";
    BOOL needSnapshot = (_snapshot == NULL);
    [self publishBlankSnapshotLocked];
    pthread_mutex_unlock(&_lock);

    if (needSnapshot) {
        uint8_t *snapshot = calloc(1, VM_FB_BYTES);
        pthread_mutex_lock(&_lock);
        _snapshot = snapshot;
        [self publishBlankSnapshotLocked];
        if (!snapshot) {
            _state = VMEngineStateIdle;
            _stopRequested = NO;
            _paused = NO;
            _status = @"out of memory";
        }
        pthread_mutex_unlock(&_lock);
        if (!snapshot) return NO;
    }

    pthread_mutex_lock(&_lock);
    BOOL cancelledEarly = (_state == VMEngineStateStopping || _stopRequested);
    VMEngineStopCompletion earlyStopCompletion = nil;
    if (cancelledEarly) {
        earlyStopCompletion = _stopCompletion;
        _stopCompletion = nil;
        _state = VMEngineStateIdle;
        _stopRequested = NO;
        _paused = NO;
        _pauseReason = nil;
        _status = @"stopped";
    }
    pthread_mutex_unlock(&_lock);
    if (cancelledEarly) {
        if (earlyStopCompletion)
            dispatch_async(dispatch_get_main_queue(), earlyStopCompletion);
        return NO;
    }

    // Measured either side of the allocation so the guest DRAM's real cost is
    // a number on the screen rather than an assertion in a comment.
    uint64_t before = [VMEngine physFootprintBytes];

    if (!s5l8900_init(&_machine, VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE)) {
        pthread_mutex_lock(&_lock);
        _state = VMEngineStateIdle;
        _stopRequested = NO;
        _status = @"allocation failed";
        pthread_mutex_unlock(&_lock);
        [self appendConsole:@"[vm] could not allocate 128 MB of guest DRAM\n"];
        return NO;
    }
    [self configureCpuBackend];
    if (![self installGuestPayload]) {
        s5l8900_free(&_machine);
        vm_firmware_boot_destroy(&_firmwareBoot);
        pthread_mutex_lock(&_lock);
        _state = VMEngineStateIdle;
        _stopRequested = NO;
        _status = @"guest setup failed";
        pthread_mutex_unlock(&_lock);
        [self appendConsole:@"[vm] could not install the guest payload\n"];
        return NO;
    }

    uint64_t after = [VMEngine physFootprintBytes];
    [self appendConsole:[NSString stringWithFormat:
        @"[vm] guest DRAM: %u MB at 0x%08x, framebuffer at 0x%08x\n"
        @"[vm] footprint before/after allocating it: %.1f / %.1f MB\n",
        VM_GUEST_RAM_SIZE / (1024u * 1024u), VM_GUEST_RAM_BASE,
        vm_guest_fb_pa(VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE),
        before / 1048576.0, after / 1048576.0]];

    NSThread *thread = [[NSThread alloc] initWithTarget:self
                                               selector:@selector(threadMain:)
                                                 object:nil];
    if (!thread) {
        s5l8900_free(&_machine);
        vm_firmware_boot_destroy(&_firmwareBoot);
        pthread_mutex_lock(&_lock);
        _state = VMEngineStateIdle;
        _stopRequested = NO;
        _status = @"thread allocation failed";
        pthread_mutex_unlock(&_lock);
        [self appendConsole:@"[vm] could not allocate the emulator thread\n"];
        return NO;
    }
    thread.name = @"S5LBox emulator";
    thread.qualityOfService = NSQualityOfServiceUserInitiated;
    thread.stackSize = 512 * 1024;

    pthread_mutex_lock(&_lock);
    BOOL cancelled = (_state == VMEngineStateStopping || _stopRequested);
    if (!cancelled) {
        _machineReady = YES;
        _thread = thread;
        _state = VMEngineStateRunning;
    }
    pthread_mutex_unlock(&_lock);

    if (cancelled) {
        s5l8900_free(&_machine);
        vm_firmware_boot_destroy(&_firmwareBoot);
        pthread_mutex_lock(&_lock);
        VMEngineStopCompletion cancelledStopCompletion = _stopCompletion;
        _stopCompletion = nil;
        _state = VMEngineStateIdle;
        _stopRequested = NO;
        _paused = NO;
        _pauseReason = nil;
        _status = @"stopped";
        pthread_mutex_unlock(&_lock);
        if (cancelledStopCompletion)
            dispatch_async(dispatch_get_main_queue(), cancelledStopCompletion);
        return NO;
    }

    VMAudioOutput *audioOutput = [[VMAudioOutput alloc] init];
    [audioOutput start];
    pthread_mutex_lock(&_lock);
    _audioOutput = audioOutput;
    pthread_mutex_unlock(&_lock);
    s5l8900_set_audio_sink(&_machine, vm_audio_tx_callback, NULL, (__bridge void *)self);

    [thread start];
    return YES;
}

- (void)stop {
    [self stopWithCompletion:nil];
}

- (void)stopWithCompletion:(VMEngineStopCompletion)completion {
    BOOL completeNow = NO;
    pthread_mutex_lock(&_lock);
    if (_state == VMEngineStateIdle) {
        completeNow = YES;
    } else {
        if (completion) {
            VMEngineStopCompletion next = [completion copy];
            VMEngineStopCompletion prior = _stopCompletion;
            if (prior) {
                _stopCompletion = ^{
                    prior();
                    next();
                };
            } else {
                _stopCompletion = next;
            }
        }
    }
    if (_state == VMEngineStateStarting || _state == VMEngineStateRunning ||
        _state == VMEngineStateCheckpointing) {
        _stopRequested = YES;
        _paused = NO;
        _pauseReason = nil;
        _state = VMEngineStateStopping;
    }
    pthread_mutex_unlock(&_lock);

    if (completeNow && completion) {
        VMEngineStopCompletion done = [completion copy];
        dispatch_async(dispatch_get_main_queue(), done);
    }
}

- (void)saveCheckpointAndStopWithCompletion:
        (VMEngineCheckpointCompletion)completion {
    NSString *refusal = nil;
    pthread_mutex_lock(&_lock);
    if (_state == VMEngineStateCheckpointing || _checkpointRequested) {
        refusal = @"A machine checkpoint is already being saved.";
    } else if (_state != VMEngineStateRunning || !_machineReady) {
        refusal = @"The machine stopped before it could be saved.";
    } else if (!_firmwareBoot) {
        refusal = @"Only a running iPhone OS machine can be resumed automatically.";
    } else {
        _checkpointRequested = YES;
        _checkpointCompletion = [completion copy];
        _state = VMEngineStateCheckpointing;
        _status = @"saving checkpoint";
        _rate = 0.0;
    }
    pthread_mutex_unlock(&_lock);

    if (refusal.length && completion) {
        VMEngineCheckpointCompletion rejected = [completion copy];
        dispatch_async(dispatch_get_main_queue(), ^{
            rejected(NO, refusal);
        });
    }
}

- (void)setPaused:(BOOL)paused {
    [self setPaused:paused reason:(paused ? @"requested" : nil)];
}

- (void)setPaused:(BOOL)paused reason:(NSString *)reason {
    NSString *event = nil;
    pthread_mutex_lock(&_lock);
    /* A suspended machine is retiring nothing, so say nothing rather than
     * leaving the smoothed rate frozen at whatever it was a moment before the
     * pause -- a number that keeps claiming work is being done.
     *
     * Only when it is actually running: -setPaused: is also reached on the way
     * to the background, and a machine that has already halted must keep
     * saying "halted" rather than being relabelled as merely paused. */
    if (_state == VMEngineStateRunning && paused != _paused) {
        _rate = 0.0;
        if (paused) {
            _pauseReason = reason.length ? [reason copy] : @"requested";
            _status = [NSString stringWithFormat:@"paused (%@)", _pauseReason];
            event = [NSString stringWithFormat:@"[vm] paused: %@\n", _pauseReason];
        } else {
            NSString *oldReason = _pauseReason ?: @"requested";
            _pauseReason = nil;
            _status = @"running";
            event = [NSString stringWithFormat:@"[vm] resumed: %@ pause cleared\n",
                                               oldReason];
        }
    }
    _paused = paused;
    VMAudioOutput *audioOutput = _audioOutput;
    pthread_mutex_unlock(&_lock);
    if (paused) {
        [audioOutput pause];
    } else {
        [audioOutput resume];
    }
    if (event.length) [self appendConsole:event];
}

- (BOOL)isPaused {
    pthread_mutex_lock(&_lock);
    BOOL paused = _paused;
    pthread_mutex_unlock(&_lock);
    return paused;
}

- (BOOL)isRunning {
    pthread_mutex_lock(&_lock);
    BOOL running = (_state == VMEngineStateRunning ||
                    _state == VMEngineStateCheckpointing);
    pthread_mutex_unlock(&_lock);
    return running;
}

- (void)setInstructionCap:(uint64_t)cap {
    pthread_mutex_lock(&_lock);
    _instructionCap = cap;
    pthread_mutex_unlock(&_lock);
}

- (uint64_t)instructionCap {
    pthread_mutex_lock(&_lock);
    uint64_t cap = _instructionCap;
    pthread_mutex_unlock(&_lock);
    return cap;
}

#pragma mark - Guest input (see the header for what reaches the guest)

+ (NSString *)buttonUnavailableReason {
    /*
     * NIL, and this is the only thing that changed about it: the path exists.
     * core/src/soc/buttons.c models all five of the board's switches on the
     * GPIO pins and interrupt lines /device-tree/buttons names, and
     * -setButton:pressed: queues a transition that the emulator thread hands
     * to that model.
     *
     * This is a statement about the EMULATOR, not about the guest. Whether any
     * particular guest has a driver listening is a live question with a live
     * answer, and -buttonUnavailableReason is where it is asked; a control bar
     * that wants to know whether pressing Home will do something must ask that
     * one. This class method exists so a control can be built at all.
     */
    return nil;
}

- (NSString *)buttonUnavailableReason {
    pthread_mutex_lock(&_lock);
    BOOL ready      = _machineReady && _state == VMEngineStateRunning;
    uint64_t made   = _buttonDelivered;
    uint64_t lost   = _buttonQueue.dropped;
    uint64_t held   = _buttonQueue.count;
    uint64_t said_no = _buttonRefused;
    pthread_mutex_unlock(&_lock);

    /* Once the board has taken a transition, the app's half of this is proven
     * and nothing later can un-prove it. A subsequent refusal is a fact about
     * the guest at that moment, not about whether this path works. */
    if (made > 0) return nil;
    if (!ready)   return @"the machine is not running";
    if (said_no > 0)
        return @"the guest has not armed the button interrupt lines — no "
               @"driver has claimed /device-tree/buttons";
    if (lost > 0) return @"the queue filled before the board took anything";
    if (held > 0) return @"queued, not yet handed to the board";
    return @"nothing sent yet";
}

- (void)buttonCountersQueued:(uint64_t *)queued
                   delivered:(uint64_t *)delivered
                     refused:(uint64_t *)refused
                     dropped:(uint64_t *)dropped {
    pthread_mutex_lock(&_lock);
    if (queued)    *queued    = _buttonQueue.queued;
    if (delivered) *delivered = _buttonDelivered;
    if (refused)   *refused   = _buttonRefused;
    if (dropped)   *dropped   = _buttonQueue.dropped;
    pthread_mutex_unlock(&_lock);
}

- (NSString *)touchUnavailableReason {
    pthread_mutex_lock(&_lock);
    BOOL ready     = _machineReady && _state == VMEngineStateRunning;
    uint64_t made  = _touchDelivered;
    uint64_t lost  = _touch.dropped;
    uint64_t held  = _touch.count;
    pthread_mutex_unlock(&_lock);

    /* Once the device has taken a report, the app's half of this is proven and
     * nothing later can un-prove it. A subsequent refusal is a fact about the
     * guest at that moment, not about whether this path works. */
    if (made > 0) return nil;
    if (!ready)   return @"the machine is not running";
    if (lost > 0)
        return @"the guest's touch controller refused the report — no driver "
               @"has announced itself";
    if (held > 0) return @"queued, not yet handed to the controller";
    return @"nothing sent yet";
}

- (void)touchCountersQueued:(uint64_t *)queued
                  delivered:(uint64_t *)delivered
                  coalesced:(uint64_t *)coalesced
                    dropped:(uint64_t *)dropped {
    pthread_mutex_lock(&_lock);
    if (queued)    *queued    = _touch.queued;
    if (delivered) *delivered = _touchDelivered;
    if (coalesced) *coalesced = _touch.coalesced;
    if (dropped)   *dropped   = _touch.dropped;
    pthread_mutex_unlock(&_lock);
}

+ (NSString *)nameForButton:(VMButton)button {
    switch (button) {
        case VMButtonHome:         return @"Home";
        case VMButtonPower:        return @"Power";
        case VMButtonVolumeUp:     return @"Vol +";
        case VMButtonVolumeDown:   return @"Vol -";
        case VMButtonRingerSilent: return @"Silent";
        case VMButtonCount:        break;
    }
    return @"?";
}

- (BOOL)setButton:(VMButton)button pressed:(BOOL)pressed {
    unsigned which = 0;
    bool guestPressed = false;
    /* The app's enum order and the core's are different and permanently so —
     * see VMButtonQueue.h. The translation is a table in plain C that a host CI
     * runner checks against the core's enum, rather than a cast that happens to
     * work; the cast below is safe only because of the _Static_asserts at the
     * top of this file, which are the one place the two app enums meet. */
    if (!vm_button_to_guest((unsigned)button, pressed ? true : false,
                            &which, &guestPressed))
        return NO;

    BOOL queued = NO, notRunning = NO;
    pthread_mutex_lock(&_lock);
    /* Recorded under the same lock as everything else the UI can read, so the
     * control bar can draw a held key without asking the emulator thread. */
    _buttons[button] = pressed;
    if (!_machineReady || _state != VMEngineStateRunning)
        notRunning = YES;
    else
        queued = vm_button_queue_push(&_buttonQueue, which, guestPressed);
    pthread_mutex_unlock(&_lock);

    if (notRunning) {
        [self noteDiscardedInput];
        return NO;
    }
    if (!queued) {
        [self noteDroppedButton];
        return NO;
    }
    return YES;     /* QUEUED. Not "the guest saw it" — see the header. */
}

- (BOOL)isButtonPressed:(VMButton)button {
    if (button >= VMButtonCount) return NO;
    pthread_mutex_lock(&_lock);
    BOOL held = _buttons[button];
    pthread_mutex_unlock(&_lock);
    return held;
}

- (BOOL)sendTouchAtGuestX:(int)x y:(int)y phase:(vm_touch_phase_t)phase {
    s5l_mt_contact_t c;
    if (!vm_touch_contact_from_ui(phase, x, y, &c))
        return NO;              // off the panel, or a phase we do not produce

    BOOL queued = NO, notRunning = NO;
    pthread_mutex_lock(&_lock);
    if (!_machineReady || _state != VMEngineStateRunning)
        notRunning = YES;
    else
        queued = vm_touch_queue_push(&_touch, &c);
    pthread_mutex_unlock(&_lock);

    if (notRunning) {
        [self noteDiscardedInput];
        return NO;
    }
    if (!queued) {
        [self noteDroppedTouch];
        return NO;
    }
    return YES;
}

/*
 * Drain one queued report into the device. Runs ONLY on the emulator thread,
 * between chunks, which is what makes it safe to reach into _machine at all.
 *
 * One per chunk is not a throttle, it is the device's own shape: the Z2 holds
 * a single report and refuses the next until the guest has clocked this one
 * out. Draining more would just be a run of refusals.
 *
 * A refusal is not necessarily an error. s5l_mtz2_set_contacts() says no when
 * a report is still pending — ordinary backpressure, and the report stays
 * queued for the next chunk — and also when the part is held in reset or the
 * driver has not yet been told it is alive, in which case the report can never
 * be read and holding it would wedge the queue behind it. s5l_mtz2_irq() tells
 * the two apart: it is true exactly while a queued report is still unread.
 */
- (void)drainOneTouch_emulatorThread {
    s5l_mt_contact_t c;
    pthread_mutex_lock(&_lock);
    BOOL have = vm_touch_queue_peek(&_touch, &c);
    pthread_mutex_unlock(&_lock);
    if (!have) return;

    if (s5l_mtz2_set_contacts(&_machine.mtz2, &c, 1u)) {
        /* The attention line moved behind the bus. `level_dirty` in soc.h says
         * why a machine that is not told re-derives the cascade up to 68
         * instructions later instead of at this chunk boundary. */
        s5l8900_tick(&_machine, 0);
        pthread_mutex_lock(&_lock);
        vm_touch_queue_pop(&_touch);
        vm_touch_delivery_note_accepted(&_touchDelivery, &c);
        _touchDelivered++;
        pthread_mutex_unlock(&_lock);
        return;
    }

    if (s5l_mtz2_irq(&_machine.mtz2))
        return;                 // backpressure: the guest has not read yet

    /* The device is in no state to report and will not become one by waiting
     * on this report in particular. Drop it, count it, and let the next one
     * try — a queue that never empties would turn a transient into a
     * permanent loss of input. */
    pthread_mutex_lock(&_lock);
    vm_touch_queue_pop(&_touch);
    _touch.dropped++;
    pthread_mutex_unlock(&_lock);
    [self noteDroppedTouch];
}

/*
 * Hand ONE queued transition to the board. Runs ONLY on the emulator thread,
 * between chunks, which is what makes it safe to reach into _machine at all.
 *
 * One per chunk is not a throttle, it is the board's own shape: it refuses a
 * second transition on a line whose previous one the guest has not serviced,
 * so draining more would just be a run of refusals.
 *
 * A refusal is not necessarily an error and it is never a reason to throw the
 * transition away. s5l8900_set_button() says no while the guest has not armed
 * that line's interrupt — which, for a real boot, is true until
 * AppleM68Buttons starts a couple of hundred million instructions in — and
 * while the previous transition on that line is still pending. Both become
 * false with time, so the transition stays queued and is retried, exactly as
 * bootkernel's --button retries on every instruction. Only a FULL queue loses
 * input, and VMButtonQueue.c counts that.
 */
- (void)drainOneButton_emulatorThread {
    vm_button_event_t e;
    pthread_mutex_lock(&_lock);
    BOOL have = vm_button_queue_peek(&_buttonQueue, &e);
    vm_button_power_hold_t powerHold = _powerHold;
    vm_button_momentary_holds_t momentaryHolds = _momentaryHolds;
    pthread_mutex_unlock(&_lock);
    if (!have) return;

    /* A UIKit tap can queue down and up in the same emulator chunk.  Every
     * momentary key is sampled by a later guest debounce callback, so a
     * zero-duration electrical pulse can disappear before AppleM68Buttons ever
     * sees it.  Power additionally has a post-wake boundary.  Both policies
     * anchor at BOARD acceptance, not at the UI event, because a press may wait
     * in this queue while the guest arms its line. */
    uint64_t nowNS = vm_now_ns();
    uint64_t nowCycles = _machine.cpu.cycles;
    bool displayRunning = s5l_clcd_running(&_machine.clcd);
    if (!vm_button_momentary_release_ready(&e, &momentaryHolds, nowNS))
        return;
    if (!vm_button_power_release_ready(&e, &powerHold, nowNS, nowCycles,
                                       displayRunning))
        return;

    if (s5l8900_set_button(&_machine, e.which, e.pressed)) {
        if (nowNS == 0u) nowNS = vm_now_ns();
        nowCycles = _machine.cpu.cycles;
        pthread_mutex_lock(&_lock);
        vm_button_queue_pop(&_buttonQueue);
        _buttonDelivered++;
        vm_button_momentary_note_accepted(&e, &_momentaryHolds, nowNS);
        if (e.which == S5L_BUTTON_HOLD) {
            if (e.pressed) {
                _powerHold.active = true;
                _powerHold.display_running_at_press = displayRunning;
                _powerHold.delivered_ns = nowNS;
                _powerHold.delivered_cycles = nowCycles;
            } else {
                memset(&_powerHold, 0, sizeof _powerHold);
            }
        }
        pthread_mutex_unlock(&_lock);
        return;
    }
    pthread_mutex_lock(&_lock);
    _buttonRefused++;
    pthread_mutex_unlock(&_lock);
}

/*
 * A checkpoint is a lifecycle cancellation boundary. Once
 * saveCheckpointAndStopWithCompletion: changes the state to Checkpointing, the
 * UI cannot enqueue anything new. Reports which have not reached hardware can
 * therefore be cancelled; reports the devices already accepted must receive a
 * real release and enough guest execution to consume it before serialization.
 */
- (void)beginCheckpointInputQuiesce_emulatorThread {
    pthread_mutex_lock(&_lock);
    unsigned touches = vm_touch_queue_cancel_pending(&_touch);
    unsigned buttons = vm_button_queue_cancel_pending(&_buttonQueue);
    memset(_buttons, 0, sizeof _buttons);
    pthread_mutex_unlock(&_lock);

    if (touches || buttons) {
        [self appendConsole:[NSString stringWithFormat:
            @"[input] checkpoint cancelled %u queued touch report(s) and %u "
             @"button transition(s) before releasing accepted input\n",
            touches, buttons]];
    }
}

- (BOOL)checkpointInputIsQuiescent_emulatorThread {
    BOOL touchWaiting = NO;
    pthread_mutex_lock(&_lock);
    unsigned touchQueued = _touch.count;
    vm_touch_delivery_state_t touchDelivery = _touchDelivery;
    pthread_mutex_unlock(&_lock);

    if (touchQueued) {
        [self drainOneTouch_emulatorThread];
        touchWaiting = YES;
    } else if (s5l_mtz2_irq(&_machine.mtz2)) {
        /* A report already accepted by the controller still has to be clocked
         * out before a cancellation can follow it. */
        touchWaiting = YES;
    } else if (touchDelivery.active) {
        s5l_mt_contact_t release;
        if (vm_touch_delivery_make_break(&touchDelivery, &release)) {
            pthread_mutex_lock(&_lock);
            BOOL queued = vm_touch_queue_push(&_touch, &release);
            pthread_mutex_unlock(&_lock);
            if (queued) [self drainOneTouch_emulatorThread];
        }
        touchWaiting = YES;
    } else if (s5l_gpioic_pending(&_machine.gpioic,
                                  S5L_GPIOIC_LINE_MULTITOUCH)) {
        /* The controller has been read; let the guest acknowledge the cascade
         * as well so the restored boundary contains no deferred touch IRQ. */
        touchWaiting = YES;
    }

    BOOL buttonWaiting = NO;
    pthread_mutex_lock(&_lock);
    unsigned buttonQueued = _buttonQueue.count;
    pthread_mutex_unlock(&_lock);

    if (buttonQueued) {
        [self drainOneButton_emulatorThread];
        buttonWaiting = YES;
    } else {
        /* Ordinary keys first. Power has a longer, display-aware release floor
         * and must not prevent independent lines from returning to rest. */
        static const unsigned releaseOrder[S5L_BUTTON_COUNT] = {
            S5L_BUTTON_MENU, S5L_BUTTON_VOLUP, S5L_BUTTON_VOLDOWN,
            S5L_BUTTON_RINGERAB, S5L_BUTTON_HOLD,
        };
        for (unsigned i = 0; i < S5L_BUTTON_COUNT; i++) {
            unsigned which = releaseOrder[i];
            if (!s5l_buttons_held(&_machine.buttons, which)) continue;
            pthread_mutex_lock(&_lock);
            BOOL queued = vm_button_queue_push(&_buttonQueue, which, false);
            pthread_mutex_unlock(&_lock);
            if (queued) [self drainOneButton_emulatorThread];
            buttonWaiting = YES;
            break;
        }

        /* The release itself is serialized, but waiting for its interrupt to
         * clear makes this a genuinely quiet guest boundary rather than a
         * deferred host gesture that happens to be representable on disk. */
        if (!buttonWaiting) {
            for (unsigned i = 0; i < S5L_BUTTON_COUNT; i++) {
                if (s5l_gpioic_pending(&_machine.gpioic,
                                       s5l_button_line(i))) {
                    buttonWaiting = YES;
                    break;
                }
            }
        }
    }

    if (touchWaiting || buttonWaiting) return NO;

    pthread_mutex_lock(&_lock);
    memset(_buttons, 0, sizeof _buttons);
    memset(&_powerHold, 0, sizeof _powerHold);
    memset(&_momentaryHolds, 0, sizeof _momentaryHolds);
    pthread_mutex_unlock(&_lock);
    return YES;
}

/* Same discipline as noteDroppedTouch. */
- (void)noteDroppedButton {
    pthread_mutex_lock(&_lock);
    BOOL first = !_droppedButtonLogged;
    _droppedButtonLogged = YES;
    uint64_t dropped = _buttonQueue.dropped;
    pthread_mutex_unlock(&_lock);
    if (!first) return;

    [self appendConsole:[NSString stringWithFormat:
        @"[input] a button transition was not delivered (%llu so far). The "
        @"queue was full, and a press or a release may never be coalesced "
        @"away. Printed once.\n", (unsigned long long)dropped]];
}

/* Same discipline as noteDiscardedInput: a drag can drop many reports, and a
 * console that scrolls the guest's own output away to repeat itself is worse
 * than one that says it once. */
- (void)noteDroppedTouch {
    pthread_mutex_lock(&_lock);
    BOOL first = !_droppedTouchLogged;
    _droppedTouchLogged = YES;
    uint64_t dropped = _touch.dropped;
    pthread_mutex_unlock(&_lock);
    if (!first) return;

    [self appendConsole:[NSString stringWithFormat:
        @"[input] a touch report was not delivered (%llu so far). The guest's "
        @"touch controller refused it, or the queue was full of edges that "
        @"must not be coalesced. Printed once.\n",
        (unsigned long long)dropped]];
}

/* Say it once. A drag produces a report per frame, and a console that scrolls
 * the guest's own output away to repeat the same refusal is worse than one
 * that states it plainly and then stops. */
- (void)noteDiscardedInput {
    pthread_mutex_lock(&_lock);
    BOOL first = !_discardedInputLogged;
    _discardedInputLogged = YES;
    pthread_mutex_unlock(&_lock);
    if (!first) return;

    /* This used to quote +buttonUnavailableReason, which now returns nil and
     * would have formatted as "(null)". It was never the right sentence
     * anyway: both callers reach here for one reason and it is this one. */
    [self appendConsole:
        @"[input] discarded: the machine is not running. The input is shown on "
        @"screen and thrown away; the guest is never told. Printed once.\n"];
}

#pragma mark - Spin sampler

/*
 * A WEDGED GUEST AND A BUSY ONE LOOK IDENTICAL FROM OUTSIDE. Instructions
 * retire, the rate looks healthy, and nothing is printed. The difference is
 * WHERE the instructions go, and on a phone there is no debugger to ask.
 *
 * So the emulator thread asks the machine itself, between chunks, at a point
 * where nothing is executing and no lock is held: one PC per chunk, binned
 * into 64-byte regions.
 *
 * Sampling once per 100,000 instructions sounds far too sparse to find a
 * loop, and for a profile it would be. For THIS question it is exactly right:
 * a spin loop executes its handful of instructions billions of times, so
 * every single sample lands inside it. Sparseness is not a weakness here, it
 * is what makes the signal unambiguous -- a region that takes almost a whole
 * window is not "hot", it is the only thing running.
 *
 * Diagnostics only, in the sense this project means it: it never stops the
 * guest, never writes guest state, never changes what executes, and cannot
 * manufacture a result. It reports where the machine already was.
 *
 * Addresses are printed raw rather than symbolised. Carrying a symbol table
 * for an 8 MB kernel on the device costs memory on the one machine that has
 * least of it, and resolving four numbers against ksyms afterwards costs
 * nothing.
 */
#define kVMSpinRegionShift 6u      /* 64-byte regions                        */
#define kVMSpinSlots       64u     /* wider than any spin; full == not a spin */
#define kVMSpinWindow      512u    /* ~2.5 s of guest time at 20 M insn/s    */
#define kVMSpinShareNum    7u      /* report at >= 7/8 of a window in one    */
#define kVMSpinShareDen    8u      /* region                                 */
#define kVMSpinReportCap   6u      /* never turn the console into a log      */
#define kVMSpinNarrowRegions 4u    /* <= 256 bytes of code IS a loop         */

typedef struct {
    uint32_t region[kVMSpinSlots];
    uint32_t count[kVMSpinSlots];
    unsigned used;
    unsigned samples;
    uint32_t reported[kVMSpinReportCap];
    unsigned reports;
    unsigned quiet;          /* windows closed without a concentrated region */
    bool     spreadReported;
} vm_spin_t;

/* Windows of ordinary work before saying so. A "stuck" report that stays
 * silent when the guest is merely grinding leaves the user unable to tell the
 * two apart, which is the whole question they are asking. */
#define kVMSpinSpreadWindows 8u

static void vm_spin_sample(vm_spin_t *s, uint32_t pc) {
    uint32_t region = pc >> kVMSpinRegionShift;
    s->samples++;
    for (unsigned i = 0; i < s->used; i++) {
        if (s->region[i] == region) { s->count[i]++; return; }
    }
    /* Table full means the guest is spread across more than 64 regions, which
     * is the shape of ordinary work, not a spin. The sample still counts so
     * the window closes on time and the share test fails honestly. */
    if (s->used < kVMSpinSlots) {
        s->region[s->used] = region;
        s->count[s->used] = 1u;
        s->used++;
    }
}

static void vm_spin_reset(vm_spin_t *s) {
    s->used = 0u;
    s->samples = 0u;
}

/* Index of the nth-hottest region, or -1 once they run out. Selection rather
 * than a sort: n is 3 and the table is 64. */
static int vm_spin_rank(const vm_spin_t *s, unsigned n) {
    int best = -1;
    uint32_t ceiling = 0xffffffffu;
    for (unsigned rank = 0; rank <= n; rank++) {
        best = -1;
        for (unsigned i = 0; i < s->used; i++) {
            if (s->count[i] > ceiling) continue;
            if (best < 0 || s->count[i] > s->count[best]) best = (int)i;
        }
        if (best < 0) return -1;
        ceiling = s->count[best] - 1u;   /* strictly below, next round */
    }
    return best;
}

static bool vm_spin_already_reported(const vm_spin_t *s, uint32_t region) {
    for (unsigned i = 0; i < s->reports && i < kVMSpinReportCap; i++)
        if (s->reported[i] == region) return true;
    return false;
}

#pragma mark - Emulator thread

- (void)threadMain:(id)unused {
    (void)unused;
    /* The engine counters describe this run, like `retired` below. */
    if (_machine.ci) arm_ci_reset_stats(_machine.ci);
    const uint64_t threadStartNs = vm_now_ns();
    const uint64_t idleBaseNs = _machine.wfi_paced_wait_ns;
    uint64_t runNs = 0;
    double lastPublish = vm_now();
    uint64_t retired = 0, retiredAtLastPublish = 0;
    arm_status_t status = ARM_OK;
    BOOL stoppedByRequest = NO;
    BOOL reachedCap = NO;
    BOOL checkpointSaved = NO;
    NSString *checkpointFailure = nil;
    BOOL checkpointInputPrepared = NO;
    uint64_t checkpointInputStartNS = 0u;
    uint64_t checkpointInputStartRetired = 0u;

    /* Emulator-thread only, so it is a local and not an ivar: nothing else may
     * read it and no lock can be forgotten. */
    vm_spin_t spin;
    memset(&spin, 0, sizeof spin);

    while (YES) {
        @autoreleasepool {
            pthread_mutex_lock(&_lock);
            BOOL stop = _stopRequested;
            BOOL checkpoint = _checkpointRequested;
            BOOL paused = _paused;
            pthread_mutex_unlock(&_lock);

            if (stop) {
                stoppedByRequest = YES;
                break;
            }
            if (checkpoint) {
                if (!checkpointInputPrepared) {
                    [self beginCheckpointInputQuiesce_emulatorThread];
                    checkpointInputStartNS = vm_now_ns();
                    checkpointInputStartRetired = retired;
                    checkpointInputPrepared = YES;
                }

                BOOL inputReady =
                    [self checkpointInputIsQuiescent_emulatorThread];
                uint64_t nowNS = vm_now_ns();
                BOOL hostTimedOut =
                    !inputReady && checkpointInputStartNS != 0u &&
                    nowNS != 0u && nowNS >= checkpointInputStartNS &&
                    nowNS - checkpointInputStartNS >=
                        kVMCheckpointInputTimeoutNS;
                BOOL guestTimedOut =
                    !inputReady && retired >= checkpointInputStartRetired &&
                    retired - checkpointInputStartRetired >=
                        kVMCheckpointInputTimeoutInstructions;

                char why[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
                vm_firmware_checkpoint_status_t saved = VM_FW_CHECKPOINT_BUSY;
                if (inputReady) {
                    /* Drain host-visible output before serializing. Otherwise
                     * UART bytes already shown on this run would be present
                     * again after restore and appear twice. */
                    [self publishRetired:retired rate:0.0 status:ARM_OK];
                    saved = vm_firmware_boot_save_resume(
                        _firmwareBoot, &_machine, why, sizeof why);
                } else if (hostTimedOut || guestTimedOut) {
                    (void)snprintf(
                        why, sizeof why,
                        "The guest did not consume transient input releases "
                        "within the checkpoint safety window. Nothing was "
                        "saved; the machine is still running.");
                    saved = VM_FW_CHECKPOINT_ERROR;
                }
                if (saved == VM_FW_CHECKPOINT_OK) {
                    [self appendConsole:@"[vm] automatic resume checkpoint saved\n"];
                    checkpointSaved = YES;
                    stoppedByRequest = YES;
                    break;
                }
                if (saved == VM_FW_CHECKPOINT_ERROR) {
                    checkpointFailure = why[0]
                        ? [NSString stringWithUTF8String:why]
                        : @"The machine checkpoint could not be saved.";
                    if (!checkpointFailure.length)
                        checkpointFailure = @"The machine checkpoint could not be saved.";

                    pthread_mutex_lock(&_lock);
                    VMEngineCheckpointCompletion failed = _checkpointCompletion;
                    _checkpointCompletion = nil;
                    _checkpointRequested = NO;
                    _state = VMEngineStateRunning;
                    _status = _paused ? @"paused" : @"running (save failed)";
                    pthread_mutex_unlock(&_lock);
                    checkpointInputPrepared = NO;

                    if (failed) {
                        NSString *message = checkpointFailure;
                        dispatch_async(dispatch_get_main_queue(), ^{
                            failed(NO, message);
                        });
                    }
                    checkpointFailure = nil;
                    continue;
                }
                /* BUSY: execute one bounded chunk so input cancellation or an
                 * in-flight disk operation can reach a safe boundary, then
                 * retry before any further UI work. This deliberately advances
                 * even if the machine was user-paused: neither boundary is safe
                 * to freeze halfway through. */
            }
            if (paused && !checkpoint) {
                usleep(50 * 1000);
                lastPublish = vm_now();
                retiredAtLastPublish = retired;
                continue;
            }

            /* The only place ordinary UI input reaches the machine, and it is
             * on this thread, between chunks, with nothing executing. During a
             * checkpoint the quiesce state machine above owns input instead. */
            if (!checkpoint) {
                [self drainOneTouch_emulatorThread];
                [self drainOneButton_emulatorThread];
            }

            {
                const uint64_t t0 = vm_now_ns();
                const unsigned ran =
                    s5l8900_run(&_machine, kVMChunkInstructions, &status);
                retired += ran;
                runNs += vm_now_ns() - t0;
                if (_profileReady) {
                    pthread_mutex_lock(&_profileLock);
                    if (ran == kVMChunkInstructions && status == ARM_OK)
                        [self profileSample_emulatorThread];
                    else
                        _profileShort++;
                    pthread_mutex_unlock(&_profileLock);
                }
            }

            /* Taken here precisely because the chunk has ENDED: the machine is
             * between instructions, this thread owns it, and no lock is held. */
            vm_spin_sample(&spin, _machine.cpu.r[15]);
            if (spin.samples >= kVMSpinWindow) {
                int hot = vm_spin_rank(&spin, 0);
                uint32_t region = hot >= 0 ? spin.region[hot] : 0u;
                /*
                 * Two ways to be a spin, and the second was learned the hard
                 * way. A single region taking almost the whole window is the
                 * obvious one. But a loop that straddles a 64-byte boundary
                 * splits across two regions, and an unrolled one across three
                 * or four -- and testing only the hottest region called a
                 * genuinely wedged device "not spinning" while it sat in
                 * AppleMBX+0xb440 with 309 of 512 samples in one half and the
                 * rest in the other. A handful of distinct regions across a
                 * whole window IS the signal, however it splits between them.
                 */
                bool concentrated =
                    hot >= 0 &&
                    (uint64_t)spin.count[hot] * kVMSpinShareDen >=
                        (uint64_t)spin.samples * kVMSpinShareNum;
                bool narrow = spin.used > 0u && spin.used <= kVMSpinNarrowRegions;
                if (hot >= 0 && (concentrated || narrow) &&
                    spin.reports < kVMSpinReportCap &&
                    !vm_spin_already_reported(&spin, region)) {
                    spin.reported[spin.reports++] = region;

                    NSMutableString *next = [NSMutableString string];
                    for (unsigned n = 1; n <= 2; n++) {
                        int i = vm_spin_rank(&spin, n);
                        if (i < 0) break;
                        [next appendFormat:@"%@0x%08x (%u)",
                            next.length ? @", " : @"",
                            (unsigned)(spin.region[i] << kVMSpinRegionShift),
                            (unsigned)spin.count[i]];
                    }
                    uint32_t base = region << kVMSpinRegionShift;
                    /* What the loop is waiting on, when it is hardware we do
                     * not model: the most recent distinct unmodelled accesses,
                     * with their pcs and repeat counts. */
                    char unmodelled[1024], devices[2048];
                    (void)s5l_access_log_describe(_machine.mmio_recent,
                                                  S5L_ACCESS_LOG, 16u,
                                                  devices, sizeof devices);
                    (void)s5l_access_log_describe(_machine.unmodelled,
                                                  S5L_ACCESS_LOG, 6u,
                                                  unmodelled, sizeof unmodelled);
                    [self appendConsole:[NSString stringWithFormat:
                        @"[stall] recent hardware accesses, any device:\n%s"
                        @"[stall] recent unmodelled hardware accesses:\n%s",
                        devices, unmodelled]];
                    [self appendConsole:[NSString stringWithFormat:
                        @"[stall] at %.1f M insn the guest is looping in %u "
                        @"region%@: 0x%08x-0x%08x took %u of %u samples, cpsr "
                        @"0x%08x.%@%@ Nothing was stopped or altered to find "
                        @"this. Printed once per region.\n",
                        retired / 1.0e6, spin.used,
                        spin.used == 1u ? @"" : @"s", base,
                        base + (1u << kVMSpinRegionShift) - 1u,
                        (unsigned)spin.count[hot], (unsigned)spin.samples,
                        _machine.cpu.cpsr,
                        next.length ? @" Next: " : @"",
                        next.length ? [next stringByAppendingString:@"."] : @""]];
                } else if (hot >= 0) {
                    /* Not concentrated. Say that too, once: "it is working, it
                     * is just slow" and "it is wedged" are the two answers, and
                     * an instrument that can only report one of them is not
                     * telling the user what they asked. */
                    spin.quiet++;
                    if (spin.quiet >= kVMSpinSpreadWindows && !spin.spreadReported) {
                        spin.spreadReported = true;
                        [self appendConsole:[NSString stringWithFormat:
                            @"[stall] at %.1f M insn the guest is NOT spinning: "
                            @"work is spread over %u regions, the busiest "
                            @"(0x%08x) taking only %u of %u samples. It is "
                            @"executing real code, just slowly. Printed once.\n",
                            retired / 1.0e6, spin.used,
                            (unsigned)(spin.region[hot] << kVMSpinRegionShift),
                            (unsigned)spin.count[hot], (unsigned)spin.samples]];
                    }
                }
                vm_spin_reset(&spin);
            }

            /* A stop can arrive while the bounded chunk is executing. Let the
             * explicit lifecycle request win before publishing a simultaneous
             * guest halt as though the controller were still active. */
            pthread_mutex_lock(&_lock);
            stop = _stopRequested;
            checkpoint = _checkpointRequested;
            uint64_t cap = _instructionCap;
            if (status != ARM_OK && !stop &&
                (_state == VMEngineStateRunning ||
                 _state == VMEngineStateCheckpointing))
                _state = VMEngineStateStopping;
            if (cap > 0 && retired >= cap && !stop &&
                _state == VMEngineStateRunning)
                _state = VMEngineStateStopping;
            pthread_mutex_unlock(&_lock);
            if (stop) {
                stoppedByRequest = YES;
                break;
            }
            if (status != ARM_OK && checkpoint) {
                checkpointFailure =
                    @"The guest stopped before its checkpoint could be completed.";
            }

            /* A diagnostic limit, not a guest fault. Publish the frame and the
             * counters that were reached and stop there, leaving the last
             * picture up: the whole point of asking for a limit is to look at
             * what the machine had drawn by then.
             *
             * `status == ARM_OK` is load-bearing, not defensive. A chunk can
             * both fault and cross the cap, and the terminal block below
             * overwrites _status with "instruction cap reached" — so without
             * this test an undefined instruction or a halt would be reported
             * as a clean diagnostic stop. A failure reported as a success is
             * the one outcome this project does not permit. The fault wins;
             * the cap is still crossed and will be reported next time. */
            if (status == ARM_OK && !checkpoint && cap > 0 && retired >= cap) {
                reachedCap = YES;
                [self publishRetired:retired rate:0.0 status:status];
                break;
            }

            double now = vm_now();
            double elapsed = now - lastPublish;
            if (elapsed >= kVMPublishInterval || status != ARM_OK) {
                double instantRate = elapsed > 0
                    ? (double)(retired - retiredAtLastPublish) / elapsed : 0.0;
                [self publishRetired:retired rate:instantRate status:status];
                [self publishDiagnostics_emulatorThread:retired
                                                  runNs:runNs
                                                 idleNs:_machine.wfi_paced_wait_ns - idleBaseNs
                                               threadNs:vm_now_ns() - threadStartNs];
                lastPublish = now;
                retiredAtLastPublish = retired;
            }

            // A non-OK status means the guest hit an encoding this core does
            // not implement, or halted. Stopping is right: spinning on the same
            // faulting instruction would burn the battery and tell us nothing.
            if (status != ARM_OK) {
                break;
            }
        }
    }

    /* An explicit stop can arrive between regular publications. Drain the
     * remaining UART bytes and publish the final counters before destroying
     * the sole copy of the machine. */
    if (stoppedByRequest && !checkpointSaved)
        [self publishRetired:retired rate:0.0 status:ARM_OK];

    if (reachedCap)
        [self appendConsole:[NSString stringWithFormat:
            @"[vm] stopped at the instruction cap: %llu retired\n",
            (unsigned long long)retired]];
    else if (status != ARM_OK && !stoppedByRequest)
        [self appendConsole:[NSString stringWithFormat:
            @"[vm] terminal CPU status %d at pc=0x%08x cpsr=0x%08x "
             "after %llu retired\n",
            (int)status, _machine.cpu.r[15], _machine.cpu.cpsr,
            (unsigned long long)retired]];

    if (_machineReady) {
        s5l8900_set_audio_sink(&_machine, NULL, NULL, NULL);
        s5l8900_free(&_machine);
    }
    /* AFTER the machine, never before: the memory-disk bridges hold a borrowed
     * descriptor onto the work image and a pointer into guest DRAM, and both
     * have to stop existing before the file they write through is closed. */
    vm_firmware_boot_destroy(&_firmwareBoot);

    /* Publish the terminal state only after the machine is gone. In
     * particular, clear _thread here so a later start really creates a fresh
     * machine instead of returning a false success for a dead worker. */
    pthread_mutex_lock(&_lock);
    VMAudioOutput *audioToStop = _audioOutput;
    _audioOutput = nil;
    VMEngineCheckpointCompletion checkpointCompletion = _checkpointCompletion;
    VMEngineStopCompletion stopCompletion = _stopCompletion;
    _checkpointCompletion = nil;
    _stopCompletion = nil;
    _checkpointRequested = NO;
    _machineReady = NO;
    _thread = nil;
    if (stoppedByRequest) {
        [self publishBlankSnapshotLocked];
        _status = @"stopped";
        _rate = 0.0;
    } else if (reachedCap) {
        _status = @"instruction cap reached";
        _rate = 0.0;
    }
    _state = VMEngineStateIdle;
    _stopRequested = NO;
    _paused = NO;
    _pauseReason = nil;
    pthread_mutex_unlock(&_lock);

    [audioToStop stop];

    if (checkpointCompletion) {
        BOOL saved = checkpointSaved;
        NSString *message = saved
            ? @"The machine was saved and will resume here next time."
            : (checkpointFailure ?:
               @"The machine stopped before its checkpoint could be completed.");
        dispatch_async(dispatch_get_main_queue(), ^{
            checkpointCompletion(saved, message);
        });
    }
    if (stopCompletion)
        dispatch_async(dispatch_get_main_queue(), stopCompletion);
}

// Called on the emulator thread only.
- (void)publishRetired:(uint64_t)retired
                  rate:(double)instantRate
                status:(arm_status_t)status {
    // Ask the display controller where the framebuffer is and how it is laid
    // out, rather than assuming. vm_guest_display() validates that the reported
    // buffer is inside DRAM and no larger than VM_FB_BYTES before returning it.
    uint32_t fbStride = 0, fbW = 0, fbH = 0;
    vm_pixel_order_t order = VM_ORDER_BGRA;
    const uint8_t *fb = vm_guest_display(&_machine, &fbW, &fbH, &fbStride, &order);
    size_t fbBytes = 0;
    if (!fb || fbW == 0 || fbH == 0 || fbW > SIZE_MAX / VM_FB_BPP ||
        fbStride < (size_t)fbW * VM_FB_BPP || fbStride > SIZE_MAX / fbH) {
        fb = NULL;
    } else {
        fbBytes = (size_t)fbStride * fbH;
        if (fbBytes == 0 || fbBytes > VM_FB_BYTES) fb = NULL;
    }

    // Drain the UART before taking the lock's contents out of the machine:
    // core's tx buffer is a fixed 8 KB that stops accepting bytes when full, so
    // whoever is watching has to empty it or the guest goes quiet.
    NSString *fresh = nil;
    if (_machine.uart0.tx_len > 0) {
        fresh = [[NSString alloc] initWithBytes:_machine.uart0.tx
                                         length:_machine.uart0.tx_len
                                       encoding:NSISOLatin1StringEncoding];
        _machine.uart0.tx_len = 0;
    }

    NSString *statusText;
    switch (status) {
        case ARM_OK:        statusText = @"running";        break;
        case ARM_UNDEFINED:
            statusText = [NSString stringWithFormat:@"undefined insn at 0x%08x",
                                                    _machine.cpu.r[15]];
            break;
        case ARM_HALT:
            statusText = [NSString stringWithFormat:@"halted at 0x%08x",
                                                    _machine.cpu.r[15]];
            break;
        default:            statusText = @"?";              break;
    }

    pthread_mutex_lock(&_lock);
    if (fb && _snapshot) {
        /* Re-upload only exact changes, including geometry and pixel order.
         * The sampled FPS signature below is a measurement, never permission
         * to discard a small pixel edit. Preserve an unread publication when
         * another identical scanout arrives before the UI consumes it. */
        vm_frame_publication_layout_t published = {
            _snapshotWidth, _snapshotHeight, _snapshotStride,
            _snapshotARGB, _snapshotBlank
        };
        const vm_frame_publication_layout_t next = {
            fbW, fbH, fbStride, order == VM_ORDER_ARGB, false
        };
        bool pending = _snapshotFresh;
        vm_frame_publication_result_t publication =
            vm_frame_publication_update(_snapshot, VM_FB_BYTES, &published,
                                        &pending, fb, &next);
        /* Counted here, where a frame actually becomes visible, and only when
         * its contents differ from the one before it. */
        uint64_t sig = vm_engine_fb_signature(fb, fbBytes);
        if (!_fbSignatureValid || sig != _fbSignature) {
            _fbSignature = sig;
            _fbSignatureValid = YES;
            _fpsWindowFrames++;
        }
        double nowSec = vm_engine_now_seconds();
        if (_fpsWindowStart <= 0.0) {
            _fpsWindowStart = nowSec;
        } else if (nowSec - _fpsWindowStart >= 0.5) {
            /* A half-second window: long enough that one slow frame does not
             * dominate, short enough to follow a stutter the user can feel. */
            _fps = (double)_fpsWindowFrames / (nowSec - _fpsWindowStart);
            _fpsWindowFrames = 0;
            _fpsWindowStart = nowSec;
        }
        if (publication == VM_FRAME_PUBLICATION_CHANGED) {
            /* The geometry and order travel with the exact pixels above. */
            _snapshotARGB = published.argb;
            _snapshotWidth = published.width;
            _snapshotHeight = published.height;
            _snapshotStride = published.stride;
            _snapshotBlank = published.blank;
        }
        _snapshotFresh = pending;
    } else if (_snapshot && !_snapshotBlank) {
        /* A stopped or invalid controller is a black panel, not permission to
         * leave the last good frame on screen forever. Publish that transition
         * once; do not allocate a new black CGImage at 30 Hz while it remains
         * stopped. */
        [self publishBlankSnapshotLocked];
    }
    if (fresh.length) [_pending appendString:fresh];
    if (_pending.length > kVMConsoleLimit) {
        [_pending deleteCharactersInRange:
            NSMakeRange(0, _pending.length - kVMConsoleLimit)];
    }
    _retired = retired;
    // Smooth the rate: a per-chunk figure jitters too much to read.
    if (_paused && _state == VMEngineStateRunning) {
        /* A pause can land while s5l8900_run() is inside its bounded chunk.
         * That chunk publishes once after the request; it must not overwrite
         * the pause cause with a misleading "running" status. */
        _rate = 0.0;
        _status = [NSString stringWithFormat:@"paused (%@)",
                   _pauseReason.length ? _pauseReason : @"requested"];
    } else {
        _rate = (_rate > 0.0) ? (_rate * 0.8 + instantRate * 0.2) : instantRate;
        _status = statusText;
    }
    pthread_mutex_unlock(&_lock);
}

- (void)appendConsole:(NSString *)text {
    if (!text.length) return;
    pthread_mutex_lock(&_lock);
    [_pending appendString:text];
    pthread_mutex_unlock(&_lock);
}

/* A black panel of the panel's nominal size. MUST be called with _lock already
 * held: it is the tail of four different critical sections, and taking the
 * lock here would deadlock every one of them. */
- (void)publishBlankSnapshotLocked {
    if (!_snapshot) return;
    memset(_snapshot, 0, VM_FB_BYTES);
    _snapshotARGB = NO;
    _snapshotFresh = YES;
    _snapshotBlank = YES;
    _snapshotWidth  = VM_FB_WIDTH;
    _snapshotHeight = VM_FB_HEIGHT;
    _snapshotStride = VM_FB_WIDTH * VM_FB_BPP;
}

#pragma mark - Snapshot readers (main thread)

- (BOOL)copyFrameInto:(void *)dst
             capacity:(size_t)capacity
                width:(uint32_t *)outWidth
               height:(uint32_t *)outHeight
               stride:(uint32_t *)outStride
                 argb:(BOOL *)outARGB {
    if (!dst || capacity < VM_FB_BYTES) return NO;
    BOOL copied = NO;
    pthread_mutex_lock(&_lock);
    if (_snapshotFresh && _snapshot) {
        memcpy(dst, _snapshot, VM_FB_BYTES);
        if (outARGB)   *outARGB   = _snapshotARGB;
        if (outWidth)  *outWidth  = _snapshotWidth;
        if (outHeight) *outHeight = _snapshotHeight;
        if (outStride) *outStride = _snapshotStride;
        _snapshotFresh = NO;
        copied = YES;
    }
    pthread_mutex_unlock(&_lock);
    return copied;
}

- (NSString *)takePendingConsoleText {
    NSString *out = nil;
    pthread_mutex_lock(&_lock);
    if (_pending.length) {
        out = [_pending copy];
        [_pending setString:@""];
    }
    pthread_mutex_unlock(&_lock);
    return out;
}

/* Emulator thread only, between chunks. */
- (void)publishDiagnostics_emulatorThread:(uint64_t)retired
                                    runNs:(uint64_t)runNs
                                   idleNs:(uint64_t)idleNs
                                 threadNs:(uint64_t)threadNs {
    arm_ci_stats_t stats;
    BOOL active = _machine.ci != NULL;
    if (active) arm_ci_get_stats(_machine.ci, &stats);
    else memset(&stats, 0, sizeof stats);
    pthread_mutex_lock(&_lock);
    _diagCiActive = active;
    _diagCiStats = stats;
    _diagRetired = retired;
    _diagRunNs = runNs;
    _diagIdleNs = idleNs;
    _diagThreadNs = threadNs;
    memcpy(_diagUnmodelled, _machine.unmodelled, sizeof _diagUnmodelled);
    memcpy(_diagMmio, _machine.mmio_recent, sizeof _diagMmio);
    _diagAudioPcLo = _machine.audio_pc_lo;
    _diagAudioPcHi = _machine.audio_pc_hi;
    _diagAudioAccesses = _machine.audio_accesses;
    memcpy(_diagPcm, _machine.pcm_recent, sizeof _diagPcm);
    _diagPcmPcLo = _machine.pcm_pc_lo;
    _diagPcmPcHi = _machine.pcm_pc_hi;
    _diagPcmAccesses = _machine.pcm_accesses;
    memcpy(_diagDmac, _machine.dmac, sizeof _diagDmac);
    memcpy(_diagI2s, _machine.i2s, sizeof _diagI2s);
    memcpy(&_diagCodec, &_machine.codec, sizeof _diagCodec);
    pthread_mutex_unlock(&_lock);
}

- (NSString *)diagnosticsDescription {
    arm_ci_stats_t stats;
    s5l_access_entry_t unmodelled[S5L_ACCESS_LOG], mmio[S5L_ACCESS_LOG];
    pthread_mutex_lock(&_lock);
    BOOL active = _diagCiActive;
    uint64_t retired = _diagRetired;
    NSString *note = _diagBackendNote;
    const uint64_t runNs = _diagRunNs, idleNs = _diagIdleNs, threadNs = _diagThreadNs;
    stats = _diagCiStats;
    memcpy(unmodelled, _diagUnmodelled, sizeof unmodelled);
    memcpy(mmio, _diagMmio, sizeof mmio);
    pthread_mutex_unlock(&_lock);

    char engine[1024], devices[4096], missing[4096], timing[512];
    {
        /* Busy = executing guest instructions (inside the machine, not
         * asleep in guest idle). A low busy share means the guest is idle or
         * the thread is starved; a high one with a low rate means the CPU
         * emulation itself is the limit. */
        const double thread = threadNs / 1e9, run = runNs / 1e9, idle = idleNs / 1e9;
        const double busy = run > idle ? run - idle : 0.0;
        (void)snprintf(timing, sizeof timing,
            "Emulator thread over %.0f s: %.0f%% executing guest code, %.0f%% asleep "
            "while the guest idles (WFI), %.0f%% outside the machine. Rate while "
            "executing: %.1f M insn/s.\n",
            thread,
            thread > 0 ? 100.0 * busy / thread : 0.0,
            thread > 0 ? 100.0 * (run - busy) / thread : 0.0,
            thread > 0 ? 100.0 * (thread > run ? thread - run : 0.0) / thread : 0.0,
            busy > 0 ? retired / busy / 1e6 : 0.0);
    }
    if (active)
        (void)arm_ci_describe_stats(&stats, retired, engine, sizeof engine);
    else
        (void)snprintf(engine, sizeof engine, "Cached interpreter not running.\n");
    (void)s5l_access_log_describe(mmio, S5L_ACCESS_LOG, 32u, devices, sizeof devices);
    (void)s5l_access_log_describe(unmodelled, S5L_ACCESS_LOG, 32u,
                                  missing, sizeof missing);
    return [NSString stringWithFormat:
        @"%s\nCPU engine:\n%s%@%@\n"
        @"Recent hardware accesses, any device (most recent first):\n%s\n"
        @"Recent accesses to unmodelled hardware (most recent first):\n%s",
        timing, engine, active || !note ? @"" : note, active || !note ? @"" : @"\n",
        devices, missing];
}

/*
 * The kernel code around every guest pc that touched the audio block (AMC
 * registers and SRAM), from this machine's own RAM: the kernel maps
 * 0xc0000000 -> physical 0x08000000 linearly. Opt-in and copied by the
 * user; it is their firmware and it is never stored by S5LBox. Code pages do
 * not change while the guest runs, so reading them from this thread is safe.
 */
/* The lowest and highest kernel pc seen touching the audio block (AMC
 * registers or its SRAM): the machine's permanent record, widened by the
 * rolling access logs. NO when no kernel code has touched it. */
- (BOOL)audioPcRangeLo:(uint32_t *)outLo hi:(uint32_t *)outHi
              accesses:(uint64_t *)outAccesses {
    s5l_access_entry_t logs[2 * S5L_ACCESS_LOG];
    pthread_mutex_lock(&_lock);
    memcpy(logs, _diagMmio, sizeof _diagMmio);
    memcpy(logs + S5L_ACCESS_LOG, _diagUnmodelled, sizeof _diagUnmodelled);
    const uint64_t accesses = _diagAudioAccesses;
    const uint32_t seenLo = _diagAudioPcLo, seenHi = _diagAudioPcHi;
    pthread_mutex_unlock(&_lock);

    /* The machine's permanent record first; the rolling logs only add to it. */
    uint32_t lo = accesses ? seenLo : UINT32_MAX, hi = accesses ? seenHi : 0u;
    for (size_t i = 0; i < 2u * S5L_ACCESS_LOG; i++) {
        const s5l_access_entry_t *e = &logs[i];
        if (!e->count) continue;
        const BOOL amc = e->addr >= S5L8900_AMC_BASE && e->addr - S5L8900_AMC_BASE < S5L8900_AMC_SIZE;
        const BOOL sram = e->addr >= S5L8900_SRAM_BASE && e->addr - S5L8900_SRAM_BASE < S5L8900_SRAM_SIZE;
        if (!amc && !sram) continue;
        if (e->pc < 0xc0000000u || e->pc >= 0xc0800000u) continue;
        if (e->pc < lo) lo = e->pc;
        if (e->pc > hi) hi = e->pc;
    }
    *outLo = lo;
    *outHi = hi;
    *outAccesses = accesses;
    return lo <= hi;
}

- (NSString *)audioDriverExcerpt {
    uint32_t lo = 0, hi = 0;
    uint64_t accesses = 0;
    if (![self audioPcRangeLo:&lo hi:&hi accesses:&accesses])
        return @"No kernel code has touched the audio block yet in this run. Play a sound (Settings > Sounds), then try again.";
    /* The register accessors are small leaf functions; the driver logic that
     * calls them (firmware load, start sequence, the "could not start DMA"
     * decision) lies before them in the same kext, so take 16 KiB before the
     * first pc and 8 KiB after the last; bounded at 32 KiB. */
    uint32_t start = (lo - 0x4000u) & ~0xfu, end = (hi + 0x2000u + 0xfu) & ~0xfu;
    if (end - start > 0x8000u) end = start + 0x8000u;
    const uint64_t pa = (uint64_t)start - 0xc0000000u + 0x08000000u;
    if (!_machine.ram || pa < _machine.ram_base ||
        pa + (end - start) > (uint64_t)_machine.ram_base + _machine.ram_size)
        return @"The kernel is not where this build expects it in guest RAM.";
    NSData *bytes = [NSData dataWithBytes:_machine.ram + (pa - _machine.ram_base) length:end - start];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.bytes, (CC_LONG)bytes.length, digest);
    NSMutableString *hex = [NSMutableString string];
    for (size_t i = 0; i < sizeof digest; i++) [hex appendFormat:@"%02x", digest[i]];
    return [NSString stringWithFormat:
        @"S5LBox audio driver excerpt (kernel code from this machine's own firmware, for register analysis; not stored by S5LBox)\n"
        @"va=0x%08x len=0x%x sha256=%@ pcs=0x%08x..0x%08x accesses=%llu\n%@\n",
        start, end - start, hex, lo, hi, (unsigned long long)accesses,
        [bytes base64EncodedStringWithOptions:NSDataBase64Encoding76CharacterLineLength]];
}

/*
 * WHERE THE GUEST'S INSTRUCTIONS WENT: the pcs sampled since the previous
 * call (or since this engine was made), named by library and function.
 *
 * Kernel addresses are named from the imported kernelcache (ksyms.h: kernel
 * symbols, and which prelinked kext owns an address). User addresses are
 * named from the dyld shared cache FILE on the pristine rootfs.img -- every
 * framework and libSystem lives there, at the same address in every process
 * -- read-only, the same way the Crash Logs screen reads the work image.
 * Code nothing can name is reported as its 256-byte block, with the address,
 * rather than attributed to the nearest symbol.
 *
 * Each sample stands for kVMChunkInstructions retired instructions, so a
 * share of samples is a share of guest instructions. Chunks that ended early
 * (the guest went idle, or the run stopped) are counted, not sampled: this is
 * a profile of busy time.
 */
/* The imported kernelcache's symbols and kext map, or NULL. ksyms points into
 * *keep, which the caller must hold for as long as it uses the result. */
static ksyms_t *VMLoadKernelSymbols(NSString *firmwareDir, NSData **keep) {
    *keep = nil;
    if (!firmwareDir.length) return NULL;
    NSData *kernel = [NSData dataWithContentsOfFile:
                         [firmwareDir stringByAppendingPathComponent:@VM_FW_BOOT_KERNEL_FILE]
                                            options:NSDataReadingMappedIfSafe error:NULL];
    ksyms_t *ks = kernel.length ? calloc(1, sizeof *ks) : NULL;
    if (!ks) return NULL;
    (void)ksyms_load(ks, kernel.bytes, kernel.length);
    *keep = kernel;
    return ks;
}

static void VMFreeKernelSymbols(ksyms_t *ks) {
    if (!ks) return;
    ksyms_free(ks);
    free(ks);
}

static NSString *VMProfileBlock(uint32_t pc) {
    return [NSString stringWithFormat:@"code @0x%08x (256 B)", pc & ~0xffu];
}

static void VMProfileAdd(NSMutableDictionary<NSString *, NSNumber *> *d,
                         NSString *key, uint64_t n) {
    d[key] = @(d[key].unsignedLongLongValue + n);
}

static NSArray<NSString *> *VMProfileTop(NSDictionary<NSString *, NSNumber *> *d,
                                         NSUInteger limit) {
    NSArray<NSString *> *keys = [d keysSortedByValueUsingComparator:
        ^NSComparisonResult(NSNumber *a, NSNumber *b) { return [b compare:a]; }];
    return keys.count > limit ? [keys subarrayWithRange:NSMakeRange(0, limit)] : keys;
}

/*
 * "Which process": each program's share, named by its exec path, then the
 * top functions inside the busiest ones. Counts are samples taken while one
 * of its address spaces (TTBR0) was loaded, so a process's kernel share is
 * the kernel working for it (system calls, faults, interrupts that landed
 * during its slice). A program seen in more than one address space was
 * started more than once in the window -- a daemon that keeps crashing and
 * being relaunched shows up here as a count, not as rows to add up.
 */
static NSString *VMProfileProcesses(const gprof_t *w,
                                    NSArray<NSMutableDictionary<NSString *, NSNumber *> *> *procFunctions,
                                    double total) {
    NSMutableString *out = [NSMutableString string];
    /* Group by name; index 0 and the unnamed spaces are groups of their own. */
    NSString *const kUnnamed = @"(no exec path readable: kernel task or not yet mapped)";
    NSMutableDictionary<NSString *, NSMutableArray<NSNumber *> *> *groups = [NSMutableDictionary dictionary];
    for (uint32_t i = 0; i < w->nproc; i++) {
        if (!w->proc[i].samples) continue;
        NSString *name = i == 0 ? @"(MMU off, or past the process table)"
                       : w->proc[i].name[0] ? [NSString stringWithUTF8String:w->proc[i].name] : kUnnamed;
        if (!name.length) name = kUnnamed;                   /* not UTF-8 */
        NSMutableArray<NSNumber *> *members = groups[name];
        if (!members) groups[name] = members = [NSMutableArray array];
        [members addObject:@(i)];
    }
    uint64_t (^samplesOf)(NSString *) = ^uint64_t(NSString *name) {
        uint64_t n = 0;
        for (NSNumber *i in groups[name]) n += w->proc[i.unsignedIntValue].samples;
        return n;
    };
    NSArray<NSString *> *order = [groups.allKeys sortedArrayUsingComparator:
        ^NSComparisonResult(NSString *a, NSString *b) {
            const uint64_t ca = samplesOf(a), cb = samplesOf(b);
            return ca == cb ? [a compare:b] : (ca > cb ? NSOrderedAscending : NSOrderedDescending);
        }];
    NSString *(^label)(NSString *) = ^NSString *(NSString *name) {
        NSArray<NSNumber *> *members = groups[name];
        const uint32_t first = members.firstObject.unsignedIntValue;
        if (first == 0) return name;
        return members.count == 1
            ? [NSString stringWithFormat:@"%@  (ttbr0 %08x)", name, w->proc[first].ttbr0]
            : [NSString stringWithFormat:@"%@  (%lu address spaces)", name, (unsigned long)members.count];
    };
    [out appendFormat:@"\nBy process (the address spaces samples ran in, by exec path; %u seen%s; "
                      @"\"seen a-b%%\" is where in the window it was first and last sampled)\n",
        w->nproc - 1u, w->proc_full ? ", table full" : ""];
    /* "seen a-b%": where in the window its first and latest samples fell,
     * so a program still busy when the report was taken reads "...-100%". */
    for (NSUInteger k = 0; k < order.count && k < 16; k++) {
        uint64_t n = 0, user = 0, first = UINT64_MAX, last = 0;
        for (NSNumber *i in groups[order[k]]) {
            const gprof_proc_t *e = &w->proc[i.unsignedIntValue];
            n += e->samples;
            user += e->user;
            if (e->first && e->first < first) first = e->first;
            if (e->last > last) last = e->last;
        }
        NSString *span = w->samples && first <= last
            ? [NSString stringWithFormat:@"  seen %.0f-%.0f%%", 100.0 * (first - 1u) / w->samples,
                                         100.0 * last / w->samples]
            : @"";
        [out appendFormat:@"%6.2f%%  %@  [user %.0f%%]%@\n", 100.0 * n / total,
            label(order[k]), 100.0 * user / n, span];
    }
    [out appendString:@"\nTop functions in the busiest processes\n"];
    for (NSUInteger k = 0; k < order.count && k < 6; k++) {
        if (100.0 * samplesOf(order[k]) / total < 1.0) break;
        [out appendFormat:@"%@\n", label(order[k])];
        NSMutableDictionary<NSString *, NSNumber *> *f = [NSMutableDictionary dictionary];
        for (NSNumber *i in groups[order[k]]) {
            NSDictionary<NSString *, NSNumber *> *one = procFunctions[i.unsignedIntValue];
            for (NSString *key in one) VMProfileAdd(f, key, one[key].unsignedLongLongValue);
        }
        for (NSString *key in VMProfileTop(f, 6))
            [out appendFormat:@"  %6.2f%%  %@\n", 100.0 * f[key].unsignedLongLongValue / total, key];
    }
    return out;
}

/*
 * What the call stacks add: inclusive time (a function and everything it
 * called, each function counted once per stack) and, for the hottest
 * functions, the call paths that reach them. A return address names the
 * call site, so it is looked up two bytes back (inside the calling
 * function even when the call is its last instruction).
 */
static NSString *VMProfileStacks(const gprof_t *w,
                                 void (^classify)(uint32_t, NSString **, NSString **),
                                 NSArray<NSString *> *hottest, double total) {
    if (!w->stack || !w->stack_samples) return @"";
    NSMutableDictionary<NSNumber *, NSString *> *names = [NSMutableDictionary dictionary];
    NSString *(^nameOf)(uint32_t) = ^NSString *(uint32_t a) {
        NSNumber *key = @(a);
        NSString *n = names[key];
        if (!n) {
            NSString *image = nil, *function = nil;
            classify(a, &image, &function);
            n = [NSString stringWithFormat:@"%@  %@", image, function];
            names[key] = n;
        }
        return n;
    };
    NSMutableDictionary<NSString *, NSNumber *> *inclusive = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSString *, NSMutableDictionary<NSString *, NSNumber *> *> *paths =
        [NSMutableDictionary dictionary];
    for (NSString *f in hottest) paths[f] = [NSMutableDictionary dictionary];
    for (uint32_t i = 0; i < w->stack_cap; i++) {
        const gprof_stack_t *e = &w->stack[i];
        if (!e->count || !e->depth) continue;
        NSMutableArray<NSString *> *chain = [NSMutableArray array];
        for (unsigned k = 0; k < e->depth; k++) {
            const uint32_t a = k && e->frame[k] >= 2u ? e->frame[k] - 2u : e->frame[k];
            NSString *n = nameOf(a);
            if (![chain.lastObject isEqualToString:n]) [chain addObject:n];
        }
        for (NSString *f in [NSSet setWithArray:chain]) VMProfileAdd(inclusive, f, e->count);
        NSMutableDictionary<NSString *, NSNumber *> *into = paths[chain[0]];
        if (into) {
            const NSUInteger callers = MIN((NSUInteger)6, chain.count - 1u);
            NSString *path = callers
                ? [[chain subarrayWithRange:NSMakeRange(1, callers)] componentsJoinedByString:@"\n        <- "]
                : @"(no caller recorded)";
            VMProfileAdd(into, path, e->count);
        }
    }
    NSMutableString *out = [NSMutableString string];
    [out appendFormat:@"\nInclusive (the function and everything it called; %llu stacks, %llu not kept: table full)\n",
        (unsigned long long)w->stack_samples, (unsigned long long)w->stack_dropped];
    for (NSString *key in VMProfileTop(inclusive, 30))
        [out appendFormat:@"%6.2f%%  %@\n", 100.0 * inclusive[key].unsignedLongLongValue / total, key];
    [out appendString:@"\nCall paths into the hottest functions (r7 frame chain, innermost caller first)\n"];
    for (NSString *f in hottest) {
        NSMutableDictionary<NSString *, NSNumber *> *into = paths[f];
        [out appendFormat:@"%@\n", f];
        for (NSString *key in VMProfileTop(into, 4))
            [out appendFormat:@"  %6.2f%%  <- %@\n", 100.0 * into[key].unsignedLongLongValue / total, key];
    }
    return out;
}

static NSString *VMGuestProfileReport(const gprof_t *window, uint64_t shortChunks,
                                      double seconds, NSString *firmwareDir,
                                      NSString *revision) {
    NSMutableString *out = [NSMutableString string];
    const uint64_t kept = window->samples - window->dropped;
    [out appendFormat:
        @"S5LBox guest profile (the guest pc sampled after every %u retired instructions)\n"
        @"Build: %@\nWindow: %.1f s, %llu samples = %.1f M instructions; %llu shorter chunks (guest idle or a stop) not sampled\n",
        kVMChunkInstructions, revision, seconds,
        (unsigned long long)window->samples,
        window->samples * (double)kVMChunkInstructions / 1e6,
        (unsigned long long)shortChunks];
    if (!window->samples) {
        [out appendString:@"\nNo samples yet. Use the guest for 30-60 seconds, then copy the profile again.\n"];
        return out;
    }
    [out appendFormat:@"User mode %.1f%%, kernel %.1f%%; %llu samples dropped (table full)\n",
        100.0 * window->user / window->samples,
        100.0 * (window->samples - window->user) / window->samples,
        (unsigned long long)window->dropped];

    /* Names: the kernelcache for kernel pcs. ksyms points into these bytes,
     * so they must outlive every ksyms call below, not just the last use ARC
     * can see. */
    NS_VALID_UNTIL_END_OF_SCOPE NSData *kernel = nil;
    ksyms_t *ks = VMLoadKernelSymbols(firmwareDir, &kernel);
    const BOOL ksLoaded = ks != NULL;
    if (ksLoaded)
        [out appendFormat:@"Kernel names: %u symbols (%s), %u kexts (%s)\n",
            ks->nsym, ksyms_strerror(ks->sym_status), ks->nkext,
            ksyms_strerror(ks->prelink_status)];
    else
        [out appendString:@"Kernel names: unavailable (no imported kernel.macho)\n"];

    /* ...and the dyld shared cache for user pcs. */
    static const char kCachePath[] =
        "/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6";
    uint8_t *cacheBytes = NULL;
    gprof_cache_t cache;
    BOOL cacheOpen = NO;
    memset(&cache, 0, sizeof cache);
    char rootfs[VM_FW_BOOT_PATH_CAPACITY];
    rootfs_work_result_t *rr = calloc(1, sizeof *rr);
    if (rr && firmwareDir.length &&
        [[firmwareDir stringByAppendingPathComponent:@VM_FW_BOOT_ROOTFS_FILE]
            getFileSystemRepresentation:rootfs maxLength:sizeof rootfs]) {
        size_t got = 0;
        uint64_t size = 0;
        rootfs_work_status_t rs =
            rootfs_work_read_file(rootfs, kCachePath, NULL, 0, &got, &size, rr);
        if (rs == ROOTFS_WORK_OK && size && size <= (UINT64_C(512) << 20)) {
            cacheBytes = malloc((size_t)size);
            if (cacheBytes)
                rs = rootfs_work_read_file(rootfs, kCachePath, cacheBytes,
                                           (size_t)size, &got, &size, rr);
        }
        if (rs == ROOTFS_WORK_OK && cacheBytes && got) {
            cacheOpen = gprof_cache_open(&cache, cacheBytes, got);
            [out appendFormat:@"User names: dyld shared cache, %.1f MB, %u images%s%s\n",
                got / 1048576.0, cacheOpen ? cache.nimage : 0u,
                cache.detail[0] ? "; " : "", cache.detail];
        } else {
            [out appendFormat:@"User names: unavailable (%s: %s)\n",
                rootfs_work_status_name(rs), rr->detail[0] ? rr->detail : "no detail"];
        }
    } else {
        [out appendString:@"User names: unavailable (no imported rootfs.img)\n"];
    }
    free(rr);

    NSMutableDictionary<NSString *, NSNumber *> *images = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSString *, NSNumber *> *functions = [NSMutableDictionary dictionary];
    /* A pc can have one slot per process; the address list sums them. */
    NSMutableDictionary<NSNumber *, NSNumber *> *addresses = [NSMutableDictionary dictionary];
    NSMutableArray<NSMutableDictionary<NSString *, NSNumber *> *> *procFunctions =
        [NSMutableArray arrayWithCapacity:window->nproc];
    for (uint32_t i = 0; i < window->nproc; i++)
        [procFunctions addObject:[NSMutableDictionary dictionary]];
    /* Library and function for one address, the same naming everywhere
     * below. Only the report's own thread uses it, so the caches need no lock. */
    gprof_cache_t *cacheRef = &cache;   /* symbolizing fills its tables */
    void (^classify)(uint32_t, NSString **, NSString **) =
        ^(uint32_t pc, NSString **imageOut, NSString **functionOut) {
        NSString *image = nil, *function = nil;
        if (pc >= 0xffff0000u) {
            image = @"exception vectors";
            function = VMProfileBlock(pc);
        } else if (pc >= 0xc0000000u) {
            const kext_t *kext = ksLoaded ? ksyms_kext_at(ks, pc) : NULL;
            if (kext) {
                image = [NSString stringWithUTF8String:kext->bundle] ?: @"(kext)";
                function = VMProfileBlock(pc);
            } else {
                char name[256];
                const char *r = ksLoaded ? ksyms_resolve(ks, pc, name, sizeof name) : "?";
                image = @"mach_kernel";
                if (r[0] == '?' || !strncmp(r, "__PRELINK_TEXT", 14)) {
                    function = VMProfileBlock(pc);
                } else {
                    const char *plus = strstr(r, "+0x");
                    function = [[NSString alloc] initWithBytes:r
                        length:plus ? (NSUInteger)(plus - r) : strlen(r)
                        encoding:NSUTF8StringEncoding] ?: VMProfileBlock(pc);
                }
            }
        } else {
            gprof_image_t *img = cacheOpen ? gprof_cache_image_at(cacheRef, pc) : NULL;
            if (img) {
                uint32_t off = 0;
                const char *sym = gprof_cache_symbolize(cacheRef, img, pc, 0x8000u, &off);
                image = [NSString stringWithUTF8String:gprof_basename(img->path)] ?: @"(image)";
                function = sym ? ([NSString stringWithUTF8String:sym] ?: VMProfileBlock(pc))
                               : VMProfileBlock(pc);
            } else if (pc >= 0x2fe00000u && pc < 0x30000000u) {
                image = @"dyld (by address)";
                function = VMProfileBlock(pc);
            } else {
                image = @"user code outside the shared cache (app or plugin)";
                function = VMProfileBlock(pc);
            }
        }
        *imageOut = image;
        *functionOut = function;
    };
    for (uint32_t i = 0; i < window->cap; i++) {
        const gprof_slot_t *slot = &window->slot[i];
        if (!slot->count) continue;
        const uint32_t pc = slot->pc;
        NSString *image = nil, *function = nil;
        classify(pc, &image, &function);
        NSString *both = [NSString stringWithFormat:@"%@  %@", image, function];
        VMProfileAdd(images, image, slot->count);
        VMProfileAdd(functions, both, slot->count);
        if (slot->proc < procFunctions.count)
            VMProfileAdd(procFunctions[slot->proc], both, slot->count);
        NSNumber *key = @(pc);
        addresses[key] = @(addresses[key].unsignedLongLongValue + slot->count);
    }
    NSArray<NSNumber *> *hot = [addresses keysSortedByValueUsingComparator:^NSComparisonResult(NSNumber *a, NSNumber *b) {
        return [b compare:a];
    }];

    const double total = (double)kept;
    [out appendString:@"\nBy library / kext (share of sampled instructions)\n"];
    for (NSString *key in VMProfileTop(images, 16))
        [out appendFormat:@"%6.2f%%  %@\n", 100.0 * images[key].unsignedLongLongValue / total, key];
    [out appendString:@"\nBy function\n"];
    for (NSString *key in VMProfileTop(functions, 40))
        [out appendFormat:@"%6.2f%%  %@\n", 100.0 * functions[key].unsignedLongLongValue / total, key];
    [out appendString:@"\nHottest single addresses\n"];
    for (NSUInteger i = 0; i < hot.count && i < 12; i++)
        [out appendFormat:@"%6.2f%%  0x%08x\n",
            100.0 * addresses[hot[i]].unsignedLongLongValue / total, hot[i].unsignedIntValue];

    [out appendString:VMProfileProcesses(window, procFunctions, total)];
    [out appendString:VMProfileStacks(window, classify, VMProfileTop(functions, 3), total)];

    if (cacheOpen) gprof_cache_close(&cache);
    free(cacheBytes);
    VMFreeKernelSymbols(ks);
    return out;
}

/*
 * One profile sample, attributed to the process it ran in (guest_profile.h):
 * the address space is the TTBR0, and its name is read from the guest's own
 * RAM -- a page-table walk and one 8 KiB scan -- only when TTBR0 changed since
 * the last sample or kVMProfileReread samples have passed, so a task that
 * exits and hands its tables to a new one is noticed. The call stack is the
 * r7 frame chain, read the same way. Emulator thread, with _profileLock held,
 * between chunks.
 */
static const uint16_t kVMProfileReread = 64u;
static const uint32_t kVMUserStackTop = 0x30000000u;   /* iPhone OS 3 USRSTACK */

- (void)profileSample_emulatorThread {
    const arm_cp15_t *cp = &_machine.cpu.cp15;
    const bool mmu = (cp->sctlr & 1u) != 0;
    const uint32_t ttbr0 = mmu ? cp->ttbr0 : 0u;
    uint16_t proc = 0;
    if (mmu && !gprof_proc_cached(&_profile, ttbr0, kVMProfileReread, &proc)) {
        char path[GPROF_NAME_MAX];
        const gprof_ram_t pathRam = { _machine.ram, _machine.ram_base, _machine.ram_size };
        gprof_exec_path(&pathRam, cp->ttbr0, cp->ttbr1, cp->ttbcr, kVMUserStackTop,
                        path, sizeof path);
        proc = gprof_proc_intern(&_profile, ttbr0, path);
    }
    const uint32_t pc = _machine.cpu.r[15];
    gprof_note_in(&_profile, pc,
                  (_machine.cpu.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR, proc);
    uint32_t frames[GPROF_STACK_MAX];
    const gprof_ram_t ram = { _machine.ram, _machine.ram_base, _machine.ram_size };
    const unsigned depth = mmu
        ? gprof_backtrace(&ram, cp->ttbr0, cp->ttbr1, cp->ttbcr, pc,
                          _machine.cpu.r[14], _machine.cpu.r[7], frames, GPROF_STACK_MAX)
        : 0u;
    gprof_note_stack(&_profile, frames, depth, proc);
}

- (void)guestProfileReportWithCompletion:(void (^)(NSString *report))completion {
    if (!completion) return;
    gprof_t *window = calloc(1, sizeof *window);
    BOOL have = NO;
    uint64_t shortChunks = 0, sinceNs = 0;
    const uint64_t nowNs = vm_now_ns();
    if (_profileReady && window && gprof_init(window, kVMProfileSlotsLog2) &&
        gprof_init_stacks(window, kVMProfileStacksLog2)) {
        pthread_mutex_lock(&_profileLock);
        have = gprof_copy(window, &_profile);
        shortChunks = _profileShort;
        sinceNs = _profileSinceNs;
        if (have) {
            gprof_reset(&_profile);
            _profileShort = 0;
            _profileSinceNs = nowNs;
        }
        pthread_mutex_unlock(&_profileLock);
    }
    if (!have) {
        if (window) gprof_free(window);
        free(window);
        completion(@"The guest profile is unavailable: its table could not be allocated.");
        return;
    }
    NSString *firmwareDir = [[VMSettings sharedSettings] firmwareDirectory];
    NSString *revision = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"S5LBoxSourceRevision"] ?: @"development";
    const double seconds = (nowNs - sinceNs) / 1e9;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSString *text;
        @autoreleasepool {
            text = VMGuestProfileReport(window, shortChunks, seconds, firmwareDir, revision);
        }
        gprof_free(window);
        free(window);
        dispatch_async(dispatch_get_main_queue(), ^{ completion(text); });
    });
}

/*
 * The whole kexts of the audio paths, from this machine's RAM, with the
 * kernel functions they reference named. The kernel's linear mapping is
 * copied here, on the caller's thread, so nothing reads guest RAM after this
 * returns; cutting out each kext and naming run in the background.
 */
/* How much of the kernel's linear mapping is copied: the kernel and every
 * prelinked kext (the last ones end below 0xc0800000 on iPhone OS 3.1.3), so
 * any kext can be cut out once the load map is read. A kext window around the
 * pcs is not enough: AppleEmbeddedAudio and AppleARMPL080DMAC are dumped by
 * name, and an I2S driver's pcs say nothing about where the AMC's kext is. */
static const uint32_t kVMKernelWindow = 0x01000000u;

/* Always dumped when the kernelcache names them: the PCM output driver that
 * prints "could not start DMA", and the DMA controller driver under it. */
static const char *const kVMAudioKextNames[] = {
    "com.apple.driver.AppleEmbeddedAudio",
    "com.apple.driver.AppleARMPL080DMAC",
};

/* SHA-256 of kexts already analysed from an earlier report: printed by name
 * and hash only, so the report stays small enough to paste. A kext whose
 * bytes differ in any way is dumped in full. */
static const char *const kVMAnalysedKexts[] = {
    /* AppleAMC_r1 from iPhone OS 3.1.3: docs/audio.md, 2026-09-24. */
    "d3611f38c830ab6918e312abd200c368529c0823158dff1554c0b33cfe29626f",
};

/* Bytes for the report: raw DEFLATE (RFC 1951; zlib's wbits=-15 inflates it),
 * then base64, since code compresses to about half and SRAM is mostly
 * zeros. Plain base64 if compression is refused, and the header says which. */
static NSString *VMPackedBase64(NSData *raw) {
    NSError *error = nil;
    NSData *packed = [raw compressedDataUsingAlgorithm:NSDataCompressionAlgorithmZlib
                                                 error:&error];
    if (packed.length)
        return [NSString stringWithFormat:@"deflate-raw+base64 (%lu -> %lu bytes; inflate with zlib wbits=-15):\n%@\n",
            (unsigned long)raw.length, (unsigned long)packed.length,
            [packed base64EncodedStringWithOptions:NSDataBase64Encoding76CharacterLineLength]];
    return [NSString stringWithFormat:@"base64 (%lu bytes, uncompressed):\n%@\n",
        (unsigned long)raw.length,
        [raw base64EncodedStringWithOptions:NSDataBase64Encoding76CharacterLineLength]];
}

static NSString *VMDriverKextText(const uint8_t *bytes, uint32_t va, uint32_t len,
                                  const char *label, const ksyms_t *ks) {
    NSMutableString *out = [NSMutableString string];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes, (CC_LONG)len, digest);
    NSMutableString *hex = [NSMutableString string];
    for (size_t i = 0; i < sizeof digest; i++) [hex appendFormat:@"%02x", digest[i]];
    [out appendFormat:@"--- %s va=0x%08x len=0x%x sha256=%@\n", label, va, len, hex];
    for (size_t i = 0; i < sizeof kVMAnalysedKexts / sizeof kVMAnalysedKexts[0]; i++) {
        if (![hex isEqualToString:@(kVMAnalysedKexts[i])]) continue;
        [out appendString:@"bytes omitted: identical to a copy already analysed\n"];
        return out;
    }

    enum { kMaxRefs = 16384 };
    vm_driver_ref_t *refs = calloc(kMaxRefs, sizeof *refs);
    size_t total = 0, n = 0;
    if (refs && ks)
        n = vm_driver_collect_refs(bytes, va, len, 0xc0000000u, 0xc1000000u,
                                   refs, kMaxRefs, &total);
    NSMutableString *named = [NSMutableString string];
    unsigned kept = 0;
    for (size_t i = 0; i < n; i++) {
        const vm_driver_ref_t *r = &refs[i];
        const char *kind = r->kinds == (VM_DRIVER_REF_CALL | VM_DRIVER_REF_WORD) ? "c+w"
                         : r->kinds == VM_DRIVER_REF_CALL ? "call" : "word";
        char name[256];
        const kext_t *other = ksyms_kext_at(ks, r->target);
        if (other) {
            /* Other kexts have no symbols; a call into one is still worth
             * knowing (IOAudioFamily, say), a data word usually is not. */
            if (!(r->kinds & VM_DRIVER_REF_CALL)) continue;
            snprintf(name, sizeof name, "%s+0x%x", other->bundle, r->target - other->addr);
        } else {
            const char *resolved = ksyms_resolve(ks, r->target, name, sizeof name);
            /* Only an exact function entry: decoding every alignment turns
             * data into "branches" that land mid-function. */
            if (resolved[0] == '?' || strstr(resolved, "+0x") || !strncmp(resolved, "__PRELINK", 9))
                continue;
        }
        [named appendFormat:@"0x%08x %-4s x%u %s\n", r->target, kind, r->count, name];
        kept++;
    }
    free(refs);
    [out appendFormat:@"kernel references named: %u (of %zu distinct candidates)\n%@",
        kept, total, named];
    [out appendString:VMPackedBase64([NSData dataWithBytes:bytes length:len])];
    return out;
}

/*
 * The kernel's SHA-1, from _SHA1Init to the first symbol after the last of
 * its entry points (capped at 16 KiB): the code-signing check behind every
 * executable page-in (cs_validate_page -> SHA1UpdateUsePhysicalAddress) and
 * about 14% of guest time in the bc45a3f profiles. Its static block
 * transform has no symbol of its own; profiles name it _SHA1Init. Copied so
 * a native implementation can be checked against the exact code it would
 * replace. nil when the symbols are missing or outside the copied window.
 */
static NSString *VMKernelSha1Text(const ksyms_t *ks, const uint8_t *window,
                                  uint32_t start, uint32_t end) {
    static const char *const kNames[] = {
        "_SHA1Init", "_SHA1Update", "_SHA1UpdateUsePhysicalAddress", "_SHA1Final",
    };
    uint32_t lo = UINT32_MAX, hi = 0;
    for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; i++) {
        const uint32_t v = ksyms_value(ks, kNames[i]) & ~1u;
        if (!v) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (lo > hi) return nil;
    uint32_t stop = hi + 0x400u;            /* no later symbol: a fixed tail */
    for (unsigned i = 0; i < ks->nsym; i++) {
        const uint32_t v = ks->sym[i].value & ~1u;
        if (v > hi && v < stop) stop = v;
    }
    if (stop - lo > 0x4000u) stop = lo + 0x4000u;
    if (lo < start || stop > end) return nil;
    return [NSString stringWithFormat:
        @"KERNEL CODE FOR SPEED WORK (this machine's own kernel, for analysis; not stored by S5LBox)\n"
        @"SHA-1 behind code-signing page checks: _SHA1Init 0x%08x .. 0x%08x\n%@",
        lo, stop, VMDriverKextText(window + (lo - start), lo, stop - lo, "mach_kernel SHA-1", ks)];
}

/*
 * The audio block's own storage as the driver left it. AMC and its SRAM are
 * storage stubs (machine.c), not device models, so this is exactly what the
 * kernel wrote: the AMC registers as a list of the non-zero ones, and the
 * SRAM as the offsets of its non-empty 1 KiB chunks plus the whole image,
 * compressed -- whatever the driver uploaded there, firmware or samples.
 * Copied under no lock: the emulator may be writing, and for a diagnostic a
 * torn word is acceptable.
 */
static NSData *VMStubBytes(const s5l8900_t *m, uint32_t base) {
    for (unsigned i = 0; i < m->stub_count && i < S5L_STUB_MAX; i++) {
        const s5l_stub_t *st = &m->stubs[i];
        if (st->base != base || !st->regs || !st->nregs) continue;
        NSMutableData *d = [NSMutableData dataWithLength:(NSUInteger)st->nregs * 4u];
        uint8_t *out = d.mutableBytes;
        for (uint32_t r = 0; r < st->nregs; r++) {          /* byte i at bits 8i */
            const uint32_t w = st->regs[r];
            out[4u * r] = (uint8_t)w;          out[4u * r + 1u] = (uint8_t)(w >> 8);
            out[4u * r + 2u] = (uint8_t)(w >> 16); out[4u * r + 3u] = (uint8_t)(w >> 24);
        }
        [d setLength:st->size];
        return d;
    }
    return nil;
}

static NSString *VMAudioBlockText(NSData *amc, NSData *sram) {
    NSMutableString *out = [NSMutableString stringWithString:
        @"AUDIO BLOCK STATE (what the kernel wrote to AMC and its SRAM; storage stubs, not a device model)\n"];
    if (!amc) [out appendString:@"amc: no storage window on this machine\n"];
    else {
        const uint8_t *b = amc.bytes;
        unsigned nonzero = 0;
        NSMutableString *regs = [NSMutableString string];
        for (NSUInteger off = 0; off + 4u <= amc.length; off += 4u) {
            const uint32_t v = (uint32_t)b[off] | (uint32_t)b[off + 1u] << 8 |
                               (uint32_t)b[off + 2u] << 16 | (uint32_t)b[off + 3u] << 24;
            if (!v) continue;
            nonzero++;
            [regs appendFormat:@"  +0x%04lx = 0x%08x\n", (unsigned long)off, v];
        }
        [out appendFormat:@"amc 0x%08x len 0x%lx: %u non-zero registers\n%@",
            S5L8900_AMC_BASE, (unsigned long)amc.length, nonzero, regs];
    }
    if (!sram) [out appendString:@"sram: no storage window on this machine\n"];
    else {
        const uint8_t *b = sram.bytes;
        NSMutableString *chunks = [NSMutableString string];
        unsigned kept = 0;
        for (NSUInteger off = 0; off < sram.length; off += 1024u) {
            const NSUInteger len = MIN((NSUInteger)1024u, sram.length - off);
            BOOL any = NO;
            for (NSUInteger i = 0; i < len && !any; i++) any = b[off + i] != 0;
            if (!any) continue;
            kept++;
            [chunks appendFormat:@"%s+0x%05lx", kept == 1u ? "" : " ", (unsigned long)off];
        }
        [out appendFormat:@"sram 0x%08x len 0x%lx: %u non-empty 1 KiB chunks%@%@\n",
            S5L8900_SRAM_BASE, (unsigned long)sram.length, kept,
            kept ? @" at " : @"", chunks];
        if (kept) [out appendString:VMPackedBase64(sram)];
    }
    return out;
}

/* The same record for the PCM output path: kernel pcs that touched either
 * I2S window. NO when none has. */
- (BOOL)pcmPcRangeLo:(uint32_t *)outLo hi:(uint32_t *)outHi
            accesses:(uint64_t *)outAccesses {
    s5l_access_entry_t log[S5L_ACCESS_LOG];
    pthread_mutex_lock(&_lock);
    memcpy(log, _diagPcm, sizeof log);
    const uint64_t accesses = _diagPcmAccesses;
    const uint32_t seenLo = _diagPcmPcLo, seenHi = _diagPcmPcHi;
    pthread_mutex_unlock(&_lock);
    uint32_t lo = accesses ? seenLo : UINT32_MAX, hi = accesses ? seenHi : 0u;
    for (size_t i = 0; i < S5L_ACCESS_LOG; i++) {
        const s5l_access_entry_t *e = &log[i];
        if (!e->count || e->pc < 0xc0000000u || e->pc >= 0xc0800000u) continue;
        if (e->pc < lo) lo = e->pc;
        if (e->pc > hi) hi = e->pc;
    }
    *outLo = lo;
    *outHi = hi;
    *outAccesses = accesses;
    return lo <= hi;
}

/* What the PCM output path's devices hold, formatted from the copies the
 * emulator thread published: the I2S access log, both PL080s, both I2S
 * windows, the codec's written registers and the Ring/Silent switch. */
- (NSString *)pcmPathStateText {
    s5l_access_entry_t log[S5L_ACCESS_LOG];
    s5l_pl080_t dmac[S5L8900_DMAC_COUNT];
    s5l_i2s_t i2s[S5L8900_I2S_COUNT];
    s5l_wm8991_t codec;
    pthread_mutex_lock(&_lock);
    memcpy(log, _diagPcm, sizeof log);
    memcpy(dmac, _diagDmac, sizeof dmac);
    memcpy(i2s, _diagI2s, sizeof i2s);
    memcpy(&codec, &_diagCodec, sizeof codec);
    const uint64_t accesses = _diagPcmAccesses;
    const BOOL silent = _buttons[VMButtonRingerSilent];
    pthread_mutex_unlock(&_lock);
    NSMutableString *out = [NSMutableString stringWithFormat:
        @"PCM PATH STATE (the I2S windows and the DMA controllers that feed them)\n"
        @"I2S accesses from kernel code: %llu. Most recent distinct accesses first:\n",
        (unsigned long long)accesses];
    char text[4096];
    (void)s5l_access_log_describe(log, S5L_ACCESS_LOG, S5L_ACCESS_LOG, text, sizeof text);
    [out appendFormat:@"%s", text];
    static const char *const dmacNames[S5L8900_DMAC_COUNT] = { "dmac0", "dmac1" };
    for (unsigned i = 0; i < S5L8900_DMAC_COUNT; i++) {
        (void)s5l_pl080_describe(&dmac[i], dmacNames[i], text, sizeof text);
        [out appendFormat:@"%s", text];
    }
    static const char *const i2sNames[S5L8900_I2S_COUNT] = { "i2s0", "i2s1" };
    for (unsigned i = 0; i < S5L8900_I2S_COUNT; i++) {
        (void)s5l_i2s_describe(&i2s[i], i2sNames[i], text, sizeof text);
        [out appendFormat:@"%s", text];
    }
    /* The codec the samples go to, and the switch that silences system
     * sounds: the two things between "the engine runs" and "a sound plays"
     * that a report can see. */
    (void)s5l_wm8991_describe(&codec, text, sizeof text);
    [out appendFormat:@"%s", text];
    [out appendFormat:@"Ring/Silent switch as set in the app: %@\n",
        silent ? @"Silent" : @"Ring (or never moved)"];
    return out;
}

- (void)audioDriverDumpWithCompletion:(void (^)(NSString *text))completion {
    if (!completion) return;
    NSData *amcBytes = VMStubBytes(&_machine, S5L8900_AMC_BASE);
    NSData *sramBytes = VMStubBytes(&_machine, S5L8900_SRAM_BASE);
    NSString *pcmState = [self pcmPathStateText];
    uint32_t lo = 0, hi = 0, pcmLo = 0, pcmHi = 0;
    uint64_t accesses = 0, pcmAccesses = 0;
    const BOOL amcSeen = [self audioPcRangeLo:&lo hi:&hi accesses:&accesses];
    const BOOL pcmSeen = [self pcmPcRangeLo:&pcmLo hi:&pcmHi accesses:&pcmAccesses];
    /* The whole kernelcache region, so any kext can be cut out of it once the
     * load map is read (off this thread): the kernel maps 0xc0000000 ->
     * physical 0x08000000 linearly. */
    const uint32_t kbase = 0xc0000000u, pbase = 0x08000000u;
    const uint64_t ramLo = _machine.ram_base, ramHi = ramLo + _machine.ram_size;
    uint32_t start = kbase, end = kbase + kVMKernelWindow;
    if ((uint64_t)pbase < ramLo) start += (uint32_t)(ramLo - pbase);
    if ((uint64_t)end - kbase + pbase > ramHi) end = (uint32_t)(ramHi - pbase + kbase);
    if (!_machine.ram || end <= start) {
        completion([NSString stringWithFormat:@"AUDIO DRIVER: the kernel is not where this build expects it in guest RAM.\n\n%@\n%@",
                    pcmState, VMAudioBlockText(amcBytes, sramBytes)]);
        return;
    }
    const uint64_t pa = (uint64_t)start - kbase + pbase;
    NSData *window = [NSData dataWithBytes:_machine.ram + (pa - ramLo) length:end - start];
    NSString *firmwareDir = [[VMSettings sharedSettings] firmwareDirectory];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSMutableString *out = [NSMutableString string];
        @autoreleasepool {
            [out appendString:@"AUDIO DRIVER (kernel code from this machine's own firmware, for register analysis; not stored by S5LBox)\n"];
            if (amcSeen)
                [out appendFormat:@"audio pcs=0x%08x..0x%08x accesses=%llu\n", lo, hi,
                    (unsigned long long)accesses];
            else
                [out appendString:@"audio pcs: no kernel code has touched the AMC or its SRAM in this run\n"];
            if (pcmSeen)
                [out appendFormat:@"pcm pcs=0x%08x..0x%08x accesses=%llu\n", pcmLo, pcmHi,
                    (unsigned long long)pcmAccesses];
            else
                [out appendString:@"pcm pcs: no kernel code has touched the I2S windows in this run\n"];
            NS_VALID_UNTIL_END_OF_SCOPE NSData *kernel = nil;
            ksyms_t *ks = VMLoadKernelSymbols(firmwareDir, &kernel);
            enum { kMaxKexts = 8 };
            const kext_t *kexts[kMaxKexts];
            unsigned nk = 0;
            const uint32_t owners[4] = { lo, hi, pcmLo, pcmHi };
            const BOOL ownerSeen[4] = { amcSeen, amcSeen, pcmSeen, pcmSeen };
            for (unsigned i = 0; ks && i < 4u; i++) {
                const kext_t *k = ownerSeen[i] ? ksyms_kext_at(ks, owners[i]) : NULL;
                BOOL dup = NO;
                for (unsigned j = 0; j < nk; j++) dup |= kexts[j] == k;
                if (k && !dup && nk < kMaxKexts) kexts[nk++] = k;
            }
            for (size_t n = 0; ks && n < sizeof kVMAudioKextNames / sizeof kVMAudioKextNames[0]; n++) {
                for (unsigned i = 0; i < ks->nkext; i++) {
                    const kext_t *k = &ks->kext[i];
                    if (!k->has_exec || strcmp(k->bundle, kVMAudioKextNames[n])) continue;
                    BOOL dup = NO;
                    for (unsigned j = 0; j < nk; j++) dup |= kexts[j] == k;
                    if (!dup && nk < kMaxKexts) kexts[nk++] = k;
                    break;
                }
            }
            BOOL any = NO;
            for (unsigned i = 0; i < nk; i++) {
                const kext_t *k = kexts[i];
                uint32_t ks0 = k->addr, ks1 = k->addr + k->size;
                BOOL clipped = ks0 < start || ks1 > end;
                if (ks0 < start) ks0 = start;
                if (ks1 > end) ks1 = end;
                if (ks1 <= ks0) continue;
                char label[160];
                snprintf(label, sizeof label, "%s%s", k->bundle,
                         clipped ? " (clipped to the copied window)" : "");
                [out appendString:VMDriverKextText((const uint8_t *)window.bytes + (ks0 - start),
                                                   ks0, ks1 - ks0, label, ks)];
                any = YES;
            }
            if (!any && amcSeen) {
                /* No kext map: the old excerpt's window, named as such. */
                uint32_t w0 = (lo - 0x4000u) & ~0xfu, w1 = (hi + 0x2000u + 0xfu) & ~0xfu;
                if (w0 < start) w0 = start;
                if (w1 > end) w1 = end;
                if (w1 - w0 > 0x8000u) w1 = w0 + 0x8000u;
                if (w1 > w0)
                    [out appendString:VMDriverKextText((const uint8_t *)window.bytes + (w0 - start),
                                                       w0, w1 - w0,
                                                       ks ? "window (no kext owns these pcs)"
                                                          : "window (no kernel.macho to map kexts)",
                                                       ks)];
            } else if (!any) {
                [out appendString:ks ? @"no audio kext found in the kernelcache's load map\n"
                                     : @"no kernel.macho to map kexts\n"];
            }
            NSString *hot = ks ? VMKernelSha1Text(ks, (const uint8_t *)window.bytes, start, end) : nil;
            VMFreeKernelSymbols(ks);
            [out appendFormat:@"\n%@\n%@", pcmState, VMAudioBlockText(amcBytes, sramBytes)];
            if (hot) [out appendFormat:@"\n=== %@", hot];
        }
        dispatch_async(dispatch_get_main_queue(), ^{ completion(out); });
    });
}

- (NSString *)audioStatusDescription {
    pthread_mutex_lock(&_lock);
    VMAudioOutput *output = _audioOutput;
    pthread_mutex_unlock(&_lock);
    if (output) {
        return [output statusDescription];
    }
    return @"Audio output idle";
}

- (void)playAudioTestTone {
    pthread_mutex_lock(&_lock);
    VMAudioOutput *output = _audioOutput;
    pthread_mutex_unlock(&_lock);
    if (output) {
        [output playTestTone];
    }
}

- (NSString *)statusLine {
    pthread_mutex_lock(&_lock);
    uint64_t retired = _retired;
    double rate = _rate;
    NSString *status = _status;
    NSString *mode = _mode;
    double fps = _fps;
    BOOL haveFps = _fbSignatureValid;
    pthread_mutex_unlock(&_lock);

    /* "--" until a frame has actually been published. Printing "0 fps" before
     * anything has rendered would report a stall that is not happening. */
    NSString *fpsText = haveFps ? [NSString stringWithFormat:@"%.0f fps", fps]
                                : @"-- fps";
    double footprintMB = [VMEngine physFootprintBytes] / 1048576.0;
    /* The mode leads, because "3.2 M insn/s" means something different
     * depending on what is retiring them, and a user who cannot see which
     * guest is running has no way to tell. */
    return [NSString stringWithFormat:
            @"%@%@  ·  %.1f M insn  ·  %.2f M insn/s  ·  %@  ·  %.0f MB",
            mode.length ? [mode stringByAppendingString:@"  ·  "] : @"",
            status, retired / 1.0e6, rate / 1.0e6, fpsText, footprintMB];
}

- (NSString *)statusDescription {
    pthread_mutex_lock(&_lock);
    NSString *status = [_status copy];
    pthread_mutex_unlock(&_lock);
    return status.length ? status : @"unknown";
}

- (NSString *)modeDescription {
    pthread_mutex_lock(&_lock);
    NSString *mode = _mode;
    pthread_mutex_unlock(&_lock);
    return mode.length ? mode : nil;
}

- (BOOL)isRunningFirmware {
    /* Deliberately derived from the same string the UI shows rather than from
     * a second flag: two sources of truth is how a UI ends up claiming one
     * thing in the status bar and another in an alert. */
    pthread_mutex_lock(&_lock);
    BOOL firmware = _firmwareBoot != NULL && _mode.length > 0 &&
                    ![_mode isEqualToString:@"built-in test guest"];
    pthread_mutex_unlock(&_lock);
    return firmware;
}

- (NSString *)bringUpNote {
    pthread_mutex_lock(&_lock);
    NSString *note = _bringUpNote;
    pthread_mutex_unlock(&_lock);
    return note.length ? note : nil;
}

- (BOOL)isPreparingRootFilesystem {
    pthread_mutex_lock(&_lock);
    BOOL preparing = _preparingRootFS;
    pthread_mutex_unlock(&_lock);
    return preparing;
}

- (double)rootFilesystemProgress {
    pthread_mutex_lock(&_lock);
    double f = _prepareTotal ? (double)_prepareDone / (double)_prepareTotal
                             : -1.0;
    pthread_mutex_unlock(&_lock);
    return f;
}

/*
 * Called from rootfs_work.c's copy loop, on the PROVISIONING thread -- never
 * the UI one. It therefore does the least possible: takes the same lock every
 * other engine field uses and stores two integers. The view polls; nothing is
 * dispatched from here, because a callback that hopped to the main queue a few
 * hundred times would be a UI storm reporting on a disk copy.
 */
static void vm_engine_prepare_progress(void *ctx, uint64_t done,
                                       uint64_t total) {
    VMEngine *engine = (__bridge VMEngine *)ctx;
    if (!engine) return;
    /* Direct ivar access rather than a message send: this is the same
     * translation unit, and a selector declared below the callback would need
     * a forward declaration for no benefit. Two stores under the lock every
     * engine field already uses. */
    pthread_mutex_lock(&engine->_lock);
    engine->_prepareDone  = done;
    engine->_prepareTotal = total;
    pthread_mutex_unlock(&engine->_lock);
}

/*
 * WHAT A MACHINE WOULD DO, asked without naming a machine.
 *
 * It therefore cannot answer the half that is per-machine. The work image
 * belongs to one machine and lives in that machine's directory, so this probes
 * the SHARED artefacts with an empty work directory -- which reports
 * NEEDS_WORK_IMAGE -- and words that as what it is: every machine prepares its
 * own on first open. Claiming READY here would be claiming a fact about a
 * machine the caller has not identified.
 */
+ (NSString *)firmwareReadinessSummary {
    NSString *directory = [[VMSettings sharedSettings] firmwareDirectory];
    char buffer[VM_FW_BOOT_PATH_CAPACITY];
    BOOL havePath = directory.length &&
        [directory getFileSystemRepresentation:buffer maxLength:sizeof buffer];
    vm_firmware_boot_paths_t paths;
    if (!havePath || !vm_firmware_boot_paths_split(&paths, buffer, ""))
        memset(&paths, 0, sizeof paths);

    vm_firmware_boot_state_t state;
    vm_firmware_boot_probe(&paths, &state);
    switch (state.readiness) {
    case VM_FW_BOOT_READY:
    case VM_FW_BOOT_NEEDS_WORK_IMAGE:
        return @"Firmware imported. Each machine prepares its own writable "
               @"root filesystem the first time you open it, then boots "
               @"iPhone OS 3.1.3.";
    case VM_FW_BOOT_INCOMPLETE:
    default:
        break;
    }
    return @"No firmware imported — runs the built-in test guest, which "
           @"exercises the processor, the serial port and the screen.";
}

@end
