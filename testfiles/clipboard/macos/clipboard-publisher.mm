// SPDX-License-Identifier: GPL-2.0-or-later
//
// Test-only native macOS clipboard publisher for the external text paste
// protocol. It is a SEPARATE process that owns the real NSPasteboard for the
// duration of one case; it deliberately does not link inkscape_base or GDK, so
// the publication is genuinely remote to the receiver (no local GDK claim).
//
// Publication: `publish.macos.representations[]` gives every representation its
// own file/sha256/bytes/as, and each is written with ITS OWN bytes
// (setData:forType: / setString:forType:). The legacy `publish.macos{utis, as}`
// form is still accepted and publishes the fixture file under every UTI. A
// representation with `stall: true` is advertised through
// NSPasteboardItemDataProvider but its bytes are never delivered.
//
// Modes (exactly one):
//   --publish       write one manifest fixture and report readiness
//   --snapshot      capture the current general pasteboard to a private file
//   --restore       write a captured snapshot back (ownership preconditions)
//   --read-back     print the current types/changeCount as JSON and exit
//
// Supporting options:
//   --session DIR              driver session dir (lease verification, ready path)
//   --fixture-manifest FILE    fixtures/manifest.json
//   --fixture ID               fixture id inside the manifest
//   --nonce N                  driver nonce, echoed into ready.json
//   --hold-until FILE          stay alive (holding the pasteboard) until FILE exists
//   --hold-timeout-ms N        bounded hold (default 120000); expiry exits 7
//   --ready-path FILE          override <session>/pub/ready.json
//   --blob-dir DIR             blob directory for --snapshot (default <FILE>.d)
//   --expect-change-count N    --restore refuses unless changeCount == N (checked
//                              on entry and again immediately before the write)
//   --force-restore            --restore accepts an incomplete snapshot
//   --allow-partial-snapshot   --snapshot exits 0 but records complete:false; the
//                              driver never passes this and never publishes after
//                              an incomplete snapshot
//
// Exported exit codes (kept stable for the Python driver):
//   0 published / held+released / restored (and verified) / read-back ok
//   2 usage error
//   3 lease missing or not owned by the parent driver process
//   4 publication failed / observed formats do not match the manifest / restore
//     write failed twice or the restored types did not verify
//   5 snapshot incomplete (without --allow-partial-snapshot) or unusable
//   6 restore refused: clipboard ownership changed
//   7 hold timeout elapsed
//
// Restore ordering: every restore object is prepared and validated before the
// pasteboard is touched, the ownership check is repeated immediately before
// clearContents, a failed writeObjects is retried once, and the restored types
// are verified before success is reported. NSPasteboard has no compare-and-swap,
// so a third party can still write between the final check and the write; that
// residual window is not race-free and the driver records it as such.
//
// The process never prints clipboard bytes. Snapshots live only under the
// driver's 0700 session directory with 0600 files and are deleted by the driver,
// and only after a verified restore (otherwise they are retained for recovery).

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Advertises a pasteboard type whose bytes are NEVER delivered: the completion
// callback below never calls [item setData:forType:]. A reader that waits for
// those bytes stays pending until its own deadline, which is exactly what the
// stall case (E08_object_target_stall) must exercise. Nothing else in the
// publisher uses this provider, so normal publication is unchanged.
@interface VACStallDataProvider : NSObject <NSPasteboardItemDataProvider>
@property (nonatomic, copy) NSString *stallType;
@end

@implementation VACStallDataProvider
- (instancetype)initWithType:(NSString *)type
{
    self = [super init];
    if (self) {
        _stallType = [type copy];
    }
    return self;
}

- (void)pasteboard:(NSPasteboard *)pasteboard
              item:(NSPasteboardItem *)item
provideDataForType:(NSPasteboardType)type
{
    // Intentionally empty: never hand data to the pasteboard, never signal
    // completion. Parameters are unused by design.
    (void)pasteboard;
    (void)item;
    (void)type;
}
@end

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitLease = 3;
constexpr int kExitPublish = 4;
constexpr int kExitSnapshot = 5;
constexpr int kExitOwnership = 6;
constexpr int kExitHoldTimeout = 7;

