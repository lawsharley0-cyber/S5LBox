#import <UIKit/UIKit.h>

/* The guest's own crash reports (Apple's ReportCrash writes them into the
 * guest disk), read-only from the stopped machine's disk image. Present only
 * from Machines, after any prior engine has closed its disk. */
@interface VMGuestLogsViewController : UITableViewController
- (instancetype)initWithInstanceID:(NSString *)identifier machineName:(NSString *)name;
@end
