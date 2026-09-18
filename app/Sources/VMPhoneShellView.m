#import "VMPhoneShellView.h"
#import "VMPhoneLayout.h"
#import <QuartzCore/QuartzCore.h>

static CGRect VMPhoneCGRect(vm_phone_rect_t r) {
    return CGRectMake(r.x, r.y, r.width, r.height);
}

@implementation VMPhoneShellView {
    UIView *_body;
    UIView *_receiver;
    UIButton *_homeButton;
    UIButton *_powerButton;
    UIView *_homeMark;
    CAGradientLayer *_glass;
    BOOL _homePressed, _powerPressed;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _classicAppearance = YES;
    _controlsEnabled = YES;
    self.backgroundColor = UIColor.blackColor;
    _body = [[UIView alloc] init];
    _body.userInteractionEnabled = NO;
    _body.layer.cornerRadius = 38;
    _body.layer.borderWidth = 1.5;
    _body.layer.borderColor = [UIColor colorWithWhite:0.32 alpha:1].CGColor;
    _body.clipsToBounds = YES;
    _glass = [CAGradientLayer layer];
    _glass.colors = @[(id)[UIColor colorWithWhite:0.12 alpha:1].CGColor,
                     (id)[UIColor colorWithWhite:0.025 alpha:1].CGColor,
                     (id)[UIColor colorWithWhite:0.055 alpha:1].CGColor];
    [_body.layer insertSublayer:_glass atIndex:0];
    [self addSubview:_body];
    _guestContainer = [[UIView alloc] init];
    _guestContainer.backgroundColor = UIColor.blackColor;
    _guestContainer.clipsToBounds = YES;
    [self addSubview:_guestContainer];
    _receiver = [[UIView alloc] init];
    _receiver.backgroundColor = [UIColor colorWithWhite:0.02 alpha:1];
    _receiver.layer.borderWidth = 1;
    _receiver.layer.borderColor = [UIColor colorWithWhite:0.23 alpha:1].CGColor;
    _receiver.layer.cornerRadius = 3;
    _receiver.isAccessibilityElement = NO;
    [self addSubview:_receiver];

    _homeButton = [UIButton buttonWithType:UIButtonTypeCustom];
    _homeButton.backgroundColor = [UIColor colorWithWhite:0.045 alpha:1];
    _homeButton.layer.borderWidth = 1;
    _homeButton.layer.borderColor = [UIColor colorWithWhite:0.22 alpha:1].CGColor;
    _homeButton.accessibilityLabel = @"Home";
    _homeButton.accessibilityHint = @"Press or hold the virtual iPhone Home button.";
    _homeButton.accessibilityIdentifier = @"s5lbox.phone.home";
    [self addSubview:_homeButton];
    _homeMark = [[UIView alloc] init];
    _homeMark.userInteractionEnabled = NO;
    _homeMark.layer.cornerRadius = 4;
    _homeMark.layer.borderWidth = 1.6;
    _homeMark.layer.borderColor = [UIColor colorWithWhite:0.7 alpha:1].CGColor;
    [_homeButton addSubview:_homeMark];
    _powerButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [_powerButton setImage:[UIImage systemImageNamed:@"power"] forState:UIControlStateNormal];
    _powerButton.tintColor = [UIColor colorWithWhite:0.65 alpha:1];
    _powerButton.accessibilityLabel = @"Sleep or wake";
    _powerButton.accessibilityHint = @"Hold to show the guest's power-off slider.";
    _powerButton.accessibilityIdentifier = @"s5lbox.phone.power";
    [self addSubview:_powerButton];
    for (UIButton *button in @[_homeButton, _powerButton]) {
        [button addTarget:self action:@selector(keyDown:) forControlEvents:UIControlEventTouchDown];
        [button addTarget:self action:@selector(keyUp:)
          forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
        /* VoiceOver activation has no touch-down; handle that path separately. */
        [button addTarget:self action:@selector(primaryAction:)
          forControlEvents:UIControlEventPrimaryActionTriggered];
    }
    _menuButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [_menuButton setImage:[UIImage systemImageNamed:@"ellipsis.circle"] forState:UIControlStateNormal];
    _menuButton.tintColor = [UIColor colorWithWhite:0.65 alpha:1];
    _menuButton.accessibilityLabel = @"iPhone controls";
    _menuButton.accessibilityHint = @"Pause, add apps, display options, and save and exit.";
    _menuButton.accessibilityIdentifier = @"s5lbox.phone.controls";
    [_menuButton addTarget:self action:@selector(showControls:) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:_menuButton];
    return self;
}

- (void)setClassicAppearance:(BOOL)classicAppearance {
    _classicAppearance = classicAppearance;
    [self setNeedsLayout];
}
- (void)setControlsEnabled:(BOOL)controlsEnabled {
    if (!controlsEnabled) [self releaseButtons];
    _controlsEnabled = controlsEnabled;
    _homeButton.enabled = _powerButton.enabled = _menuButton.enabled = controlsEnabled;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    vm_phone_layout_t layout;
    if (!vm_phone_layout(self.bounds.size.width, self.bounds.size.height,
                         self.classicAppearance, &layout)) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _body.frame = VMPhoneCGRect(layout.body);
    _glass.frame = _body.bounds;
    _body.hidden = !self.classicAppearance;
    _guestContainer.frame = VMPhoneCGRect(layout.screen);
    _receiver.frame = VMPhoneCGRect(layout.receiver);
    _receiver.hidden = !layout.receiver_visible || !self.classicAppearance;
    _homeButton.frame = VMPhoneCGRect(layout.home);
    _homeButton.layer.cornerRadius = layout.home.width / 2;
    _homeMark.frame = CGRectMake((layout.home.width - 17) / 2,
                                 (layout.home.height - 17) / 2, 17, 17);
    _powerButton.frame = VMPhoneCGRect(layout.power);
    _menuButton.frame = VMPhoneCGRect(layout.menu);
    [CATransaction commit];
}
- (void)keyDown:(UIButton *)button {
    if (!self.controlsEnabled) return;
    BOOL *pressed = button == _homeButton ? &_homePressed : &_powerPressed;
    if (*pressed) return;
    *pressed = YES;
    [self.delegate phoneShell:self button:button == _homeButton ? VMButtonHome : VMButtonPower pressed:YES];
}
- (void)keyUp:(UIButton *)button {
    BOOL *pressed = button == _homeButton ? &_homePressed : &_powerPressed;
    if (!*pressed) return;
    *pressed = NO;
    [self.delegate phoneShell:self button:button == _homeButton ? VMButtonHome : VMButtonPower pressed:NO];
}
- (void)primaryAction:(UIButton *)button {
    if (!UIAccessibilityIsVoiceOverRunning() || !self.controlsEnabled) return;
    [self keyDown:button];
    __weak VMPhoneShellView *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{ [weakSelf keyUp:button]; });
}
- (void)releaseButtons {
    [self keyUp:_homeButton];
    [self keyUp:_powerButton];
}
- (void)showControls:(id)sender {
    (void)sender;
    [self releaseButtons];
    [self.delegate phoneShellShowControls:self];
}
@end