// Per-type and total caps for the user's snapshot. An over-cap type marks the
// snapshot incomplete instead of copying unbounded personal data.
constexpr unsigned long long kMaxBlobBytes = 32ull * 1024ull * 1024ull;
constexpr unsigned long long kMaxTotalBlobBytes = 64ull * 1024ull * 1024ull;

struct Options {
    std::string mode;
    std::string session;
    std::string fixture_manifest;
    std::string fixture;
    std::string nonce;
    std::string hold_until;
    std::string ready_path;
    std::string snapshot_file;
    std::string blob_dir;
    long long hold_timeout_ms = 120000;
    long long expect_change_count = -1;
    bool force_restore = false;
    bool allow_partial_snapshot = false;
};

// One published representation: its own UTI, payload file, declared hash and
// `as` mode. A stalled representation is advertised with a data provider that
// never answers instead of being written with setData:/setString:.
struct Representation {
    std::string uti;
    std::string file;
    std::string sha256;
    NSData *data = nil;
    bool as_string = false;
    bool stall = false;
};

std::string to_std(NSString *value)
{
    if (!value) {
        return {};
    }
    char const *utf8 = [value UTF8String];
    return utf8 ? std::string(utf8) : std::string();
}

NSString *to_ns(std::string const &value)
{
    return [NSString stringWithUTF8String:value.c_str()];
}

void print_error(int code, std::string const &message)
{
    NSDictionary *payload = @{@"error" : to_ns(message), @"code" : @(code)};
    NSData *data = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
    std::string const line =
        data ? to_std([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]) : std::string("{}");
    std::fprintf(stderr, "%s\n", line.c_str());
}

void usage()
{
    std::fprintf(stderr,
                 "usage: vacards-clipboard-publisher MODE [options]\n"
                 "  --publish --session DIR --fixture-manifest FILE --fixture ID --nonce N\n"
                 "            [--hold-until FILE] [--hold-timeout-ms N] [--ready-path FILE]\n"
                 "  --snapshot FILE [--blob-dir DIR] [--allow-partial-snapshot] [--session DIR]\n"
                 "  --restore FILE [--expect-change-count N] [--force-restore] [--session DIR]\n"
                 "  --read-back [--session DIR]\n"
                 "  --help\n");
}

bool take_value(int argc, char **argv, int &i, std::string &out)
{
    if (i + 1 >= argc) {
        return false;
    }
    out = argv[++i];
    return true;
}

bool parse_long(std::string const &text, long long &out)
{
    try {
        std::size_t used = 0;
        long long const value = std::stoll(text, &used, 10);
        if (used != text.size()) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_args(int argc, char **argv, Options &opt)
{
    for (int i = 1; i < argc; ++i) {
        std::string const arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage();
            std::exit(kExitOk);
        } else if (arg == "--publish" || arg == "--snapshot" || arg == "--restore" || arg == "--read-back") {
            if (!opt.mode.empty()) {
                print_error(kExitUsage, "exactly one mode flag is allowed");
                return false;
            }
            opt.mode = arg.substr(2);
        } else if (arg == "--session") {
            if (!take_value(argc, argv, i, opt.session)) return false;
        } else if (arg == "--fixture-manifest") {
            if (!take_value(argc, argv, i, opt.fixture_manifest)) return false;
        } else if (arg == "--fixture") {
            if (!take_value(argc, argv, i, opt.fixture)) return false;
        } else if (arg == "--nonce") {
            if (!take_value(argc, argv, i, opt.nonce)) return false;
        } else if (arg == "--hold-until") {
            if (!take_value(argc, argv, i, opt.hold_until)) return false;
        } else if (arg == "--ready-path") {
            if (!take_value(argc, argv, i, opt.ready_path)) return false;
        } else if (arg == "--blob-dir") {
            if (!take_value(argc, argv, i, opt.blob_dir)) return false;
        } else if (arg == "--hold-timeout-ms") {
            std::string value;
            if (!take_value(argc, argv, i, value) || !parse_long(value, opt.hold_timeout_ms)) return false;
        } else if (arg == "--expect-change-count") {
            std::string value;
            if (!take_value(argc, argv, i, value) || !parse_long(value, opt.expect_change_count)) return false;
        } else if (arg == "--force-restore") {
            opt.force_restore = true;
        } else if (arg == "--allow-partial-snapshot") {
            opt.allow_partial_snapshot = true;
        } else if (arg == "--snapshot-file") {
            if (!take_value(argc, argv, i, opt.snapshot_file)) return false;
        } else if (!arg.empty() && arg[0] != '-' && opt.snapshot_file.empty() &&
                   (opt.mode == "snapshot" || opt.mode == "restore")) {
            opt.snapshot_file = arg;
        } else {
            print_error(kExitUsage, "unknown argument: " + arg);
            return false;
        }
    }
    if (opt.mode.empty()) {
        print_error(kExitUsage, "missing mode flag");
        return false;
    }
    if ((opt.mode == "snapshot" || opt.mode == "restore") && opt.snapshot_file.empty()) {
        print_error(kExitUsage, "missing snapshot file");
        return false;
    }
    if (opt.mode == "publish" && (opt.session.empty() || opt.fixture_manifest.empty() || opt.fixture.empty())) {
        print_error(kExitUsage, "--publish requires --session, --fixture-manifest and --fixture");
        return false;
    }
    return true;
}

NSString *ready_path_for(Options const &opt)
{
    if (!opt.ready_path.empty()) {
        return to_ns(opt.ready_path);
    }
    return [to_ns(opt.session) stringByAppendingPathComponent:@"pub/ready.json"];
}

bool write_data_atomic(NSString *path, NSData *data, std::string &error, int mode = 0644)
{
    NSString *tmp = [path stringByAppendingString:[NSString stringWithFormat:@".tmp.%d", getpid()]];
    NSError *write_error = nil;
    if (![data writeToFile:tmp options:0 error:&write_error]) {
        error = "cannot write " + to_std(tmp) + ": " + to_std(write_error.localizedDescription);
        return false;
    }
    chmod(tmp.fileSystemRepresentation, mode);
    if (rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation) != 0) {
        error = "cannot rename " + to_std(tmp) + ": " + std::strerror(errno);
        [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];
        return false;
    }
    return true;
}

