//
//  S5LBox — host audio output engine implementation.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMAudioOutput.h"

#if defined(__APPLE__)
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include <math.h>
#include <stdatomic.h>

static const uint32_t kFramesPerBuffer = 1024u;
static const uint32_t kBufferCount = 4u;
static const Float64  kSampleRate = 44100.0;

@implementation VMAudioOutput {
    vm_audio_buffer_t   _ringBuffer;
    AudioQueueRef       _audioQueue;
    AudioQueueBufferRef _buffers[kBufferCount];
    BOOL                _running;
    BOOL                _paused;
    BOOL                _sessionConfigured;
    OSStatus            _lastError;
    _Atomic bool        _testToneRequested;
    uint32_t            _testToneFrame; // AudioQueue callback owns playback cursor.
    _Atomic uint64_t    _guestWords;
    _Atomic uint64_t    _guestNonzeroWords;
}

static void vm_audio_queue_output_callback(void *userData,
                                           AudioQueueRef inAQ,
                                           AudioQueueBufferRef inBuffer) {
    VMAudioOutput *self = (__bridge VMAudioOutput *)userData;
    [self fillAudioBuffer:inBuffer];
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
}

- (instancetype)init {
    self = [super init];
    if (self) {
        vm_audio_buffer_init(&_ringBuffer);
        _running = NO;
        _paused = NO;
        _sessionConfigured = NO;
        _lastError = noErr;
        atomic_init(&_testToneRequested, false);
        atomic_init(&_guestWords, 0);
        atomic_init(&_guestNonzeroWords, 0);
        _testToneFrame = (uint32_t)(0.25 * kSampleRate);
    }
    return self;
}

- (void)dealloc {
    [self stop];
    vm_audio_buffer_destroy(&_ringBuffer);
}

- (vm_audio_buffer_t *)ringBuffer {
    return &_ringBuffer;
}

- (void)configureAudioSession {
    if (_sessionConfigured) return;
    @try {
        AVAudioSession *session = [AVAudioSession sharedInstance];
        NSError *error = nil;
        [session setCategory:AVAudioSessionCategoryPlayback
                 withOptions:AVAudioSessionCategoryOptionMixWithOthers
                       error:&error];
        if (error) {
            NSLog(@"[VMAudioOutput] setCategory failed: %@", error);
        }
        [session setActive:YES error:&error];
        if (error) {
            NSLog(@"[VMAudioOutput] setActive failed: %@", error);
        } else {
            _sessionConfigured = YES;
        }
    } @catch (NSException *e) {
        NSLog(@"[VMAudioOutput] audio session exception: %@", e);
    }
}

- (BOOL)start {
    if (_running) return YES;

    [self configureAudioSession];
    vm_audio_buffer_reset(&_ringBuffer);
    _lastError = noErr;

    AudioStreamBasicDescription format;
    memset(&format, 0, sizeof format);
    format.mSampleRate       = kSampleRate;
    format.mFormatID         = kAudioFormatLinearPCM;
    format.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket   = 4;
    format.mFramesPerPacket  = 1;
    format.mBytesPerFrame    = 4;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel   = 16;

    OSStatus st = AudioQueueNewOutput(&format,
                                      vm_audio_queue_output_callback,
                                      (__bridge void *)self,
                                      NULL,
                                      NULL,
                                      0,
                                      &_audioQueue);
    if (st != noErr) {
        _lastError = st;
        NSLog(@"[VMAudioOutput] AudioQueueNewOutput failed: %d", (int)st);
        return NO;
    }

    uint32_t bufferByteSize = kFramesPerBuffer * format.mBytesPerFrame;
    for (uint32_t i = 0; i < kBufferCount; i++) {
        st = AudioQueueAllocateBuffer(_audioQueue, bufferByteSize, &_buffers[i]);
        if (st != noErr) {
            _lastError = st;
            NSLog(@"[VMAudioOutput] AudioQueueAllocateBuffer %u failed: %d", i, (int)st);
            [self stop];
            return NO;
        }
        [self fillAudioBuffer:_buffers[i]];
        AudioQueueEnqueueBuffer(_audioQueue, _buffers[i], 0, NULL);
    }

    st = AudioQueueStart(_audioQueue, NULL);
    if (st != noErr) {
        _lastError = st;
        NSLog(@"[VMAudioOutput] AudioQueueStart failed: %d", (int)st);
        [self stop];
        return NO;
    }

    _running = YES;
    _paused = NO;
    return YES;
}

