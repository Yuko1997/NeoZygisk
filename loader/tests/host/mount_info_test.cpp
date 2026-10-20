// Host tests for the mountinfo reader.
//
// The important property is that an unreadable, truncated or partially broken
// mountinfo is never reported as a clean scan: an empty record set must not be
// mistaken for "there is nothing to clean up".

#include <string>

#include "mount_info.hpp"
#include "test_util.hpp"

namespace {

const char *kSample =
        "27 1 254:5 / / ro,nodev,relatime shared:1 - erofs /dev/block/dm-5 ro,seclabel\n"
        "18 27 0:19 / /dev rw,nosuid,relatime shared:2 - tmpfs tmpfs rw,seclabel,mode=755\n"
        "167 27 254:6 /adb/modules/bindhosts/system/etc/hosts /system/etc/hosts rw,nosuid,nodev,noatime "
        "shared:28 - ext4 /dev/block/dm-6 rw,seclabel,resgid=1065,errors=panic\n"
        "24 27 0:24 / /apex rw,nosuid,nodev,noexec,relatime - tmpfs tmpfs rw,seclabel,mode=755\n";

void test_parses_a_complete_snapshot() {
    section("complete snapshot");
    const auto scan = parse_mount_info_text(kSample);
    CHECK(scan.clean());
    CHECK_EQ(std::to_string(scan.records.size()), "4");
    CHECK_EQ(std::to_string(scan.malformed_lines), "0");

    const auto &root = scan.records[0];
    CHECK_EQ(std::to_string(root.id), "27");
    CHECK_EQ(std::to_string(root.parent), "1");
    CHECK_EQ(root.root, "/");
    CHECK_EQ(root.target, "/");
    CHECK_EQ(root.vfs_options, "ro,nodev,relatime shared:1");
    CHECK_EQ(root.type, "erofs");
    CHECK_EQ(root.source, "/dev/block/dm-5");
    CHECK_EQ(root.fs_options, "ro,seclabel");

    const auto &marker = scan.records[2];
    CHECK_EQ(marker.root, "/adb/modules/bindhosts/system/etc/hosts");
    CHECK_EQ(marker.target, "/system/etc/hosts");
    CHECK_EQ(marker.type, "ext4");
    CHECK_EQ(marker.source, "/dev/block/dm-6");
    CHECK_EQ(marker.fs_options, "rw,seclabel,resgid=1065,errors=panic");
}

void test_empty_input_is_not_clean() {
    section("empty input");
    const auto scan = parse_mount_info_text("");
    CHECK(!scan.clean());
    CHECK(scan.records.empty());
}

void test_truncated_tail_is_not_clean() {
    section("truncated tail");
    // The last record is cut in half, so the file never reached EOF.
    std::string truncated(kSample);
    truncated.resize(truncated.size() - 20);
    const auto scan = parse_mount_info_text(truncated);
    CHECK(!scan.clean());
    CHECK(scan.malformed_lines > 0);
}

void test_malformed_line_is_not_clean() {
    section("malformed line");
    const std::string broken =
            std::string(kSample) + "this is not a mountinfo record\n";
    const auto scan = parse_mount_info_text(broken);
    CHECK(!scan.clean());
    CHECK_EQ(std::to_string(scan.malformed_lines), "1");
    // The well formed records are still parsed, but the scan as a whole failed.
    CHECK_EQ(std::to_string(scan.records.size()), "4");
}

void test_malformed_fields_are_rejected() {
    section("malformed fields");
    struct bad_line {
        const char *line;
        bool expect_clean;
    };
    const bad_line cases[] = {
            // A record without optional fields is still a valid record.
            {"27 1 254:5 / / ro - erofs /dev/block/dm-5 ro\n", true},
            // A non numeric mount id.
            {"notanumber 1 254:5 / / ro - erofs /dev/block/dm-5 ro\n", false},
            // A device that is not "major:minor".
            {"27 1 baddevice / / ro - erofs /dev/block/dm-5 ro\n", false},
            // A record that is missing the mount point.
            {"27 1 254:5 / - erofs /dev/block/dm-5 ro\n", false},
            // A record without the " - " separator.
            {"27 1 254:5 / / ro erofs /dev/block/dm-5 ro\n", false},
            // A record without the mount source.
            {"27 1 254:5 / / ro - erofs\n", false},
    };
    for (const auto &test_case : cases) {
        const auto scan = parse_mount_info_text(test_case.line);
        if (test_case.expect_clean) {
            CHECK(scan.clean());
            CHECK_EQ(std::to_string(scan.records.size()), "1");
        } else {
            CHECK(!scan.clean());
            CHECK_EQ(std::to_string(scan.malformed_lines), "1");
        }
    }
}

void test_unreadable_process_is_not_clean() {
    section("unreadable process");
    const auto scan = scan_mount_info("this-pid-does-not-exist");
    CHECK(!scan.clean());
    CHECK(scan.records.empty());
}

void test_self_scan_is_clean() {
    section("self scan");
    const auto scan = scan_mount_info("self");
    CHECK(scan.clean());
    CHECK(!scan.records.empty());
    for (const auto &record : scan.records) {
        CHECK(!record.target.empty());
        CHECK(!record.type.empty());
    }
}

}  // namespace

int main() {
    test_parses_a_complete_snapshot();
    test_empty_input_is_not_clean();
    test_truncated_tail_is_not_clean();
    test_malformed_line_is_not_clean();
    test_malformed_fields_are_rejected();
    test_unreadable_process_is_not_clean();
    test_self_scan_is_clean();
    return test_summary("mount_info_test");
}
