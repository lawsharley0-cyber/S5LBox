//
//  S5LBox — host audio output engine.
//
//  Streams 44.1 kHz 16-bit stereo PCM from the emulated I2S0 audio subsystem
//  to iOS speakers/headphones using AudioQueue and AVAudioSession.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <Foundation/Foundation.h>
#include "VMAudioBuffer.h"

NS_ASSUME_NONNULL_BEGIN

@interface VMAudioOutput : NSObject

/* Start host audio output. Configures AVAudioSession and primes the AudioQueue. */
- (BOOL)start;

/* Stop host audio output and release AudioQueue resources. */
- (void)stop;

/* Suspend/resume audio playback (e.g. when emulator pauses). */
- (void)pause;
- (void)resume;

/* Push a 32-bit PCM stereo sample word from emulator thread. */
- (void)pushSampleWord:(uint32_t)word;

/* Flow control query for PL080 DMA pacing. */
- (BOOL)isReadyForMore;

/* Status string with live telemetry for the UI and diagnostics. */
- (NSString *)statusDescription;

/* Pointer to the underlying ring buffer for direct C callback access. */
- (vm_audio_buffer_t *)ringBuffer;

@end

NS_ASSUME_NONNULL_END
