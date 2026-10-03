//
//  NEON — VMN88Engine. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMN88Engine.h"

#import "VMSettings.h"

#include "file_block.h"
#include "n88.h"
#include "n88_ios3.h"
#include "VMTouchQueue.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Instructions per slice: small enough that stop, pause, pacing and input
 * are prompt (a few milliseconds of host time), large enough that the cached
 * interpreter spends its time in guest code. */
static const unsigned kVMN88Slice = 200000u;
/* Asleep, nothing runs, so a slice is only time passing: 20 ms of it at
 * N88_CPU_HZ, which the pacing below then waits out instead of spinning. */
static const unsigned kVMN88SleepSlice = N88_CPU_HZ / 50u;
/* Console text held for the screen between drains. */
static const NSUInteger kVMN88ConsoleLimit = 64u * 1024u;
/* How often the framebuffer is published. */
static const double kVMN88FrameInterval = 1.0 / 30.0;
static const size_t kVMN88FrameBytes = (size_t)N88_FB_STRIDE * N88_FB_HEIGHT;
/*
 * The iOS 6 preview's boot-args. The core's default carries serial=3, which
 * sends the console to the UART, where the desktop harness reads it; the
 * preview has a screen instead, and without serial= the kernel paints its
 * verbose log on the framebuffer (core/include/n88.h). iPhone OS 3 boots with
 * the measured ones (n88_ios3_boot_request): its console comes through the
 * UART to the console screen, and SpringBoard has the framebuffer.
 */
static const char kVMN88Cmdline[] = "debug=0x8 -v";
/* The shortest press, in guest seconds. A UIKit tap can queue a press and its
 * release within one slice; AppleM68Buttons reads every button when one of
 * them interrupts, and a pin already back at rest reads as no change. */
static const double kVMN88MinPress = 0.1;
/* This machine's working root filesystem, in its own folder: the same name
 * the iPhone 3G machines use (VMInstancePaths.h). */
static NSString *const kVMN88WorkImage = @"rootfs-work.img";

#define VM_N88_EDGE_CAP 16u
typedef struct {
    uint8_t input;              /* n88_input_t */
    bool    on;
} vm_n88_edge_t;

static double vm_n88_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* The app's buttons as the 3GS's pins: Home is the menu button, Power the
 * hold button, and the switch's "pressed" is silent. */
static bool vm_n88_input_for(VMButton button, n88_input_t *out) {
    switch (button) {
        case VMButtonHome:         *out = N88_INPUT_MENU;    return true;
        case VMButtonPower:        *out = N88_INPUT_HOLD;    return true;
        case VMButtonVolumeUp:     *out = N88_INPUT_VOLUP;   return true;
        case VMButtonVolumeDown:   *out = N88_INPUT_VOLDOWN; return true;
        case VMButtonRingerSilent: *out = N88_INPUT_SILENT;  return true;
        case VMButtonCount:        break;
    }
    return false;
}

@interface VMN88Engine ()
- (void)notePrepareDone:(uint64_t)done total:(uint64_t)total;
@end

static void vm_n88_prepare_progress(void *ctx, uint64_t done, uint64_t total) {
    [(__bridge VMN88Engine *)ctx notePrepareDone:done total:total];
}

@implementation VMN88Engine {
    pthread_mutex_t  _lock;
    n88_t           *_m;               /* owned by the thread while it runs */
    BOOL             _stop;            /* these two under _lock */
    BOOL             _paused;
    dispatch_semaphore_t _exited;
    BOOL             _running;
    NSMutableString *_pending;
    NSString        *_state;
    double           _rate;            /* guest instructions per host second */
    double           _guestSeconds;
    uint64_t         _retired;
    uint8_t         *_frame;           /* the published frame, under _lock */
    uint64_t         _frameSerial;     /* bumped per publication           */
    uint64_t         _frameTaken;      /* the serial last copied out       */

    /* What this start boots. Set before the thread starts, read-only after. */
    NSString        *_machineDirectory;
    BOOL             _ios3;
    NSData          *_kernel;
    NSData          *_tree;
    NSString        *_rootfsPath;      /* the imported, pristine one        */
    NSString        *_workPath;        /* this machine's own copy           */

    /* Under _lock. */
    double           _prepareFraction; /* < 0 when not preparing           */
    BOOL             _asleep;
    BOOL             _inputReady;      /* iPhone OS 3 booted                */
    vm_touch_queue_t _touch;
    uint64_t         _touchDelivered;
    vm_n88_edge_t    _edges[VM_N88_EDGE_CAP];
    unsigned         _edgeHead;
    unsigned         _edgeCount;
    BOOL             _buttons[VMButtonCount];

    /* The emulator thread's own. */
    double           _pressedAt[N88_INPUT_COUNT];
    BOOL             _frameBlack;
}

