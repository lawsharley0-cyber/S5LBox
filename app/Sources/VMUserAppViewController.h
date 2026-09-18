#import <UIKit/UIKit.h>

/* Present only from Machines, after any prior engine has closed its disk. */
@interface VMUserAppViewController : UIViewController
- (instancetype)initWithInstanceID:(NSString *)identifier machineName:(NSString *)name;
@end
