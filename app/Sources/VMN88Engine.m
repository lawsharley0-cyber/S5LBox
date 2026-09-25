//
//  NEON — VMN88Engine. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMN88Engine.h"

#import "VMSettings.h"

#include "n88.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* Instructions per slice: small enough that stop, pause and pacing are
 * prompt (a few milliseconds of host time), large enough that the cached
 * interpreter spends its time in guest code. */
static const unsigned kVMN88Slice = 200000u;
/* Console text held for the screen between drains. */
static const NSUInteger kVMN88ConsoleLimit = 64u * 1024u;

static double vm_n88_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
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
}

+ (BOOL)firmwarePresent {
    NSString *dir = [[VMSettings sharedSettings] iPhone3GSFirmwareDirectory];
    if (dir.length == 0) return NO;
    NSFileManager *fm = [NSFileManager defaultManager];
    return [fm fileExistsAtPath:[dir stringByAppendingPathComponent:@"kernel.macho"]] &&
           [fm fileExistsAtPath:[dir stringByAppendingPathComponent:@"devicetree.bin"]];
}

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    pthread_mutex_init(&_lock, NULL);
    _pending = [NSMutableString string];
    _state = @"not started";
    return self;
}

- (void)dealloc {
    [self stop];
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
                      "iPhone2,1 iOS 6 IPSW with its kernelcache and device "
                      "tree keys (Settings > Import Firmware); its files go "
                      "to their own folder, firmware-iphone3gs.";
        return NO;
    }
    if (!n88_devicetree_is_3gs(tree.bytes, tree.length)) {
        if (error)
            *error = @"The device tree in the firmware-iphone3gs folder is "
                      "not an iPhone 3GS (N88AP) device tree.";
        return NO;
    }

    n88_t *m = calloc(1, sizeof *m);
    if (!m || !n88_init(m, true)) {
        free(m);
        if (error) *error = @"The machine's 256 MB of memory could not be allocated.";
        return NO;
    }
    char detail[256] = {0};
    const n88_boot_t req = {
        .kernel = kernel.bytes, .kernel_size = kernel.length,
        .devicetree = tree.bytes, .devicetree_size = tree.length,
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
         "[neon] no storage, display or input yet: the kernel runs until it "
         "waits for its root device\n\n",
        m->entry_pa, m->devicetree_pa, m->devicetree_size, N88_DEFAULT_CMDLINE]];

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
    _stop = NO;
    _m = m;
    _running = YES;
    _exited = dispatch_semaphore_create(0);
    _state = @"running";
    _retired = 0;
    _rate = 0.0;
    _guestSeconds = 0.0;
    pthread_mutex_unlock(&_lock);
    [thread start];
    return YES;
}

- (void)threadMain:(id)unused {
    (void)unused;
    n88_t *m = _m;
    const double start = vm_n88_now();
    const double guestStart = n88_guest_seconds(m);
    double pausedFor = 0.0, lastRateAt = start;
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
            retired += n88_run(m, kVMN88Slice, &st);

            size_t n;
            while ((n = n88_console_take(m, buf, sizeof buf)) > 0) {
                NSString *text = [[NSString alloc] initWithBytes:buf
                                                          length:n
                                                        encoding:NSISOLatin1StringEncoding];
                [self appendConsole:text];
            }

            const double now = vm_n88_now();
            const double guest = n88_guest_seconds(m) - guestStart;
            pthread_mutex_lock(&_lock);
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

    n88_free(m);
    free(m);
    pthread_mutex_lock(&_lock);
    _m = NULL;
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

- (NSString *)statusLine {
    pthread_mutex_lock(&_lock);
    NSString *state = (_running && _paused) ? @"paused" : _state;
    NSString *line = [NSString stringWithFormat:
        @"iPhone 3GS preview  ·  %@  ·  %.1f M instr/s  ·  guest %.1f s  ·  %.2f G instructions",
        state, _rate / 1e6, _guestSeconds, (double)_retired / 1e9];
    pthread_mutex_unlock(&_lock);
    return line;
}

@end