bool write_json_atomic(NSString *path, NSDictionary *payload, std::string &error, int mode = 0644)
{
    NSError *json_error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:payload
                                                   options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                                                     error:&json_error];
    if (!data) {
        error = "cannot serialize JSON: " + to_std(json_error.localizedDescription);
        return false;
    }
    return write_data_atomic(path, data, error, mode);
}

NSDictionary *read_json(NSString *path, std::string &error)
{
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data) {
        error = "cannot read " + to_std(path);
        return nil;
    }
    NSError *json_error = nil;
    id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:&json_error];
    if (![parsed isKindOfClass:[NSDictionary class]]) {
        error = "not a JSON object: " + to_std(path);
        return nil;
    }
    return parsed;
}

NSString *sha256_hex(NSData *data)
{
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    NSMutableString *out = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for (unsigned char byte : digest) {
        [out appendFormat:@"%02x", byte];
    }
    return out;
}

double monotonic_seconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

NSString *iso8601_now()
{
    NSISO8601DateFormatter *formatter = [[NSISO8601DateFormatter alloc] init];
    return [formatter stringFromDate:[NSDate date]];
}

NSString *json_string(NSDictionary *dict, NSString *key)
{
    id value = dict[key];
    return [value isKindOfClass:[NSString class]] ? value : nil;
}

long long json_int(NSDictionary *dict, NSString *key, long long fallback)
{
    id value = dict[key];
    if ([value isKindOfClass:[NSNumber class]]) {
        return [value longLongValue];
    }
    return fallback;
}

// Verify that the driver that launched us still owns the session lease. This
// prevents a stray manual invocation from stealing the user's clipboard
// mid-run. VACARDS_CLIP_REQUIRE_LEASE=0 disables the check for manual probing.
bool verify_lease(Options const &opt, std::string &error)
{
    char const *require = std::getenv("VACARDS_CLIP_REQUIRE_LEASE");
    if (require && std::string(require) == "0") {
        return true;
    }
    if (opt.session.empty()) {
        error = "no session directory";
        return false;
    }
    NSString *lease_path = [to_ns(opt.session) stringByAppendingPathComponent:@"lease.json"];
    std::string read_error;
    NSDictionary *lease = read_json(lease_path, read_error);
    if (!lease) {
        error = "lease record unavailable: " + read_error;
        return false;
    }
    long long const pid = json_int(lease, @"pid", -1);
    if (pid != (long long)getppid()) {
        error = "lease pid " + std::to_string(pid) + " is not the parent process " + std::to_string((long long)getppid());
        return false;
    }
    if (!opt.nonce.empty()) {
        NSString *nonce = json_string(lease, @"nonce");
        if (!nonce || to_std(nonce) != opt.nonce) {
            error = "lease nonce does not match the requested nonce";
            return false;
        }
    }
    return true;
}