+ (BOOL)firmwarePresent {
    NSString *dir = [[VMSettings sharedSettings] iPhone3GSFirmwareDirectory];
    if (dir.length == 0) return NO;
    NSFileManager *fm = [NSFileManager defaultManager];
    return [fm fileExistsAtPath:[dir stringByAppendingPathComponent:@"kernel.macho"]] &&
           [fm fileExistsAtPath:[dir stringByAppendingPathComponent:@"devicetree.bin"]];
}

- (instancetype)initWithMachineDirectory:(NSString *)directory {
    self = [super init];
    if (!self) return nil;
    pthread_mutex_init(&_lock, NULL);
    _frame = calloc(1, kVMN88FrameBytes);
    if (!_frame) return nil;
    _pending = [NSMutableString string];
    _state = @"not started";
    _machineDirectory = [directory copy];
    _prepareFraction = -1.0;
    vm_touch_queue_reset(&_touch);
    return self;
}

- (instancetype)init {
    return [self initWithMachineDirectory:nil];
}

- (void)dealloc {
    [self stop];
    free(_frame);
    pthread_mutex_destroy(&_lock);
}

- (void)appendConsole:(NSString *)text {
    if (text.length == 0) return;
    pthread_mutex_lock(&_lock);
    [_pending appendString:text];
    if (_pending.length > kVMN88ConsoleLimit)
        [_pending deleteCharactersInRange:
            NSMakeRange(0, _pending.length - kVMN88ConsoleLimit)];
    pthread_mutex_unlock(&_lock);
}

- (void)setState:(NSString *)state {
    pthread_mutex_lock(&_lock);
    _state = state;
    pthread_mutex_unlock(&_lock);
}

- (void)notePrepareDone:(uint64_t)done total:(uint64_t)total {
    pthread_mutex_lock(&_lock);
    _prepareFraction = total ? (double)done / (double)total : 0.0;
    pthread_mutex_unlock(&_lock);
}

