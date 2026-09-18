//
//  S5LBox — host audio output engine implementation.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMAudioOutput.h"

#if defined(__APPLE__)
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

static const uint32_t kFramesPerBuffer = 1024u;
static const uint32_t kBufferCount = 3u;
static const Float64  kSampleRate = 44100.0;

@implementation VMAudioOutput {
    vm_audio_buffer_t   _ringBuffer;
    AudioQueueRef       _audioQueue;
    AudioQueueBufferRef _buffers[kBufferCount];
    BOOL                _running;
    BOOL                _paused;
    BOOL                _sessionConfigured;
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
                                      kCFRunLoopCommonModes,
                                      0,
                                      &_audioQueue);
    if (st != noErr) {
        NSLog(@"[VMAudioOutput] AudioQueueNewOutput failed: %d", (int)st);
        return NO;
    }

    uint32_t bufferByteSize = kFramesPerBuffer * format.mBytesPerFrame;
    for (uint32_t i = 0; i < kBufferCount; i++) {
        st = AudioQueueAllocateBuffer(_audioQueue, bufferByteSize, &_buffers[i]);
        if (st != noErr) {
            NSLog(@"[VMAudioOutput] AudioQueueAllocateBuffer %u failed: %d", i, (int)st);
            [self stop];
            return NO;
        }
        [self fillAudioBuffer:_buffers[i]];
        AudioQueueEnqueueBuffer(_audioQueue, _buffers[i], 0, NULL);
    }

    st = AudioQueueStart(_audioQueue, NULL);
    if (st != noErr) {
        NSLog(@"[VMAudioOutput] AudioQueueStart failed: %d", (int)st);
        [self stop];
        return NO;
    }

    _running = YES;
    _paused = NO;
    return YES;
}

- (void)stop {
    if (!_running) return;
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
    buffer->mAudioDataByteSize = kFramesPerBuffer * sizeof(int16_t) * 2u;
}

- (void)pushSampleWord:(uint32_t)word {
    vm_audio_buffer_push_word(&_ringBuffer, word);
}

- (BOOL)isReadyForMore {
    return vm_audio_buffer_ready_for_more(&_ringBuffer);
}

- (NSString *)statusDescription {
    vm_audio_telemetry_t telem;
    vm_audio_buffer_telemetry(&_ringBuffer, &telem);

    if (!_running) {
        return @"Audio output stopped";
    }

    if (telem.produced == 0) {
        return @"Audio output active (44.1 kHz stereo) · idle (no guest sound playing)";
    }

    double bufferedMs = (double)telem.count * 1000.0 / kSampleRate;
    return [NSString stringWithFormat:
        @"Audio output active (44.1 kHz stereo) · %llu frames delivered, %.0f ms buffered (%llu underflow, %llu overflow)",
        (unsigned long long)telem.consumed,
        bufferedMs,
        (unsigned long long)telem.underflows,
        (unsigned long long)telem.overflows];
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

- (NSString *)statusDescription {
    return @"Audio output stub (non-Apple platform)";
}

@end

#endif
