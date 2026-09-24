#import "VMUserAppViewController.h"
#import "VMUserAppInstall.h"
#import "VMInstanceStore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BOOL CopyString(id value, char *destination, size_t capacity) {
    if (![value isKindOfClass:NSString.class]) return NO;
    return [(NSString *)value getCString:destination maxLength:capacity encoding:NSUTF8StringEncoding];
}

static bool ParseAppPlist(void *context, const uint8_t *bytes, size_t size,
                          vm_user_app_metadata_t *out, char *detail, size_t capacity) {
    (void)context;
    NSData *data = [NSData dataWithBytes:bytes length:size];
    NSError *error = nil;
    id decoded = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:&error];
    if (![decoded isKindOfClass:NSDictionary.class]) {
        snprintf(detail, capacity, "Info.plist is not a valid XML or binary property-list dictionary.");
        return false;
    }
    NSDictionary *plist = decoded;
    if (!CopyString(plist[@"CFBundleIdentifier"], out->identifier, sizeof out->identifier) ||
        !CopyString(plist[@"CFBundleExecutable"], out->executable, sizeof out->executable) ||
        !CopyString(plist[@"MinimumOSVersion"], out->minimum_os, sizeof out->minimum_os)) {
        snprintf(detail, capacity, "Info.plist must declare a short bundle identifier, executable, and MinimumOSVersion.");
        return false;
    }
    NSString *name = plist[@"CFBundleDisplayName"] ?: plist[@"CFBundleName"] ?: plist[@"CFBundleIdentifier"];
    if (!CopyString(name, out->display_name, sizeof out->display_name)) {
        snprintf(detail, capacity, "The app name is invalid or too long."); return false;
    }
    out->iphone_application = [plist[@"CFBundlePackageType"] isEqual:@"APPL"];
    id platforms = plist[@"CFBundleSupportedPlatforms"];
    if (platforms && (![platforms isKindOfClass:NSArray.class] || ![platforms containsObject:@"iPhoneOS"]))
        out->iphone_application = false;
    id families = plist[@"UIDeviceFamily"];
    if (families && (![families isKindOfClass:NSArray.class] || ![families containsObject:@1]))
        out->iphone_application = false;
    id required = plist[@"UIRequiredDeviceCapabilities"];
    for (NSString *capability in @[@"armv7", @"arm64", @"metal", @"opengles-2", @"opengles-3", @"gyroscope"]) {
        BOOL needs = [required isKindOfClass:NSArray.class] && [required containsObject:capability];
        if ([required isKindOfClass:NSDictionary.class]) {
            id value = required[capability];
            needs = [value isKindOfClass:NSNumber.class] && [value boolValue];
        }
        if (needs) {
            snprintf(detail, capacity, "This app requires %s, which this early iPhone does not support.", capability.UTF8String);
            return false;
        }
    }
    if (required && ![required isKindOfClass:NSArray.class] && ![required isKindOfClass:NSDictionary.class]) {
        snprintf(detail, capacity, "UIRequiredDeviceCapabilities has an invalid type."); return false;
    }
    return true;
}

/*
 * SQUARE ICONS. iPhone OS 3's SpringBoard rounds the corners and adds the
 * gloss for App Store apps it installs itself, but shows an app in
 * /Applications -- where this importer puts one -- exactly as its icon file
 * is: square. So the importer does what SpringBoard would have done: round
 * the corners, and add the gloss unless the app says its icon is already
 * rendered (UIPrerenderedIcon). The result is an ordinary 8-bit sRGB PNG,
 * which the guest's UIKit reads like any other. Anything unexpected leaves
 * the app's own icon untouched; this never blocks an install.
 */