// Build one snapshot entry and write its blob when it is usable. A missing
// provider result, an over-cap blob or a failed write marks the whole snapshot
// incomplete instead of silently dropping a type.
NSDictionary *snapshot_entry(NSData *blob, NSPasteboardType type, NSUInteger item_index, NSUInteger type_index,
                             NSMutableArray *reasons, BOOL *complete, NSString *blob_dir,
                             unsigned long long *total_bytes)
{
    if (!blob) {
        *complete = NO;
        [reasons addObject:[NSString stringWithFormat:@"provider-no-data:%@", type]];
        return @{@"uti" : type, @"blob" : [NSNull null], @"bytes" : @0, @"sha256" : [NSNull null]};
    }
    if ((unsigned long long)blob.length > kMaxBlobBytes ||
        *total_bytes + (unsigned long long)blob.length > kMaxTotalBlobBytes) {
        *complete = NO;
        [reasons addObject:[NSString stringWithFormat:@"type-too-large:%@", type]];
        return @{@"uti" : type, @"blob" : [NSNull null], @"bytes" : @(blob.length), @"sha256" : [NSNull null]};
    }
    NSString *name = [NSString stringWithFormat:@"blob-%03lu-%03lu.bin", (unsigned long)item_index,
                                                (unsigned long)type_index];
    NSString *path = [blob_dir stringByAppendingPathComponent:name];
    NSError *write_error = nil;
    if (![blob writeToFile:path options:NSDataWritingAtomic error:&write_error]) {
        *complete = NO;
        [reasons addObject:[NSString stringWithFormat:@"blob-write-failed:%@", type]];
        return @{@"uti" : type, @"blob" : [NSNull null], @"bytes" : @(blob.length), @"sha256" : [NSNull null]};
    }
    chmod(path.fileSystemRepresentation, 0600);
    *total_bytes += (unsigned long long)blob.length;
    return @{@"uti" : type, @"blob" : name, @"bytes" : @(blob.length), @"sha256" : sha256_hex(blob)};
}

// ---------------------------------------------------------------------------
// --read-back
// ---------------------------------------------------------------------------
int mode_read_back(Options const &opt)
{
    if (!opt.session.empty()) {
        std::string lease_error;
        if (!verify_lease(opt, lease_error)) {
            print_error(kExitLease, lease_error);
            return kExitLease;
        }
    }
    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    NSMutableArray *types = [NSMutableArray array];
    for (NSPasteboardType type in [pasteboard types]) {
        [types addObject:type];
    }
    NSDictionary *payload = @{
        @"schema" : @1,
        @"mode" : @"read-back",
        @"pid" : @(getpid()),
        @"ppid" : @(getppid()),
        @"change_count" : @([pasteboard changeCount]),
        @"types" : types,
        @"mono" : @(monotonic_seconds()),
    };
    NSData *data = [NSJSONSerialization dataWithJSONObject:payload options:NSJSONWritingSortedKeys error:nil];
    std::string const line =
        data ? to_std([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]) : std::string("{}");
    std::printf("%s\n", line.c_str());
    return kExitOk;
}

