#import "VMGuestLogsViewController.h"
#import "VMInstanceStore.h"
#include "VMGuestInstall.h"
#include "rootfs_work.h"

/*
 * Where iPhone OS 3 keeps crash reports: ReportCrash writes one property list
 * per crash (the report text is its "description" string) under the owner's
 * Logs folder, so apps (user mobile) and daemons (root) land in different
 * places. Paths go through /private because the reader does not follow the
 * /var symlink.
 */
static NSArray<NSString *> *CrashDirectories(void) {
    return @[@"/private/var/mobile/Library/Logs/CrashReporter",
             @"/private/var/logs/CrashReporter",
             @"/private/var/logs/CrashReporter/Panics"];
}

static const size_t kMaxListed = 256u;
static const size_t kMaxReportBytes = 1024u * 1024u;
/* HFS+ dates count from 1904-01-01; Unix time from 1970-01-01. */
static const NSTimeInterval kHFSEpochOffset = 2082844800.0;

@interface VMGuestLogEntry : NSObject
@property (nonatomic, copy) NSString *path;
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *folder;
@property (nonatomic, strong) NSDate *date;
@property (nonatomic) uint64_t size;
@end
@implementation VMGuestLogEntry
@end

/* ------------------------------------------------------------- one report */

@interface VMGuestLogTextViewController : UIViewController
- (instancetype)initWithTitle:(NSString *)title text:(NSString *)text;
@end

@implementation VMGuestLogTextViewController {
    NSString *_text;
    UITextView *_view;
}

- (instancetype)initWithTitle:(NSString *)title text:(NSString *)text {
    self = [super initWithNibName:nil bundle:nil];
    if (!self) return nil;
    self.title = title;
    _text = [text copy];
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
        initWithTitle:@"Copy" style:UIBarButtonItemStylePlain target:self action:@selector(copyText)];
    _view = [[UITextView alloc] init];
    _view.translatesAutoresizingMaskIntoConstraints = NO;
    _view.editable = NO;
    _view.selectable = YES;
    _view.font = [UIFont monospacedSystemFontOfSize:11.0 weight:UIFontWeightRegular];
    _view.text = _text;
    [self.view addSubview:_view];
    [NSLayoutConstraint activateConstraints:@[
        [_view.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [_view.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [_view.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:8],
        [_view.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-8]
    ]];
}

- (void)copyText {
    UIPasteboard.generalPasteboard.string = _text;
    self.navigationItem.rightBarButtonItem.title = @"Copied";
}
@end

/* ------------------------------------------------------------------ list */

@implementation VMGuestLogsViewController {
    NSString *_identifier;
    NSString *_machineName;
    NSArray<VMGuestLogEntry *> *_entries;
    NSString *_message;
    dispatch_queue_t _queue;
    BOOL _loading;
}

- (instancetype)initWithInstanceID:(NSString *)identifier machineName:(NSString *)name {
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    if (!self) return nil;
    _identifier = [identifier copy];
    _machineName = [name copy];
    _entries = @[];
    _queue = dispatch_queue_create("com.j0shua.S5LBox.GuestLogs", DISPATCH_QUEUE_SERIAL);
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Guest Crash Logs";
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh target:self action:@selector(reload)];
    [self reload];
}

- (NSString *)diskPath {
    NSString *work = [[VMInstanceStore sharedStore] directoryForInstanceWithID:_identifier];
    return work.length ? [work stringByAppendingPathComponent:@VM_GUEST_INSTALL_LIVE_FILE] : nil;
}

/* The reader's refusal, in the words the rest of the app uses. */
static NSString *Explain(rootfs_work_status_t status, const rootfs_work_result_t *result) {
    NSString *detail = result->detail[0] ? [NSString stringWithUTF8String:result->detail] : @"";
    if ([detail containsString:@"not cleanly unmounted"])
        return @"Shut down iPhone OS first: hold Power, slide to power off, wait for it to halt, then return to Machines. Crash reports are read from the stopped guest disk; pausing or closing NEON does not cleanly unmount it.";
    return [NSString stringWithFormat:@"The guest disk could not be read (%s). %@",
            rootfs_work_status_name(status), detail];
}

