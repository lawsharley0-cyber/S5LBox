//
//  NEON — the iPhone 3GS preview machine's engine.
//
//  A deliberately small sibling of VMEngine. VMEngine is the S5L8900 machine's
//  engine and everything in it -- the framebuffer, touch, buttons, audio,
//  checkpoints, the root-filesystem work image -- belongs to that machine.
//  The iPhone 3GS machine (core/include/n88.h) has none of those yet: it boots
//  the iOS 6 kernel as far as a machine with no storage, display or input
//  allows, and prints what the kernel prints. So this runs it on a thread of
//  its own and hands the console text to the screen, and nothing else.
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

/* One line: state, speed, guest time. */
- (NSString *)statusLine;

@end

NS_ASSUME_NONNULL_END