// ---------------------------------------------------------------------------
// --snapshot
// ---------------------------------------------------------------------------
int mode_snapshot(Options const &opt)
{
    std::string lease_error;
    if (!verify_lease(opt, lease_error)) {
        print_error(kExitLease, lease_error);
        return kExitLease;
    }
    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    NSInteger const change_count = [pasteboard changeCount];

    NSString *snapshot_path = to_ns(opt.snapshot_file);
    NSString *blob_dir =
        opt.blob_dir.empty() ? [snapshot_path stringByAppendingString:@".d"] : to_ns(opt.blob_dir);
    NSFileManager *fm = [NSFileManager defaultManager];
    NSError *dir_error = nil;
    [fm createDirectoryAtPath:blob_dir
  withIntermediateDirectories:YES
                   attributes:@{NSFilePosixPermissions : @0700}
                        error:&dir_error];
    if (dir_error) {
        print_error(kExitSnapshot, "cannot create blob dir: " + to_std(dir_error.localizedDescription));
        return kExitSnapshot;
    }

    NSMutableArray *items = [NSMutableArray array];
    NSMutableArray *reasons = [NSMutableArray array];
    BOOL complete = YES;
    unsigned long long total_bytes = 0;

    NSArray<NSPasteboardItem *> *source_items = [pasteboard pasteboardItems];
    if (source_items.count > 0) {
        for (NSUInteger item_index = 0; item_index < source_items.count; ++item_index) {
            NSPasteboardItem *source_item = source_items[item_index];
            NSMutableArray *entries = [NSMutableArray array];
            NSUInteger type_index = 0;
            for (NSPasteboardType type in source_item.types) {
                NSData *blob = [source_item dataForType:type];
                [entries addObject:snapshot_entry(blob, type, item_index, type_index++, reasons, &complete, blob_dir,
                                                  &total_bytes)];
            }
            [items addObject:@{@"types" : entries}];
        }
    } else {
        NSMutableArray *entries = [NSMutableArray array];
        NSUInteger type_index = 0;
        for (NSPasteboardType type in [pasteboard types]) {
            NSData *blob = [pasteboard dataForType:type];
            [entries addObject:snapshot_entry(blob, type, 0, type_index++, reasons, &complete, blob_dir,
                                              &total_bytes)];
        }
        [items addObject:@{@"types" : entries}];
    }

    NSDictionary *payload = @{
        @"schema" : @1,
        @"mode" : @"snapshot",
        @"complete" : @(complete),
        @"pid" : @(getpid()),
        @"change_count" : @(change_count),
        @"created_utc" : iso8601_now(),
        @"items" : items,
        @"incomplete_reasons" : reasons,
    };
    std::string error;
    if (!write_json_atomic(snapshot_path, payload, error, 0600)) {
        print_error(kExitSnapshot, error);
        return kExitSnapshot;
    }
    if (!complete && !opt.allow_partial_snapshot) {
        print_error(kExitSnapshot, "snapshot incomplete");
        return kExitSnapshot;
    }
    std::printf("{\"mode\":\"snapshot\",\"complete\":%s,\"types\":%lu,\"items\":%lu}\n", complete ? "true" : "false",
                (unsigned long)[pasteboard types].count, (unsigned long)items.count);
    return kExitOk;
}