- (void)reload {
    if (_loading) return;
    NSString *disk = [self diskPath];
    if (!disk) { _message = @"This machine no longer exists."; [self.tableView reloadData]; return; }
    _loading = YES;
    _message = @"Reading the guest disk…";
    [self.tableView reloadData];
    dispatch_async(_queue, ^{
        NSMutableArray<VMGuestLogEntry *> *found = [NSMutableArray array];
        NSString *failure = nil;
        rootfs_work_dirent_t *list = calloc(kMaxListed, sizeof *list);
        if (!list) failure = @"Not enough memory to list the guest's crash reports.";
        for (NSString *folder in CrashDirectories()) {
            if (failure) break;
            size_t count = 0u, total = 0u;
            rootfs_work_result_t result;
            rootfs_work_status_t status = rootfs_work_list_directory(
                disk.fileSystemRepresentation, folder.UTF8String, list, kMaxListed,
                &count, &total, &result);
            if (status == ROOTFS_WORK_NOT_FOUND) continue;   /* nothing ever crashed there */
            if (status != ROOTFS_WORK_OK) { failure = Explain(status, &result); break; }
            for (size_t i = 0u; i < count; i++) {
                if (list[i].kind != ROOTFS_WORK_NODE_FILE) continue;
                VMGuestLogEntry *entry = [[VMGuestLogEntry alloc] init];
                entry.name = [NSString stringWithUTF8String:list[i].name] ?: @"(unreadable name)";
                entry.folder = folder;
                entry.path = [folder stringByAppendingPathComponent:entry.name];
                entry.size = list[i].size;
                entry.date = [NSDate dateWithTimeIntervalSince1970:
                    (NSTimeInterval)list[i].modify_time - kHFSEpochOffset];
                [found addObject:entry];
            }
        }
        free(list);
        [found sortUsingComparator:^NSComparisonResult(VMGuestLogEntry *a, VMGuestLogEntry *b) {
            return [b.date compare:a.date];
        }];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_loading = NO;
            self->_entries = failure ? @[] : [found copy];
            self->_message = failure ?: (found.count ? nil :
                @"No crash reports on this guest disk. An app that closes straight after launch without a report was probably stopped rather than crashing (for example by code signing): open it again, then copy Performance & Sound Details, which includes the guest console.");
            [self.tableView reloadData];
        });
    });
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void)tableView;
    return 1;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return (NSInteger)_entries.count;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return _machineName;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return _message;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"log"];
    if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"log"];
    VMGuestLogEntry *entry = _entries[(NSUInteger)indexPath.row];
    cell.textLabel.text = entry.name;
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ · %.1f KB · %@",
        [NSDateFormatter localizedStringFromDate:entry.date dateStyle:NSDateFormatterShortStyle
                                       timeStyle:NSDateFormatterMediumStyle],
        entry.size / 1024.0, entry.folder.lastPathComponent];
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

/* A report is a property list whose "description" is the text; anything else
 * is shown as the property list, or as text. */
static NSString *ReportText(NSData *data) {
    id plist = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable
                                                          format:NULL error:NULL];
    if ([plist isKindOfClass:NSDictionary.class]) {
        id description = plist[@"description"];
        if ([description isKindOfClass:NSString.class]) return description;
        return [plist description];
    }
    if (plist) return [plist description];
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
        ?: [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding]
        ?: @"(unreadable)";
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    VMGuestLogEntry *entry = _entries[(NSUInteger)indexPath.row];
    NSString *disk = [self diskPath];
    if (!disk || _loading) return;
    dispatch_async(_queue, ^{
        uint8_t *bytes = malloc(kMaxReportBytes);
        size_t length = 0u;
        uint64_t size = 0u;
        rootfs_work_result_t result;
        rootfs_work_status_t status = bytes ? rootfs_work_read_file(
            disk.fileSystemRepresentation, entry.path.UTF8String, bytes, kMaxReportBytes,
            &length, &size, &result) : ROOTFS_WORK_NO_MEMORY;
        NSString *text;
        if (status == ROOTFS_WORK_OK) {
            text = ReportText([NSData dataWithBytes:bytes length:length]);
            if (size > length)
                text = [text stringByAppendingFormat:@"\n\n(first %zu of %llu bytes)", length,
                        (unsigned long long)size];
        } else {
            text = bytes ? Explain(status, &result) : @"Not enough memory to read this report.";
        }
        free(bytes);
        dispatch_async(dispatch_get_main_queue(), ^{
            VMGuestLogTextViewController *screen = [[VMGuestLogTextViewController alloc]
                initWithTitle:entry.name text:[NSString stringWithFormat:@"%@\n\n%@", entry.path, text]];
            [self.navigationController pushViewController:screen animated:YES];
        });
    });
}
@end