- (BOOL)startWithError:(NSString **)error {
    pthread_mutex_lock(&_lock);
    const BOOL busy = _running || _m;
    pthread_mutex_unlock(&_lock);
    if (busy) {
        if (error) *error = @"The machine is already running.";
        return NO;
    }

    NSString *dir = [[VMSettings sharedSettings] iPhone3GSFirmwareDirectory];
    NSData *kernel = dir.length
        ? [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"kernel.macho"]
                                 options:NSDataReadingMappedIfSafe
                                   error:NULL]
        : nil;
    NSData *tree = dir.length
        ? [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"devicetree.bin"]
                                 options:0
                                   error:NULL]
        : nil;
    if (!kernel.length || !tree.length) {
        if (error)
            *error = @"This machine needs iPhone 3GS firmware. Import an "
                      "iPhone2,1 IPSW -- iPhone OS 3.1.3 with its kernelcache, "
                      "device tree and root filesystem keys, or the iOS 6 "
                      "preview's -- in Settings > Import Firmware; its files go "
                      "to their own folder, firmware-iphone3gs.";
        return NO;
    }
    if (!n88_devicetree_is_3gs(tree.bytes, tree.length)) {
        if (error)
            *error = @"The device tree in the firmware-iphone3gs folder is "
                      "not an iPhone 3GS (N88AP) device tree.";
        return NO;
    }

    const BOOL ios3 = n88_ios3_identify(kernel.bytes, kernel.length);
    NSString *rootfs = nil, *work = nil;
    if (ios3) {
        if (!_machineDirectory.length) {
            if (error)
                *error = @"This machine has no folder of its own to keep its "
                          "root filesystem in. Open it from the machine list.";
            return NO;
        }
        NSFileManager *fm = [NSFileManager defaultManager];
        rootfs = [dir stringByAppendingPathComponent:@"rootfs.img"];
        work = [_machineDirectory stringByAppendingPathComponent:kVMN88WorkImage];
        if (![fm fileExistsAtPath:work] && ![fm fileExistsAtPath:rootfs]) {
            if (error)
                *error = @"iPhone OS 3.1.3 needs its root filesystem. Import the "
                          "iPhone2,1 3.1.3 IPSW again with the root filesystem "
                          "key (Settings > Import Firmware), so rootfs.img is in "
                          "the firmware-iphone3gs folder.";
            return NO;
        }
    }

    n88_t *m = calloc(1, sizeof *m);
    if (!m || !n88_init(m, true)) {
        free(m);
        if (error) *error = @"The machine's 256 MB of memory could not be allocated.";
        return NO;
    }

    if (ios3) {
        /* Booted on the thread: the first start makes the root filesystem
         * first, which takes a while. */
        [self appendConsole:
            @"[neon] iPhone 3GS (N88AP): iPhone OS 3.1.3 (7E18) on a Cortex-A8, "
             "cached interpreter, 256 MB DRAM\n"];
    } else {
        char detail[256] = {0};
        const n88_boot_t req = {
            .kernel = kernel.bytes, .kernel_size = kernel.length,
            .devicetree = tree.bytes, .devicetree_size = tree.length,
            .cmdline = kVMN88Cmdline,
        };
        const n88_status_t st = n88_boot(m, &req, detail, sizeof detail);
        if (st != N88_OK) {
            n88_free(m);
            free(m);
            if (error)
                *error = [NSString stringWithFormat:
                    @"The iPhone 3GS firmware could not be started: %s (%s).",
                    n88_strerror(st), detail];
            return NO;
        }
        [self appendConsole:[NSString stringWithFormat:
            @"[neon] iPhone 3GS (N88AP) preview: the iOS 6 kernel on a Cortex-A8, "
             "cached interpreter, 256 MB DRAM\n"
             "[neon] kernel entry 0x%08x, device tree 0x%08x (%u bytes), "
             "boot-args \"%s\"\n"
             "[neon] no storage or input yet: the kernel paints its log on the "
             "screen until it waits for its root device\n\n",
            m->entry_pa, m->devicetree_pa, m->devicetree_size, kVMN88Cmdline]];
    }

    NSThread *thread = [[NSThread alloc] initWithTarget:self
                                               selector:@selector(threadMain:)
                                                 object:nil];
    if (!thread) {
        n88_free(m);
        free(m);
        if (error) *error = @"The emulator thread could not be created.";
        return NO;
    }
    thread.name = @"NEON iPhone 3GS";
    thread.qualityOfService = NSQualityOfServiceUserInitiated;
    thread.stackSize = 512 * 1024;

    pthread_mutex_lock(&_lock);
    _ios3 = ios3;
    _kernel = kernel;
    _tree = tree;
    _rootfsPath = rootfs;
    _workPath = work;
    _stop = NO;
    _m = m;
    _running = YES;
    _exited = dispatch_semaphore_create(0);
    _state = ios3 ? @"starting" : @"running";
    _retired = 0;
    _rate = 0.0;
    _guestSeconds = 0.0;
    _frameSerial = _frameTaken = 0;
    _prepareFraction = -1.0;
    _asleep = NO;
    _inputReady = NO;
    vm_touch_queue_reset(&_touch);
    _touchDelivered = 0;
    _edgeHead = _edgeCount = 0;
    memset(_buttons, 0, sizeof _buttons);
    pthread_mutex_unlock(&_lock);
    for (unsigned i = 0; i < N88_INPUT_COUNT; i++) _pressedAt[i] = -1e9;
    _frameBlack = NO;
    [thread start];
    return YES;
}

/*
 * iPhone OS 3's boot, on the emulator thread (tools/n88_ios3.h): this
 * machine's root filesystem, made the first time; then the boot, the kernel's
 * memory-disk patch and the clock. Returns the open work image, which the
 * machine reads and writes until the thread ends, or NULL with `reason`.
 */