static NSString *RoundAppIcon(vm_user_app_plan_t *plan) {
    const uint8_t *bytes = NULL;
    size_t size = 0u;
    if (!vm_user_app_plan_file(plan, "Info.plist", &bytes, &size)) return @"no Info.plist";
    id decoded = [NSPropertyListSerialization
        propertyListWithData:[NSData dataWithBytes:bytes length:size]
                     options:NSPropertyListImmutable format:NULL error:NULL];
    if (![decoded isKindOfClass:NSDictionary.class]) return @"unreadable Info.plist";
    NSDictionary *info = decoded;
    /* iPhone OS 3.1 reads CFBundleIconFile, then Icon.png. */
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    id iconFile = info[@"CFBundleIconFile"];
    if ([iconFile isKindOfClass:NSString.class] && [(NSString *)iconFile length]) {
        [names addObject:iconFile];
        if (![(NSString *)iconFile pathExtension].length)
            [names addObject:[(NSString *)iconFile stringByAppendingPathExtension:@"png"]];
    }
    [names addObject:@"Icon.png"];
    id flag = info[@"UIPrerenderedIcon"];
    const BOOL prerendered = [flag isKindOfClass:NSNumber.class] && [(NSNumber *)flag boolValue];

    for (NSString *name in names) {
        const char *relative = name.UTF8String;
        if (!relative || !vm_user_app_plan_file(plan, relative, &bytes, &size)) continue;
        /* Apple-crushed (CgBI) PNGs decode here too; ImageIO knows them. */
        UIImage *icon = [UIImage imageWithData:[NSData dataWithBytes:bytes length:size]];
        const CGSize px = icon ? CGSizeMake(round(icon.size.width * icon.scale),
                                            round(icon.size.height * icon.scale)) : CGSizeZero;
        if (px.width < 16.0 || px.height < 16.0 || px.width > 1024.0 || px.height > 1024.0)
            return [NSString stringWithFormat:@"%@ could not be read as an image", name];
        UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
        format.scale = 1.0;
        format.opaque = NO;
        format.preferredRange = UIGraphicsImageRendererFormatRangeStandard;   /* 8-bit sRGB */
        UIGraphicsImageRenderer *renderer =
            [[UIGraphicsImageRenderer alloc] initWithSize:px format:format];
        UIImage *rounded = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
            const CGRect r = { CGPointZero, px };
            /* A 57-pixel iPhone OS 3 icon has corners of about 10 pixels. */
            [[UIBezierPath bezierPathWithRoundedRect:r
                                        cornerRadius:MIN(px.width, px.height) * (10.0 / 57.0)] addClip];
            [icon drawInRect:r];
            if (prerendered) return;
            /* The gloss: a white wash over the top half, its lower edge an
             * arc that dips towards the middle. */
            CGContextRef cg = context.CGContext;
            CGContextSaveGState(cg);
            [[UIBezierPath bezierPathWithOvalInRect:CGRectMake(-0.5 * px.width, -0.62 * px.height,
                                                               2.0 * px.width, 1.14 * px.height)] addClip];
            CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
            const CGFloat components[8] = { 1, 1, 1, 0.50, 1, 1, 1, 0.12 };
            const CGFloat locations[2] = { 0, 1 };
            CGGradientRef gradient = space
                ? CGGradientCreateWithColorComponents(space, components, locations, 2) : NULL;
            if (gradient)
                CGContextDrawLinearGradient(cg, gradient, CGPointZero,
                                            CGPointMake(0, 0.52 * px.height), 0);
            CGGradientRelease(gradient);
            CGColorSpaceRelease(space);
            CGContextRestoreGState(cg);
        }];
        NSData *png = UIImagePNGRepresentation(rounded);
        uint8_t *copy = png.length ? malloc(png.length) : NULL;
        if (!copy) return @"out of memory for the icon";
        memcpy(copy, png.bytes, png.length);
        if (!vm_user_app_plan_replace_file(plan, relative, copy, png.length)) {
            free(copy);
            return [NSString stringWithFormat:@"%@ could not be replaced", name];
        }
        return nil;
    }
    return @"no icon file";
}

static size_t ReadIPA(void *context, uint64_t offset, uint8_t *bytes, size_t size) {
    NSData *data = (__bridge NSData *)context;
    if (offset > data.length || size > data.length - offset) return 0u;
    memcpy(bytes, (const uint8_t *)data.bytes + (size_t)offset, size);
    return size;
}

@interface VMUserAppViewController () <UIDocumentPickerDelegate>
- (void)receivedProgress:(uint64_t)done total:(uint64_t)total;
@end

static void AppInstallProgress(void *context, uint64_t done, uint64_t total) {
    VMUserAppViewController *controller = (__bridge VMUserAppViewController *)context;
    [controller receivedProgress:done total:total];
}

