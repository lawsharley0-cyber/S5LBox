//
//  NEON — the iPhone 3GS machine's engine.
//
//  A deliberately small sibling of VMEngine. VMEngine is the S5L8900 machine's
//  engine and everything in it -- audio, checkpoints, the graphics choice --
//  belongs to that machine. This one runs the iPhone 3GS machine
//  (core/include/n88.h) on a thread of its own and publishes its screen and
//  console text. What it boots depends on the kernel in the iPhone 3GS
//  firmware folder:
//
//  - iPhone OS 3.1.3 (the 7E18 kernelcache, imported with its root
//    filesystem): the whole system, booted the way tools/n88_ios3.h describes,
//    which is the boot the desktop harness measured to the home screen. Each
//    machine has its own working copy of the root filesystem, made from the
//    imported one the first time it starts (a few hundred MB copied on this
//    engine's thread, with the progress in the status line); the guest writes
//    to that copy. Touch goes to the touch controller, and Home, Power, the
//    volume buttons and the ring/silent switch to the board's pins, so a
//    phone that has gone to sleep wakes with Home or Power. The clock starts
//    at the host's time. Its sound (i2s0, n88.h "Sound") plays through the
//    device's speaker; the guest's own volume buttons set how loud.
//  - iOS 6 (10B500): the preview, as before -- the kernel paints its verbose
//    boot log on the framebuffer iBoot would have set up, as far as a machine
//    with no storage or input allows. No input.
//
//  Time: the guest's clock is kept from running ahead of the wall clock.
//  An idle kernel skips straight to its next timer interrupt, which on a fast
//  host covers hours of guest time a minute; pacing it makes the kernel's
//  "once a minute" messages arrive once a minute, and keeps auto-lock at a
//  minute. A busy guest is never slowed -- it simply runs as fast as the host
//  allows.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <Foundation/Foundation.h>
#import "VMEngine.h"
#import "VMTouchMap.h"

NS_ASSUME_NONNULL_BEGIN

@interface VMN88Engine : NSObject

/* YES when the iPhone 3GS firmware folder holds both files the machine
 * needs (kernel.macho, devicetree.bin). Reads no contents. */
+ (BOOL)firmwarePresent;

/* `directory` is this machine's own folder (VMInstanceStore), where its
 * working root filesystem lives. Without one, iPhone OS 3 cannot start (the
 * iOS 6 preview needs none). */
- (instancetype)initWithMachineDirectory:(nullable NSString *)directory
    NS_DESIGNATED_INITIALIZER;
- (instancetype)init;

/* Load the firmware, bring the machine up and start its thread. On NO,
 * `error` is one sentence for the user. */
- (BOOL)startWithError:(NSString * _Nullable * _Nullable)error;

/* Stop the thread and free the machine. Returns once both are done, the
 * working root filesystem flushed and closed. */
- (void)stop;

- (void)setPaused:(BOOL)paused;
- (BOOL)isPaused;
- (BOOL)isRunning;

/* Console text produced since the last call, or nil. */
- (nullable NSString *)takePendingConsoleText;

/* The newest published frame, if there is one the caller has not taken:
 * copies it into `dst` (at least `capacity` bytes) and reports its geometry.
 * Pixels are 32 bits in B,G,R,X order. Frames are copied out of guest memory
 * on the machine's own thread, so the caller never reads guest RAM. While the
 * phone sleeps the frame is black, as its screen is. */
- (BOOL)copyFrameInto:(void *)dst
             capacity:(size_t)capacity
                width:(uint32_t *)width
               height:(uint32_t *)height
               stride:(uint32_t *)stride;

/* YES once iPhone OS 3 is running: touch and the buttons reach the guest. */
- (BOOL)acceptsInput;

/* A finger on the panel, in panel pixels (320x480). Queued for the touch
 * controller; NO if it was not (not running, off the panel, queue full). */
- (BOOL)sendTouchAtGuestX:(int)x y:(int)y phase:(vm_touch_phase_t)phase;

/* Press or release a button, or move the ring/silent switch (pressed =
 * silent). Queued; a release follows its press by at least a tenth of a
 * second of guest time, so a tap is never a pulse the guest cannot see. */
- (BOOL)setButton:(VMButton)button pressed:(BOOL)pressed;
- (BOOL)isButtonPressed:(VMButton)button;

/* One line: state, speed, guest time. */
- (NSString *)statusLine;

@end

NS_ASSUME_NONNULL_END