- (file_block_t *)bootIOS3_emulatorThread:(n88_t *)m reason:(NSString **)reason {
    char src[1024], dst[1024];
    if (![_rootfsPath getFileSystemRepresentation:src maxLength:sizeof src] ||
        ![_workPath getFileSystemRepresentation:dst maxLength:sizeof dst]) {
        *reason = @"this machine's root filesystem is somewhere this app cannot name";
        return NULL;
    }

    struct stat sb;
    if (stat(dst, &sb) != 0) {
        [self setState:@"preparing the root filesystem"];
        [self appendConsole:
            @"[neon] making this machine's root filesystem from the imported one "
             "(only the first time; a few hundred MB)\n"];
        rootfs_work_result_t rr;
        const rootfs_work_status_t rs = n88_ios3_make_work_image(
            src, dst, true, vm_n88_prepare_progress, (__bridge void *)self, &rr);
        pthread_mutex_lock(&_lock);
        _prepareFraction = -1.0;
        pthread_mutex_unlock(&_lock);
        if (rs != ROOTFS_WORK_OK) {
            *reason = [NSString stringWithFormat:
                @"the root filesystem could not be prepared: %s at %s (%s)",
                rootfs_work_status_name(rs), rootfs_work_stage_name(rr.stage), rr.detail];
            return NULL;
        }
        [self appendConsole:[NSString stringWithFormat:
            @"[neon] root filesystem ready: %llu bytes, mounted as /dev/md0\n",
            (unsigned long long)rr.final_size]];
        if (stat(dst, &sb) != 0) {
            *reason = @"the root filesystem was prepared but cannot be found";
            return NULL;
        }
    }

    pthread_mutex_lock(&_lock);
    const BOOL stop = _stop;
    pthread_mutex_unlock(&_lock);
    if (stop) {
        *reason = @"stopped before booting";
        return NULL;
    }

    [self setState:@"booting"];
    file_block_t *root = file_block_create();
    file_block_status_t fs = root ? file_block_open(root, dst, (uint64_t)sb.st_size)
                                  : FILE_BLOCK_STATUS_NO_MEMORY;
    if (fs != FILE_BLOCK_STATUS_OK) {
        *reason = [NSString stringWithFormat:
            @"this machine's root filesystem could not be opened: %s",
            file_block_strerror(fs)];
        if (root) file_block_destroy(&root);
        return NULL;
    }

    n88_boot_t req;
    n88_ios3_boot_request(&req, _kernel.bytes, _kernel.length, _tree.bytes, _tree.length,
                          file_block_get(root));
    char detail[256] = {0};
    const n88_status_t bs = n88_boot(m, &req, detail, sizeof detail);
    guest_patch_report_t pr;
    memset(&pr, 0, sizeof pr);
    const guest_patch_status_t ps =
        bs == N88_OK ? n88_ios3_patch(m, &pr) : GUEST_PATCH_STATUS_OK;
    if (bs != N88_OK || ps != GUEST_PATCH_STATUS_OK) {
        *reason = bs != N88_OK
            ? [NSString stringWithFormat:@"the firmware could not be started: %s (%s)",
                  n88_strerror(bs), detail]
            : [NSString stringWithFormat:@"the kernel could not be patched: %s",
                  guest_patch_status_string(ps)];
        file_block_close(root);
        file_block_destroy(&root);
        return NULL;
    }
    n88_set_rtc(m, (uint32_t)time(NULL));
    [self appendConsole:[NSString stringWithFormat:
        @"[neon] kernel entry 0x%08x, boot-args \"%s\", /dev/md0 %llu bytes\n\n",
        m->entry_pa, N88_ROOT_CMDLINE, (unsigned long long)sb.st_size]];
    pthread_mutex_lock(&_lock);
    _inputReady = YES;
    _state = @"running";
    pthread_mutex_unlock(&_lock);
    return root;
}

/*
 * Input, between slices, on the emulator thread: one touch report (the
 * controller holds one until the guest reads it), then every button edge
 * whose time has come, in order.
 */
- (void)deliverInput_emulatorThread:(n88_t *)m {
    s5l_mt_contact_t c;
    pthread_mutex_lock(&_lock);
    const BOOL haveTouch = vm_touch_queue_peek(&_touch, &c);
    pthread_mutex_unlock(&_lock);
    if (haveTouch) {
        if (s5l_mtz2_set_contacts(&m->touch, &c, 1u)) {
            pthread_mutex_lock(&_lock);
            vm_touch_queue_pop(&_touch);
            _touchDelivered++;
            pthread_mutex_unlock(&_lock);
        } else if (!s5l_mtz2_irq(&m->touch)) {
            /* Not backpressure (a report still unread) but a part that cannot
             * report at all: off with the display, or in reset. Waiting on
             * this report would hold every later one behind it. */
            pthread_mutex_lock(&_lock);
            vm_touch_queue_pop(&_touch);
            _touch.dropped++;
            pthread_mutex_unlock(&_lock);
        }
    }

    const double now = n88_guest_seconds(m);
    for (;;) {
        vm_n88_edge_t e = { 0u, false };
        pthread_mutex_lock(&_lock);
        const BOOL have = _edgeCount > 0;
        if (have) e = _edges[_edgeHead];
        pthread_mutex_unlock(&_lock);
        if (!have) break;
        if (!e.on && now < _pressedAt[e.input] + kVMN88MinPress) break;
        n88_set_input(m, (n88_input_t)e.input, e.on);
        if (e.on) _pressedAt[e.input] = now;
        pthread_mutex_lock(&_lock);
        _edgeHead = (_edgeHead + 1u) % VM_N88_EDGE_CAP;
        _edgeCount--;
        pthread_mutex_unlock(&_lock);
    }
}

