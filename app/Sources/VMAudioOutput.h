//
//  VMAudioOutput.h
//  S5LBox
//

#import <Foundation/Foundation.h>
#include <stdint.h>

@interface VMAudioOutput : NSObject

- (BOOL)start;
- (void)stop;
- (void)pushWord:(uint32_t)word;

@end
