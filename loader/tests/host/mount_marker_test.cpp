// Host tests for the zygote_next mount marker predicate.
//
// The predicate has to stay isomorphic with DuckDetector's
// core/native/src/main/cpp/common/mount_artifact_markers.h at commit
// d3a085e3eaedeb160eda0566e43c3535675a190b. The first half of this file pins the
// documented behaviour, the second half diffs our implementation against the
// vendored detector header over a generated corpus.

#include <algorithm>
#include <string>
#include <vector>

#include "mount_marker.hpp"
#include "reference/mount_artifact_markers.h"
#include "test_util.hpp"

namespace {

bool reference_is_dangerous(const std::string &root, const std::string &target,
                            const std::string &source) {
    const auto labels = duckdetector::common::mount_artifact_labels(root, target, source);
    return std::any_of(labels.begin(), labels.end(),
                       [](const std::string &label) { return label != "debug_ramdisk"; });
}

std::string join_labels(const std::vector<std::string> &labels) {
    std::string joined;
    for (const auto &label : labels) {
        if (!joined.empty()) joined += ",";
        joined += label;
    }
    return joined;
}

void test_source_names() {
    section("source names");
    for (const char *name : {"magisk", "ksu", "kernelsu", "zygisk", "apatch", "magic_mount"}) {
        CHECK(is_zygote_next_mount_marker("/", "/system", name));
    }
    // Case variants of the same names are matched case-insensitively.
    for (const char *name : {"Magisk", "MAGISK", "Ksu", "KSU", "KernelSU", "KERNELSU",
                              "Zygisk", "ZYGISK", "APatch", "apatch", "Magic_Mount",
                              "MAGIC_MOUNT"}) {
        CHECK(is_zygote_next_mount_marker("/", "/system", name));
    }
    // Near misses must not match.
    for (const char *name : {"magiskd", "magisk2", "kernelsu2", "ksud", "apatchd", "magicmount",
                              "magic_mount2", "zygisk2", "magisk.bin", "/dev/magisk"}) {
        CHECK(!is_zygote_next_mount_marker("/", "/system", name));
    }
}

void test_path_prefixes() {
    section("path prefixes");
    const char *prefixes[] = {"/data/adb",      "/adb/modules",  "/.magisk",
                              "/sbin/.magisk",  "/dev/.magisk",  "/debug_ramdisk/.magisk",
                              "/.magic_mount"};
    for (const char *prefix : prefixes) {
        // Exact match.
        CHECK(is_zygote_next_mount_marker(prefix, "/system", "ext4"));
        // Descendant match.
        CHECK(is_zygote_next_mount_marker(std::string(prefix) + "/child", "/system", "ext4"));
        CHECK(is_zygote_next_mount_marker(std::string(prefix) + "/a/b/c", "/system", "ext4"));
        // The prefix may appear in the target or in the source as well.
        CHECK(is_zygote_next_mount_marker("/", prefix, "ext4"));
        CHECK(is_zygote_next_mount_marker("/", "/system", prefix));
    }
}

void test_directory_boundaries() {
    section("directory boundaries");
    CHECK(!is_zygote_next_mount_marker("/data/adb_backup", "/system", "ext4"));
    CHECK(!is_zygote_next_mount_marker("/data/adb_backup/child", "/system", "ext4"));
    CHECK(!is_zygote_next_mount_marker("/adb/modules_backup", "/system", "ext4"));
    CHECK(!is_zygote_next_mount_marker("/adb/modules_backup/child", "/system", "ext4"));
    CHECK(!is_zygote_next_mount_marker("/.magisk_backup", "/system", "ext4"));
    CHECK(!is_zygote_next_mount_marker("/debug_ramdisk/.magisk_backup", "/system", "ext4"));
    CHECK(!is_zygote_next_mount_marker("/.magic_mount_backup", "/system", "ext4"));
    CHECK(!is_zygote_next_mount_marker("/dev/.magisk_backup", "/system", "ext4"));
}

void test_debug_ramdisk_is_contextual() {
    section("debug_ramdisk");
    // A plain debug_ramdisk record only carries the contextual label.
    const auto contextual = classify_mount_record("/", "/debug_ramdisk", "rootfs");
    CHECK(contextual.debug_ramdisk);
    CHECK(!contextual.dangerous);
    CHECK_EQ(join_labels(contextual.labels), "debug_ramdisk");
    CHECK(!is_zygote_next_mount_marker("/", "/debug_ramdisk", "rootfs"));

    // A debug_ramdisk path that also hits a root marker is dangerous.
    CHECK(is_zygote_next_mount_marker("/debug_ramdisk/.magisk", "/debug_ramdisk", "rootfs"));
    const auto mixed = classify_mount_record("/debug_ramdisk/.magisk", "/debug_ramdisk", "rootfs");
    CHECK(mixed.dangerous);
    CHECK(mixed.debug_ramdisk);
    CHECK_EQ(join_labels(mixed.labels), "Magisk,debug_ramdisk");
}

void test_documented_fixtures() {
    section("documented fixtures");
    // A module overlay that hides a system file.
    CHECK(is_zygote_next_mount_marker("/adb/modules/bindhosts/system/etc/hosts", "/system/etc/hosts",
                                      "/dev/block/dm-6"));
    // A module overlay that covers a whole partition.
    CHECK(is_zygote_next_mount_marker("/adb/modules/foo/system", "/system", "overlay"));
    // A root-managed source on a critical partition.
    CHECK(is_zygote_next_mount_marker("/", "/product", "magisk"));
    // Root and source carrying the /data/adb prefix.
    CHECK(is_zygote_next_mount_marker("/data/adb/modules/x", "/somewhere", "ext4"));
    CHECK(is_zygote_next_mount_marker("/", "/somewhere", "/data/adb/modules/x"));
    // Overlay options that merely mention a root path are not structural evidence.
    CHECK(!is_zygote_next_mount_marker("/", "/system", "overlay"));
    CHECK(!is_zygote_next_mount_marker("/", "/system", "/dev/block/dm-5"));
}

void test_differential_against_detector() {
    section("differential against DuckDetector");

    std::vector<std::string> roots = {
        "/",           "/system",    "/vendor",      "/product",   "/data",
        "/data/adb",   "/data/adb/modules/neozygisk", "/data/adb_backup",
        "/adb/modules", "/adb/modules_backup", "/adb/modules/neozygisk/system",
        "/.magisk",    "/sbin/.magisk", "/dev/.magisk", "/debug_ramdisk",
        "/debug_ramdisk/.magisk", "/.magic_mount", "/system/etc/hosts",
        "/dev/block/dm-6", "/dev/block/loop3", "rootfs", "overlay", "tmpfs",
        "/proc",       "/sys",        "/apex",
    };
    std::vector<std::string> targets = {
        "/",             "/system",    "/system/etc/hosts", "/vendor",  "/product",
        "/data",         "/data/adb",  "/adb/modules",     "/.magisk",  "/dev/.magisk",
        "/debug_ramdisk", "/debug_ramdisk/.magisk", "/.magic_mount", "/apex/com.android.art",
        "/mnt/installer", "/linkerconfig", "/metadata", "/dev", "/proc", "/sys",
    };
    std::vector<std::string> sources = {
        "magisk", "Magisk", "MAGISK", "ksu", "KSU", "kernelsu", "KernelSU", "zygisk",
        "Zygisk", "apatch", "APatch", "magic_mount", "Magic_Mount", "ext4", "erofs",
        "overlay", "tmpfs", "rootfs", "proc", "sysfs", "/dev/block/dm-6", "/dev/block/loop3",
        "/data/adb/modules/neozygisk", "/adb/modules/neozygisk/system", "", "magiskd",
        "kernelsu2", "magicmount",
    };

    size_t compared = 0;
    size_t mismatches = 0;
    for (const auto &root : roots) {
        for (const auto &target : targets) {
            for (const auto &source : sources) {
                compared += 1;
                const bool ours = is_zygote_next_mount_marker(root, target, source);
                const bool theirs = reference_is_dangerous(root, target, source);
                if (ours != theirs) {
                    mismatches += 1;
                    if (mismatches < 10) {
                        std::printf("FAIL mismatch root=%s target=%s source=%s ours=%d theirs=%d\n",
                                    root.c_str(), target.c_str(), source.c_str(), int(ours),
                                    int(theirs));
                    }
                }
            }
        }
    }
    std::printf("   compared %zu combinations, %zu mismatch(es)\n", compared, mismatches);
    CHECK(mismatches == 0);

    // The label sets themselves have to match, not just the dangerous flag.
    size_t label_mismatches = 0;
    for (const auto &root : roots) {
        for (const auto &target : targets) {
            for (const auto &source : sources) {
                const auto ours = classify_mount_record(root, target, source);
                const auto theirs =
                        duckdetector::common::mount_artifact_labels(root, target, source);
                if (join_labels(ours.labels) != join_labels(theirs)) {
                    label_mismatches += 1;
                    if (label_mismatches < 10) {
                        std::printf("FAIL label mismatch root=%s target=%s source=%s ours=%s theirs=%s\n",
                                    root.c_str(), target.c_str(), source.c_str(),
                                    join_labels(ours.labels).c_str(), join_labels(theirs).c_str());
                    }
                }
            }
        }
    }
    std::printf("   %zu label mismatch(es)\n", label_mismatches);
    CHECK(label_mismatches == 0);
}

}  // namespace

int main() {
    test_source_names();
    test_path_prefixes();
    test_directory_boundaries();
    test_debug_ramdisk_is_contextual();
    test_documented_fixtures();
    test_differential_against_detector();
    return test_summary("mount_marker_test");
}