- (void)stop {
    _running = NO;
    _paused = NO;

    if (_audioQueue) {
        AudioQueueStop(_audioQueue, true);
        AudioQueueDispose(_audioQueue, true);
        _audioQueue = NULL;
    }
}

- (void)pause {
    if (!_running || _paused) return;
    if (_audioQueue) {
        AudioQueuePause(_audioQueue);
    }
    _paused = YES;
}

- (void)resume {
    if (!_running || !_paused) return;
    if (_audioQueue) {
        AudioQueueStart(_audioQueue, NULL);
    }
    _paused = NO;
}

- (void)fillAudioBuffer:(AudioQueueBufferRef)buffer {
    uint32_t framesRead = vm_audio_buffer_read_frames(
        &_ringBuffer, (int16_t *)buffer->mAudioData, kFramesPerBuffer);
    (void)framesRead;
    // The chime is rendered by the consumer. It never writes into the guest
    // SPSC queue or changes guest sample counters.
    if (atomic_exchange_explicit(&_testToneRequested, false, memory_order_relaxed))
        _testToneFrame = 0;
    const uint32_t total = (uint32_t)(0.25 * kSampleRate);
    const uint32_t split = (uint32_t)(0.10 * kSampleRate);
    int16_t *samples = (int16_t *)buffer->mAudioData;
    for (uint32_t i = 0; i < kFramesPerBuffer && _testToneFrame < total;
         ++i, ++_testToneFrame) {
        uint32_t f = _testToneFrame;
        double frequency = f < split ? 587.33 : 880.0;
        double progress = f < split ? (double)f / split : (double)(f - split) / (total - split);
        int16_t tone = (int16_t)(sin(2.0 * M_PI * frequency * f / kSampleRate)
                                  * 14000.0 * exp(-3.5 * progress));
        samples[2 * i] = tone;
        samples[2 * i + 1] = tone;
    }
    buffer->mAudioDataByteSize = kFramesPerBuffer * sizeof(int16_t) * 2u;
}

- (void)pushSampleWord:(uint32_t)word {
    atomic_fetch_add_explicit(&_guestWords, 1, memory_order_relaxed);
    if (word != 0) atomic_fetch_add_explicit(&_guestNonzeroWords, 1, memory_order_relaxed);
    vm_audio_buffer_push_word(&_ringBuffer, word);
}

- (BOOL)isReadyForMore {
    return vm_audio_buffer_ready_for_more(&_ringBuffer);
}

- (void)playTestTone {
    atomic_store_explicit(&_testToneRequested, true, memory_order_relaxed);
}

- (NSString *)statusDescription {
    if (_lastError != noErr) {
        return [NSString stringWithFormat:@"Audio queue error %d", (int)_lastError];
    }
    if (!_running) {
        return @"Audio output stopped";
    }

    vm_audio_telemetry_t telem;
    vm_audio_buffer_telemetry(&_ringBuffer, &telem);

    uint64_t received = atomic_load_explicit(&_guestWords, memory_order_relaxed);
    uint64_t nonzero = atomic_load_explicit(&_guestNonzeroWords, memory_order_relaxed);
    return [NSString stringWithFormat:
        @"Guest I2S0: %llu words received, %llu nonzero; %llu frames consumed, %llu dropped, %llu underruns. Output assumes 44.1 kHz / 16-bit stereo; guest format unverified. Test Sound excluded.",
        (unsigned long long)received, (unsigned long long)nonzero,
        (unsigned long long)telem.consumed, (unsigned long long)telem.overflows,
        (unsigned long long)telem.underflows];
}

@end

#else

/* Non-Apple fallback for portable host tools */
@implementation VMAudioOutput {
    vm_audio_buffer_t _ringBuffer;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        vm_audio_buffer_init(&_ringBuffer);
    }
    return self;
}

- (void)dealloc {
    vm_audio_buffer_destroy(&_ringBuffer);
}

- (vm_audio_buffer_t *)ringBuffer {
    return &_ringBuffer;
}

- (BOOL)start { return YES; }
- (void)stop {}
- (void)pause {}
- (void)resume {}

- (void)pushSampleWord:(uint32_t)word {
    vm_audio_buffer_push_word(&_ringBuffer, word);
}

- (BOOL)isReadyForMore {
    return vm_audio_buffer_ready_for_more(&_ringBuffer);
}

- (void)playTestTone {}

- (NSString *)statusDescription {
    return @"Audio output stub (non-Apple platform)";
}

@end

#endif