// ---------------------------------------------------------------------------
// --restore
// ---------------------------------------------------------------------------
int mode_restore(Options const &opt)
{
    std::string lease_error;
    if (!verify_lease(opt, lease_error)) {
        print_error(kExitLease, lease_error);
        return kExitLease;
    }
    NSString *snapshot_path = to_ns(opt.snapshot_file);
    std::string error;
    NSDictionary *snapshot = read_json(snapshot_path, error);
    if (!snapshot) {
        print_error(kExitSnapshot, error);
        return kExitSnapshot;
    }
    BOOL const complete = [snapshot[@"complete"] boolValue];
    if (!complete && !opt.force_restore) {
        print_error(kExitSnapshot, "refusing to restore an incomplete snapshot");
        return kExitSnapshot;
    }
    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    // Ownership check #1: refuse before reading or validating snapshot blobs.
    if (opt.expect_change_count >= 0 && [pasteboard changeCount] != (NSInteger)opt.expect_change_count) {
        print_error(kExitOwnership, "clipboard ownership changed: expected changeCount " +
                                        std::to_string(opt.expect_change_count) + ", found " +
                                        std::to_string((long long)[pasteboard changeCount]));
        return kExitOwnership;
    }

    NSString *blob_dir =
        opt.blob_dir.empty() ? [snapshot_path stringByAppendingString:@".d"] : to_ns(opt.blob_dir);
    // Prepare and validate EVERY restore object before the pasteboard is touched:
    // a missing blob or a hash mismatch must leave the user's clipboard intact.
    NSMutableArray<NSPasteboardItem *> *restored_items = [NSMutableArray array];
    for (NSDictionary *item_entry in snapshot[@"items"]) {
        if (![item_entry isKindOfClass:[NSDictionary class]]) {
            continue;
        }
        NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
        NSInteger restored_types = 0;
        for (NSDictionary *type_entry in item_entry[@"types"]) {
            if (![type_entry isKindOfClass:[NSDictionary class]]) {
                continue;
            }
            NSString *type = json_string(type_entry, @"uti");
            NSString *blob_name = json_string(type_entry, @"blob");
            if (!type || !blob_name) {
                continue;
            }
            NSString *blob_path = [blob_dir stringByAppendingPathComponent:blob_name];
            NSData *blob = [NSData dataWithContentsOfFile:blob_path];
            if (!blob) {
                print_error(kExitSnapshot, "missing snapshot blob " + to_std(blob_name));
                return kExitSnapshot;
            }
            NSString *expected = json_string(type_entry, @"sha256");
            if (expected && ![sha256_hex(blob) isEqualToString:expected]) {
                print_error(kExitSnapshot, "snapshot blob hash mismatch " + to_std(blob_name));
                return kExitSnapshot;
            }
            [item setData:blob forType:type];
            restored_types += 1;
        }
        if (restored_types > 0) {
            [restored_items addObject:item];
        }
    }

    // Ownership check #2, as close to the destructive call as the API allows.
    // NSPasteboard has no compare-and-swap: a third party can still write between
    // this check and clearContents. The window is small but real, so the driver
    // keeps the snapshot until the write is verified and the restore outcome is
    // recorded honestly instead of calling the check race-free.
    if (opt.expect_change_count >= 0 && [pasteboard changeCount] != (NSInteger)opt.expect_change_count) {
        print_error(kExitOwnership, "clipboard ownership changed before the write: expected changeCount " +
                                        std::to_string(opt.expect_change_count) + ", found " +
                                        std::to_string((long long)[pasteboard changeCount]));
        return kExitOwnership;
    }

    [pasteboard clearContents];
    BOOL wrote = restored_items.count == 0 ? YES : [pasteboard writeObjects:restored_items];
    if (!wrote) {
        // The failed write left the pasteboard empty; retry once with the same
        // prepared items. The snapshot is the only copy of the user's data at
        // this point, so the driver keeps it if both attempts fail.
        wrote = [pasteboard writeObjects:restored_items];
    }
    if (!wrote) {
        print_error(kExitPublish, "restore writeObjects failed after one retry");
        return kExitPublish;
    }

    // Verified restore: every requested type must be visible again. Only then may
    // the driver delete the snapshot blobs.
    NSArray<NSPasteboardType> *observed = [pasteboard types];
    NSMutableArray<NSString *> *missing_types = [NSMutableArray array];
    for (NSPasteboardItem *item in restored_items) {
        for (NSPasteboardType type in [item types]) {
            if (![observed containsObject:type]) {
                [missing_types addObject:type];
            }
        }
    }
    if (missing_types.count > 0) {
        print_error(kExitPublish, "restore verification failed, missing types: " +
                                      to_std([missing_types componentsJoinedByString:@","]));
        return kExitPublish;
    }
    std::printf("{\"mode\":\"restore\",\"items\":%lu,\"change_count\":%lld}\n", (unsigned long)restored_items.count,
                (long long)[pasteboard changeCount]);
    return kExitOk;
}