- (NSString *)runMachine_emulatorThread:(n88_t *)m {
    const double start = vm_n88_now();
    const double guestStart = n88_guest_seconds(m);
    double pausedFor = 0.0, lastRateAt = start, lastFrameAt = 0.0;
    uint64_t retired = 0, retiredAtRate = 0;
    arm_status_t st = ARM_OK;
    NSString *finalState = @"stopped";
    char buf[4096];

    for (;;) {
        @autoreleasepool {
            pthread_mutex_lock(&_lock);
            const BOOL stop = _stop, paused = _paused;
            pthread_mutex_unlock(&_lock);
            if (stop) break;
            if (paused) {
                const double p0 = vm_n88_now();
                usleep(50000);
                pausedFor += vm_n88_now() - p0;
                continue;
            }
            if (_ios3) [self deliverInput_emulatorThread:m];
            const BOOL asleep = n88_asleep(m);
            retired += n88_run(m, asleep ? kVMN88SleepSlice : kVMN88Slice, &st);

            size_t n;
            while ((n = n88_console_take(m, buf, sizeof buf)) > 0) {
                NSString *text = [[NSString alloc] initWithBytes:buf
                                                          length:n
                                                        encoding:NSISOLatin1StringEncoding];
                [self appendConsole:text];
            }

            const double now = vm_n88_now();
            const double guest = n88_guest_seconds(m) - guestStart;
            const BOOL dark = n88_asleep(m);
            const uint8_t *fb = dark ? NULL : n88_framebuffer(m);
            pthread_mutex_lock(&_lock);
            if (dark && !_frameBlack) {
                /* The screen of a sleeping phone is off. */
                memset(_frame, 0, kVMN88FrameBytes);
                _frameSerial++;
                _frameBlack = YES;
            } else if (fb && now - lastFrameAt >= kVMN88FrameInterval) {
                /* The guest is not running while this copies. */
                memcpy(_frame, fb, kVMN88FrameBytes);
                _frameSerial++;
                _frameBlack = NO;
                lastFrameAt = now;
            }
            _asleep = dark;
            _retired = retired;
            _guestSeconds = guest;
            if (now - lastRateAt >= 1.0) {
                _rate = (double)(retired - retiredAtRate) / (now - lastRateAt);
                retiredAtRate = retired;
                lastRateAt = now;
            }
            pthread_mutex_unlock(&_lock);

            if (st != ARM_OK) {
                finalState = [NSString stringWithFormat:
                    @"stopped: the CPU refused an instruction at 0x%08x (status %d)",
                    m->cpu.r[15], (int)st];
                [self appendConsole:[NSString stringWithFormat:@"\n[neon] %@\n", finalState]];
                break;
            }

            /* Keep the guest clock from running ahead of the wall clock. */
            const double host = now - start - pausedFor;
            if (guest > host + 0.01)
                usleep((useconds_t)(fmin(guest - host, 0.05) * 1e6));
        }
    }
    return finalState;
}

- (void)threadMain:(id)unused {
    (void)unused;
    n88_t *m = _m;
    file_block_t *root = NULL;
    NSString *finalState = nil;

    if (_ios3) {
        NSString *why = nil;
        @autoreleasepool {
            root = [self bootIOS3_emulatorThread:m reason:&why];
        }
        if (!root) {
            finalState = [@"stopped: " stringByAppendingString:why ?: @"the machine did not start"];
            [self appendConsole:[NSString stringWithFormat:@"\n[neon] %@\n", finalState]];
        }
    }
    if (!finalState) finalState = [self runMachine_emulatorThread:m];

    pthread_mutex_lock(&_lock);
    _m = NULL;
    _inputReady = NO;
    pthread_mutex_unlock(&_lock);
    n88_free(m);
    free(m);
    if (root) {
        /* The guest's disk: everything it wrote reaches the file before the
         * machine is reported stopped. */
        file_block_close(root);
        file_block_destroy(&root);
    }
    pthread_mutex_lock(&_lock);
    _running = NO;
    _state = finalState;
    dispatch_semaphore_t exited = _exited;
    pthread_mutex_unlock(&_lock);
    dispatch_semaphore_signal(exited);
}

