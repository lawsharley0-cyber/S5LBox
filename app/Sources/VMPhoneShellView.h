// Original early-iPhone-style host chrome. The real guest owns every screen pixel.
#import <UIKit/UIKit.h>
#import "VMEngine.h"
@class VMPhoneShellView;
@protocol VMPhoneShellDelegate <NSObject>
- (void)phoneShell:(VMPhoneShellView *)shell button:(VMButton)button pressed:(BOOL)pressed;
- (void)phoneShellShowControls:(VMPhoneShellView *)shell;
@end
@interface VMPhoneShellView : UIView
@property (nonatomic, weak) id<VMPhoneShellDelegate> delegate;
@property (nonatomic, readonly) UIView *guestContainer;
@property (nonatomic, readonly) UIButton *menuButton;
@property (nonatomic) BOOL classicAppearance;
@property (nonatomic) BOOL controlsEnabled;
- (void)releaseButtons;
@end
