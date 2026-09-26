//
//  NEON — the iPhone 3GS preview machine's engine.
//
//  A deliberately small sibling of VMEngine. VMEngine is the S5L8900 machine's
//  engine and everything in it -- the framebuffer, touch, buttons, audio,
//  checkpoints, the root-filesystem work image -- belongs to that machine.
//  The iPhone 3GS machine (core/include/n88.h) has few of those yet: it boots
//  the iOS 6 kernel as far as a machine with no storage or input allows, on
//  the framebuffer iBoot would have set up, where the kernel paints its boot
//  log. So this runs it on a thread of its own and publishes that framebuffer
//  and any console text, and nothing else.
//
//  Time: the guest's clock is kept from running ahead of the wall clock.
//  An idle kernel skips straight to its next timer interrupt, which on a fast
//  host covers hours of guest time a minute; pacing it makes the kernel's
//  "once a minute" messages arrive once a minute. A busy guest is never
//  slowed -- it simply runs as fast as the host allows.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface VMN88Engine : NSObject

/* YES when the iPhone 3GS firmware folder holds both files the machine
 * needs (kernel.macho, devicetree.bin). Reads no contents. */
+ (BOOL)firmwarePresent;

/* Load the firmware, bring the machine up and start its thread. On NO,
 * `error` is one sentence for the user. */
- (BOOL)startWithError:(NSString * _Nullable * _Nullable)error;

/* Stop the thread and free the machine. Returns once both are done. */
- (void)stop;

- (void)setPaused:(BOOL)paused;
- (BOOL)isPaused;
- (BOOL)isRunning;

/* Console text produced since the last call, or nil. */
- (nullable NSString *)takePendingConsoleText;

/* The newest published frame, if there is one the caller has not taken:
 * copies it into `dst` (at least `capacity` bytes) and reports its geometry.
 * Pixels are 32 bits; present them in the B,G,R,X order the iPhone OS 3
 * machine's framebuffer uses (the kernel's console draws only greys, so the
 * order is not yet confirmed on this machine). Frames are copied out of guest
 * memory on the machine's own thread, so the caller never reads guest RAM. */
- (BOOL)copyFrameInto:(void *)dst
             capacity:(size_t)capacity
                width:(uint32_t *)width
               height:(uint32_t *)height
               stride:(uint32_t *)stride;

/* One line: state, speed, guest time. */
- (NSString *)statusLine;

@end

NS_ASSUME_NONNULL_END
