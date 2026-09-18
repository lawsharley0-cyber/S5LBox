//
//  VMAudioOutput.m
//  S5LBox
//

#import "VMAudioOutput.h"
#import <AVFAudio/AVFAudio.h>
#include <stdatomic.h>
#include <string.h>

enum { kVMAudioRingFrames = 16384 };

@implementation VMAudioOutput {
    AVAudioEngine *_engine;
    AVAudioSourceNode *_source;
    _Atomic(uint32_t) _read;
    _Atomic(uint32_t) _write;
    uint32_t _ring[kVMAudioRingFrames];
    BOOL _interleaved;
}

- (BOOL)start {
    if (_engine) return YES;

    _engine = [[AVAudioEngine alloc] init];
    AVAudioSession *session = [AVAudioSession sharedInstance];
    NSError *error = nil;
    if (![session setCategory:AVAudioSessionCategoryPlayback error:&error] ||
        ![session setActive:YES error:&error]) {
        _engine = nil;
        return NO;
    }

    AVAudioFormat *format =
        [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0
                                                       channels:2];
    if (!format) {
        [session setActive:NO error:nil];
        _engine = nil;
        return NO;
    }
    _interleaved = format.isInterleaved;
    __weak VMAudioOutput *weakSelf = self;
    _source = [[AVAudioSourceNode alloc] initWithFormat:format
                                           renderBlock:^OSStatus(
                                               const AudioTimeStamp *timestamp,
                                               AVAudioFrameCount frameCount,
                                               AudioBufferList *outputData) {
        (void)timestamp;
        VMAudioOutput *self = weakSelf;
        if (!self) return noErr;
        if (!outputData || outputData->mNumberBuffers == 0u)
            return noErr;
        UInt32 required = self->_interleaved
            ? frameCount * 2u * (UInt32)sizeof(float)
            : frameCount * (UInt32)sizeof(float);
        AudioBuffer *first = &outputData->mBuffers[0];
        if (!first->mData || first->mDataByteSize < required)
            return noErr;
        float *left = (float *)first->mData;
        float *right = NULL;
        if (!self->_interleaved) {
            if (outputData->mNumberBuffers < 2u)
                return noErr;
            AudioBuffer *second = &outputData->mBuffers[1];
            if (!second->mData || second->mDataByteSize < required)
                return noErr;
            right = (float *)second->mData;
        }
        for (AVAudioFrameCount i = 0; i < frameCount; i++) {
            uint32_t read = atomic_load_explicit(&self->_read,
                                                 memory_order_relaxed);
            uint32_t write = atomic_load_explicit(&self->_write,
                                                  memory_order_acquire);
            if (read == write) {
                if (self->_interleaved) {
                    left[2u * i] = 0.0f;
                    left[2u * i + 1u] = 0.0f;
                } else {
                    left[i] = 0.0f;
                    right[i] = 0.0f;
                }
                continue;
            }
            uint32_t word = self->_ring[read % kVMAudioRingFrames];
            atomic_store_explicit(&self->_read, read + 1u,
                                  memory_order_release);
            int16_t l = (int16_t)(word & 0xffffu);
            int16_t r = (int16_t)(word >> 16);
            if (self->_interleaved) {
                left[2u * i] = (float)l / 32768.0f;
                left[2u * i + 1u] = (float)r / 32768.0f;
            } else {
                left[i] = (float)l / 32768.0f;
                right[i] = (float)r / 32768.0f;
            }
        }
        return noErr;
    }];
    [_engine attachNode:_source];
    [_engine connect:_source to:_engine.mainMixerNode format:format];
    [_engine prepare];
    if (![_engine startAndReturnError:&error]) {
        [_engine detachNode:_source];
        _source = nil;
        [session setActive:NO error:nil];
        return NO;
    }
    return YES;
}

- (void)stop {
    [_engine stop];
    if (_source) [_engine detachNode:_source];
    _source = nil;
    [_engine reset];
    _engine = nil;
    _interleaved = NO;
    [[AVAudioSession sharedInstance] setActive:NO error:nil];
    atomic_store_explicit(&_read, 0u, memory_order_relaxed);
    atomic_store_explicit(&_write, 0u, memory_order_relaxed);
}

- (void)pushWord:(uint32_t)word {
    uint32_t write = atomic_load_explicit(&_write, memory_order_relaxed);
    uint32_t read = atomic_load_explicit(&_read, memory_order_acquire);
    if (write - read >= kVMAudioRingFrames) {
        atomic_store_explicit(&_read, read + 1u, memory_order_release);
    }
    _ring[write % kVMAudioRingFrames] = word;
    atomic_store_explicit(&_write, write + 1u, memory_order_release);
}

@end