- (void)stop {
    pthread_mutex_lock(&_lock);
    _stop = YES;
    const BOOL running = _running;
    dispatch_semaphore_t exited = _exited;
    pthread_mutex_unlock(&_lock);
    if (running && exited)
        dispatch_semaphore_wait(exited, dispatch_time(DISPATCH_TIME_NOW,
                                                      (int64_t)(5 * NSEC_PER_SEC)));
}

- (void)setPaused:(BOOL)paused {
    pthread_mutex_lock(&_lock);
    _paused = paused;
    pthread_mutex_unlock(&_lock);
}

- (BOOL)isPaused {
    pthread_mutex_lock(&_lock);
    const BOOL paused = _paused;
    pthread_mutex_unlock(&_lock);
    return paused;
}

- (BOOL)isRunning {
    pthread_mutex_lock(&_lock);
    const BOOL running = _running;
    pthread_mutex_unlock(&_lock);
    return running;
}

- (NSString *)takePendingConsoleText {
    pthread_mutex_lock(&_lock);
    NSString *text = _pending.length ? [_pending copy] : nil;
    [_pending setString:@""];
    pthread_mutex_unlock(&_lock);
    return text;
}

- (BOOL)copyFrameInto:(void *)dst
             capacity:(size_t)capacity
                width:(uint32_t *)width
               height:(uint32_t *)height
               stride:(uint32_t *)stride {
    if (!dst || capacity < kVMN88FrameBytes) return NO;
    pthread_mutex_lock(&_lock);
    const BOOL fresh = _frameSerial != 0 && _frameSerial != _frameTaken;
    if (fresh) {
        memcpy(dst, _frame, kVMN88FrameBytes);
        _frameTaken = _frameSerial;
    }
    pthread_mutex_unlock(&_lock);
    if (!fresh) return NO;
    if (width) *width = N88_FB_WIDTH;
    if (height) *height = N88_FB_HEIGHT;
    if (stride) *stride = N88_FB_STRIDE;
    return YES;
}

- (BOOL)acceptsInput {
    pthread_mutex_lock(&_lock);
    const BOOL ready = _inputReady;
    pthread_mutex_unlock(&_lock);
    return ready;
}

- (BOOL)sendTouchAtGuestX:(int)x y:(int)y phase:(vm_touch_phase_t)phase {
    s5l_mt_contact_t c;
    if (!vm_touch_contact_from_ui(phase, x, y, &c)) return NO;
    pthread_mutex_lock(&_lock);
    const BOOL queued = _inputReady && vm_touch_queue_push(&_touch, &c);
    pthread_mutex_unlock(&_lock);
    return queued;
}

- (BOOL)setButton:(VMButton)button pressed:(BOOL)pressed {
    n88_input_t in;
    if (!vm_n88_input_for(button, &in)) return NO;
    BOOL queued = NO;
    pthread_mutex_lock(&_lock);
    if (_inputReady && _edgeCount < VM_N88_EDGE_CAP) {
        _edges[(_edgeHead + _edgeCount) % VM_N88_EDGE_CAP] =
            (vm_n88_edge_t){ (uint8_t)in, pressed ? true : false };
        _edgeCount++;
        _buttons[button] = pressed;
        queued = YES;
    }
    pthread_mutex_unlock(&_lock);
    return queued;
}

- (BOOL)isButtonPressed:(VMButton)button {
    if (button >= VMButtonCount) return NO;
    pthread_mutex_lock(&_lock);
    const BOOL held = _buttons[button];
    pthread_mutex_unlock(&_lock);
    return held;
}

- (NSString *)statusLine {
    pthread_mutex_lock(&_lock);
    NSString *state = (_running && _paused) ? @"paused"
                    : (_running && _asleep) ? @"asleep (press Home or Power)"
                    : _state;
    if (_running && _prepareFraction >= 0.0)
        state = [NSString stringWithFormat:@"%@ %.0f%%", _state, _prepareFraction * 100.0];
    NSString *line = [NSString stringWithFormat:
        @"iPhone 3GS%@  ·  %@  ·  %.1f M instr/s  ·  guest %.1f s  ·  %.2f G instructions",
        _ios3 ? @" · iPhone OS 3.1.3" : @" preview", state, _rate / 1e6, _guestSeconds,
        (double)_retired / 1e9];
    pthread_mutex_unlock(&_lock);
    return line;
}

@end