// ---------------------------------------------------------------------------
// --publish
// ---------------------------------------------------------------------------
int mode_publish(Options const &opt)
{
    std::string lease_error;
    if (!verify_lease(opt, lease_error)) {
        print_error(kExitLease, lease_error);
        return kExitLease;
    }
    NSString *manifest_path = to_ns(opt.fixture_manifest);
    std::string error;
    NSDictionary *manifest = read_json(manifest_path, error);
    if (!manifest) {
        print_error(kExitPublish, error);
        return kExitPublish;
    }
    NSDictionary *fixture = nil;
    for (NSDictionary *candidate in manifest[@"fixtures"]) {
        if ([candidate isKindOfClass:[NSDictionary class]] &&
            to_std(json_string(candidate, @"id")) == opt.fixture) {
            fixture = candidate;
            break;
        }
    }
    if (!fixture) {
        print_error(kExitPublish, "fixture not found in manifest: " + opt.fixture);
        return kExitPublish;
    }

    // The fixture file/sha256 stay the primary payload (the driver records them),
    // but every representation below carries ITS OWN file/hash/bytes/as.
    NSString *relative = json_string(fixture, @"file");
    NSString *fixture_path = [[manifest_path stringByDeletingLastPathComponent] stringByAppendingPathComponent:relative];
    NSData *fixture_bytes = [NSData dataWithContentsOfFile:fixture_path];
    if (!fixture_bytes) {
        print_error(kExitPublish, "cannot read fixture file " + to_std(fixture_path));
        return kExitPublish;
    }
    NSString *expected_sha = json_string(fixture, @"sha256");
    if (!expected_sha || ![sha256_hex(fixture_bytes) isEqualToString:expected_sha]) {
        print_error(kExitPublish, "fixture hash does not match the manifest");
        return kExitPublish;
    }

    NSDictionary *macos = fixture[@"publish"][@"macos"];
    NSArray *raw_representations = macos[@"representations"];
    if (raw_representations && ![raw_representations isKindOfClass:[NSArray class]]) {
        print_error(kExitPublish, "publish.macos.representations must be an array");
        return kExitPublish;
    }
    if (!raw_representations) {
        // Legacy form: one file, one `as` mode, every declared UTI.
        NSString *as = json_string(macos, @"as");
        NSArray *utis = macos[@"utis"];
        if (![as isEqualToString:@"string"] && ![as isEqualToString:@"data"]) {
            print_error(kExitPublish, "manifest fixture has no supported macos publish mode");
            return kExitPublish;
        }
        if (![utis isKindOfClass:[NSArray class]] || utis.count == 0) {
            print_error(kExitPublish, "manifest fixture has no macos UTIs");
            return kExitPublish;
        }
        NSMutableArray *legacy = [NSMutableArray array];
        for (NSString *uti in utis) {
            [legacy addObject:@{@"uti" : uti, @"file" : relative ?: @"", @"sha256" : expected_sha ?: @"",
                                @"as" : as}];
        }
        raw_representations = legacy;
    }
    if (raw_representations.count == 0) {
        print_error(kExitPublish, "manifest fixture has no macos representations");
        return kExitPublish;
    }

    NSString *base_dir = [manifest_path stringByDeletingLastPathComponent];
    std::vector<Representation> representations;
    for (NSDictionary *entry in raw_representations) {
        if (![entry isKindOfClass:[NSDictionary class]]) {
            print_error(kExitPublish, "manifest representation is not an object");
            return kExitPublish;
        }
        Representation rep;
        NSString *uti = json_string(entry, @"uti");
        NSString *file = json_string(entry, @"file");
        NSString *sha = json_string(entry, @"sha256");
        NSString *as = json_string(entry, @"as");
        if (!uti || !file || !sha || (![as isEqualToString:@"string"] && ![as isEqualToString:@"data"])) {
            print_error(kExitPublish, "manifest representation is missing uti/file/sha256/as");
            return kExitPublish;
        }
        NSData *bytes = [NSData dataWithContentsOfFile:[base_dir stringByAppendingPathComponent:file]];
        if (!bytes) {
            print_error(kExitPublish, "cannot read representation file " + to_std(file));
            return kExitPublish;
        }
        if (![sha256_hex(bytes) isEqualToString:sha]) {
            print_error(kExitPublish, "representation hash does not match the manifest: " + to_std(uti));
            return kExitPublish;
        }
        rep.uti = to_std(uti);
        rep.file = to_std(file);
        rep.sha256 = to_std(sha);
        rep.data = bytes;
        rep.as_string = [as isEqualToString:@"string"];
        rep.stall = [entry[@"stall"] boolValue];
        representations.push_back(rep);
    }

    NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
    // Keep every data provider alive until writeObjects: has returned (the item
    // retains it as well, this is belt and braces for the lazy path).
    NSMutableArray *providers = [NSMutableArray array];
    NSMutableArray *published = [NSMutableArray array];
    NSMutableArray *utis = [NSMutableArray array];
    for (Representation const &rep : representations) {
        NSString *uti = to_ns(rep.uti);
        [utis addObject:uti];
        BOOL wrote = NO;
        if (rep.stall) {
            // Advertise the type with a provider that never answers. This is the
            // only publication path that does not hand over the bytes.
            VACStallDataProvider *provider = [[VACStallDataProvider alloc] initWithType:uti];
            [providers addObject:provider];
            wrote = [item setDataProvider:provider forTypes:@[ uti ]];
        } else if (rep.as_string) {
            NSString *text = [[NSString alloc] initWithData:rep.data encoding:NSUTF8StringEncoding];
            if (!text) {
                print_error(kExitPublish, "representation is not valid UTF-8 for the string publish path: " +
                                              rep.uti);
                return kExitPublish;
            }
            wrote = [item setString:text forType:uti];
        } else {
            wrote = [item setData:rep.data forType:uti];
        }
        if (!wrote) {
            print_error(kExitPublish, "cannot declare type " + rep.uti);
            return kExitPublish;
        }
        [published addObject:@{
            @"uti" : uti,
            @"file" : to_ns(rep.file),
            @"sha256" : to_ns(rep.sha256),
            @"bytes" : @(rep.data.length),
            @"as" : rep.as_string ? @"string" : @"data",
            @"stall" : @(rep.stall),
        }];
    }

    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    NSInteger const change_count_before = [pasteboard changeCount];
    [pasteboard clearContents];
    if (![pasteboard writeObjects:@[ item ]]) {
        print_error(kExitPublish, "writeObjects failed");
        return kExitPublish;
    }
    NSInteger const change_count = [pasteboard changeCount];

    NSMutableArray *observed = [NSMutableArray array];
    for (NSPasteboardType type in [pasteboard types]) {
        [observed addObject:type];
    }
    for (NSString *uti in utis) {
        if (![observed containsObject:uti]) {
            print_error(kExitPublish, "published type missing from read-back: " + to_std(uti));
            return kExitPublish;
        }
    }

    // `representations` is the exactness record the driver compares against the
    // manifest (uti set, per-representation hash and the stall flag).
    NSString *publish_as = @"mixed";
    if (representations.size() == 1) {
        publish_as = representations.front().as_string ? @"string" : @"data";
    }
    NSDictionary *ready = @{
        @"schema" : @1,
        @"mode" : @"publish",
        @"backend" : @"native-macos",
        @"nonce" : to_ns(opt.nonce),
        @"pid" : @(getpid()),
        @"ppid" : @(getppid()),
        @"fixture" : to_ns(opt.fixture),
        @"fixture_file" : relative ?: @"",
        @"fixture_sha256" : expected_sha,
        @"publish_as" : publish_as,
        @"requested_utis" : utis,
        @"representations" : published,
        @"formats_observed" : observed,
        @"change_count_before" : @(change_count_before),
        @"change_count" : @(change_count),
        @"published_mono" : @(monotonic_seconds()),
        @"published_utc" : iso8601_now(),
        @"session" : to_ns(opt.session),
    };
    if (!write_json_atomic(ready_path_for(opt), ready, error)) {
        print_error(kExitPublish, error);
        return kExitPublish;
    }

    if (!opt.hold_until.empty()) {
        NSString *release = to_ns(opt.hold_until);
        NSFileManager *fm = [NSFileManager defaultManager];
        double const deadline = monotonic_seconds() + (double)opt.hold_timeout_ms / 1000.0;
        while (![fm fileExistsAtPath:release]) {
            if (monotonic_seconds() >= deadline) {
                print_error(kExitHoldTimeout, "hold-until file did not appear before the timeout");
                return kExitHoldTimeout;
            }
            [NSThread sleepForTimeInterval:0.025];
        }
    }
    std::printf("{\"mode\":\"publish\",\"fixture\":\"%s\",\"change_count\":%lld,\"types\":%lu}\n", opt.fixture.c_str(),
                (long long)change_count, (unsigned long)observed.count);
    return kExitOk;
}

} // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        Options opt;
        if (!parse_args(argc, argv, opt)) {
            usage();
            return kExitUsage;
        }
        if (opt.mode == "read-back") {
            return mode_read_back(opt);
        }
        if (opt.mode == "snapshot") {
            return mode_snapshot(opt);
        }
        if (opt.mode == "restore") {
            return mode_restore(opt);
        }
        if (opt.mode == "publish") {
            return mode_publish(opt);
        }
        usage();
        return kExitUsage;
    }
}