@implementation VMUserAppViewController {
    NSString *_identifier;
    NSString *_machineName;
    UILabel *_description;
    UILabel *_status;
    UIButton *_choose;
    UIButton *_install;
    UIProgressView *_progress;
    vm_user_app_plan_t *_plan;
    dispatch_queue_t _queue;
    BOOL _busy;
    BOOL _priorPopEnabled;
    __weak UIGestureRecognizer *_contentPop;
    BOOL _priorContentPopEnabled;
    UIBackgroundTaskIdentifier _backgroundTask;
}

- (instancetype)initWithInstanceID:(NSString *)identifier machineName:(NSString *)name {
    self = [super initWithNibName:nil bundle:nil];
    if (!self) return nil;
    _identifier = [identifier copy]; _machineName = [name copy];
    _backgroundTask = UIBackgroundTaskInvalid;
    _queue = dispatch_queue_create("com.j0shua.S5LBox.UserAppImport", DISPATCH_QUEUE_SERIAL);
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Add Your App";
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    _description = [[UILabel alloc] init];
    _description.text = @"Experimental IPA import for iPhone OS 3.1.3. Choose an unencrypted ARMv6 app that you own. Shut down iPhone OS with its Power slider first.\n\nThe app is added to the guest's Applications folder and guest code signing is relaxed on its next boot. The modern iPhone's signing and security settings are unchanged. App Store downloads, DRM removal, updates, and uninstall are not supported. Guest app launch still needs testing.";
    _description.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _description.adjustsFontForContentSizeCategory = YES;
    _description.numberOfLines = 0;
    _status = [[UILabel alloc] init];
    _status.text = [NSString stringWithFormat:@"Machine: %@", _machineName];
    _status.numberOfLines = 0;
    _status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _status.adjustsFontForContentSizeCategory = YES;
    _status.accessibilityIdentifier = @"s5lbox.user-app.status";
    _choose = [UIButton buttonWithType:UIButtonTypeSystem];
    [_choose setTitle:@"Choose IPA from Files" forState:UIControlStateNormal];
    [_choose addTarget:self action:@selector(chooseIPA) forControlEvents:UIControlEventTouchUpInside];
    _choose.accessibilityIdentifier = @"s5lbox.user-app.choose";
    _install = [UIButton buttonWithType:UIButtonTypeSystem];
    [_install setTitle:@"Install into This Guest" forState:UIControlStateNormal];
    [_install addTarget:self action:@selector(installApp) forControlEvents:UIControlEventTouchUpInside];
    _install.enabled = NO;
    _install.accessibilityIdentifier = @"s5lbox.user-app.install";
    _progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    UIScrollView *scroll = [[UIScrollView alloc] init];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scroll];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[_description, _status, _choose, _install, _progress]];
    stack.axis = UILayoutConstraintAxisVertical; stack.spacing = 20.0;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:24],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-24],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-24],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-48],
        [_choose.heightAnchor constraintGreaterThanOrEqualToConstant:44],
        [_install.heightAnchor constraintGreaterThanOrEqualToConstant:44]
    ]];
}

- (void)setBusy:(BOOL)busy {
    _busy = busy; _choose.enabled = !busy; _install.enabled = !busy && _plan != NULL;
    self.navigationItem.hidesBackButton = busy;
    if (busy) {
        _priorPopEnabled = self.navigationController.interactivePopGestureRecognizer.enabled;
        self.navigationController.interactivePopGestureRecognizer.enabled = NO;
        SEL selector = NSSelectorFromString(@"interactiveContentPopGestureRecognizer");
        if ([self.navigationController respondsToSelector:selector]) {
            _contentPop = [self.navigationController valueForKey:NSStringFromSelector(selector)];
            _priorContentPopEnabled = _contentPop.enabled;
            _contentPop.enabled = NO;
        }
    } else {
        self.navigationController.interactivePopGestureRecognizer.enabled = _priorPopEnabled;
        _contentPop.enabled = _priorContentPopEnabled;
        _contentPop = nil;
    }
}

- (void)chooseIPA {
    if (_busy) return;
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
        initWithDocumentTypes:@[@"public.data", @"public.zip-archive"] inMode:UIDocumentPickerModeImport];
    picker.delegate = self; picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    NSURL *url = urls.firstObject;
    if (_busy || !url) return;
    if (![url.pathExtension.lowercaseString isEqualToString:@"ipa"]) {
        _status.text = @"Choose a compatible .ipa file."; return;
    }
    vm_user_app_plan_close(&_plan);
    [self setBusy:YES];
    _progress.progress = 0;
    _status.text = @"Checking the archive, app metadata, and ARMv6 executable…";
    dispatch_async(_queue, ^{
        BOOL scoped = [url startAccessingSecurityScopedResource];
        NSNumber *size = nil;
        [url getResourceValue:&size forKey:NSURLFileSizeKey error:NULL];
        NSError *error = nil;
        NSData *data = size && size.unsignedLongLongValue <= VM_USER_APP_MAX_ARCHIVE
            ? [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:&error] : nil;
        char detail[VM_USER_APP_DETAIL_CAPACITY] = {0};
        vm_user_app_plan_t *plan = data ? vm_user_app_plan_open(ReadIPA, (__bridge void *)data,
            data.length, ParseAppPlist, NULL, detail, sizeof detail) : NULL;
        if (scoped) [url stopAccessingSecurityScopedResource];
        NSString *iconNote = plan ? RoundAppIcon(plan) : nil;
        if (iconNote) NSLog(@"[apps] icon left as shipped: %@", iconNote);
        NSString *failure = detail[0] ? [NSString stringWithUTF8String:detail]
            : (error.localizedDescription ?: @"The IPA could not be read or is larger than 128 MiB.");
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_plan = plan;
            [self setBusy:NO];
            if (!plan) { self->_status.text = failure; return; }
            const vm_user_app_metadata_t *metadata = vm_user_app_plan_metadata(plan);
            self->_status.text = [NSString stringWithFormat:@"%@\n%s\n%zu files and folders · %.1f MiB\nCompatibility checks passed. Tap Install to prepare a new guest disk. Actual app launch is not yet verified.",
                [NSString stringWithUTF8String:metadata->display_name], metadata->identifier,
                vm_user_app_plan_entry_count(plan), (double)vm_user_app_plan_content_bytes(plan) / (1024.0 * 1024.0)];
        });
    });
}

- (void)installApp {
    if (_busy || !_plan) return;
    NSString *work = [[VMInstanceStore sharedStore] directoryForInstanceWithID:_identifier];
    if (!work.length) { _status.text = @"This machine no longer exists."; return; }
    [self setBusy:YES];
    _status.text = @"Checking the stopped guest disk and preparing a separate copy…";
    _backgroundTask = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"Importing a guest app" expirationHandler:^{
        if (self->_backgroundTask != UIBackgroundTaskInvalid) {
            [UIApplication.sharedApplication endBackgroundTask:self->_backgroundTask];
            self->_backgroundTask = UIBackgroundTaskInvalid;
        }
    }];
    dispatch_async(_queue, ^{
        char detail[VM_USER_APP_DETAIL_CAPACITY] = {0};
        vm_guest_install_result_t result;
        BOOL installed = vm_user_app_install(work.fileSystemRepresentation, self->_plan,
            AppInstallProgress, (__bridge void *)self, &result, detail, sizeof detail);
        NSString *message = installed
            ? @"The app files are installed. Open this machine for a fresh boot, then look for the app in SpringBoard. Launch and hardware compatibility are experimental; importing files does not prove the app runs."
            : (detail[0] ? [NSString stringWithUTF8String:detail] : @"Installation could not finish. Reopen this screen to recover any interrupted transaction.");
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self->_backgroundTask != UIBackgroundTaskInvalid) {
                [UIApplication.sharedApplication endBackgroundTask:self->_backgroundTask];
                self->_backgroundTask = UIBackgroundTaskInvalid;
            }
            [self setBusy:NO];
            self->_status.text = message;
            if (installed) { self->_progress.progress = 1; self->_install.enabled = NO; }
        });
    });
}

- (void)receivedProgress:(uint64_t)done total:(uint64_t)total {
    float fraction = total ? (float)((double)done / (double)total) : 0.0f;
    dispatch_async(dispatch_get_main_queue(), ^{ self->_progress.progress = fraction; });
}

- (void)dealloc {
    vm_user_app_plan_close(&_plan);
}
@end
