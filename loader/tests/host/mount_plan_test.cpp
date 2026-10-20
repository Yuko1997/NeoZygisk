#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "mount_info.hpp"
#include "test_util.hpp"
#include "zygote_next_mount_plan.hpp"

namespace {

mount_info record(unsigned int id, unsigned int parent, const std::string &target,
                  const std::string &root = "/", const std::string &source = "ext4",
                  const std::string &options = "rw,relatime") {
    mount_info value;
    value.id = id;
    value.parent = parent;
    value.device = 1;
    value.root = root;
    value.target = target;
    value.vfs_options = options;
    value.type = "ext4";
    value.source = source;
    value.fs_options = "rw";
    return value;
}

std::vector<mount_info> anchor_records() {
    std::vector<mount_info> records;
    unsigned int id = 1;
    for (const char *target : {"/", "/dev", "/proc", "/sys", "/data", "/system", "/vendor",
                               "/product"}) {
        records.push_back(record(id, id == 1 ? 0 : 1, target));
        id++;
    }
    return records;
}

void test_mount_option_tokens() {
    section("mount option tokens");
    CHECK(neozygisk::mount_options_have_prefix("rw,relatime shared:4 master:2", "shared:"));
    CHECK(neozygisk::mount_options_have_prefix("master:9,rw", "master:"));
    CHECK(!neozygisk::mount_options_have_prefix("rw,nosuid,nodev", "shared:"));
    CHECK(!neozygisk::mount_options_have_prefix("noshared:4", "shared:"));
}

void test_marker_subtrees() {
    section("marker subtrees");
    const std::vector<mount_info> records = {
            record(1, 0, "/"),
            record(2, 1, "/system"),
            record(3, 2, "/system", "/adb/modules/x/system", "overlay"),
            record(4, 3, "/system/etc/hosts"),
            record(5, 2, "/vendor"),
            record(6, 5, "/product", "/", "magisk"),
            record(7, 6, "/product/private"),
    };

    const auto plan = neozygisk::build_zygote_next_mount_plan(records);
    CHECK_EQ(std::to_string(plan.marker_count), "2");
    CHECK_EQ(std::to_string(plan.removal.size()), "4");
    CHECK_EQ(std::to_string(plan.roots.size()), "2");
    CHECK_EQ(std::to_string(plan.removal[0]), "2");
    CHECK_EQ(std::to_string(plan.removal[1]), "3");
    CHECK_EQ(std::to_string(plan.removal[2]), "5");
    CHECK_EQ(std::to_string(plan.removal[3]), "6");
    CHECK(plan.depth[3] > plan.depth[2]);
    CHECK(plan.depth[6] > plan.depth[5]);
}

void test_private_propagation() {
    section("private propagation");
    auto records = anchor_records();
    const char *error = nullptr;
    CHECK(neozygisk::verify_recursive_private_propagation(records, &error));

    records[0].vfs_options = "rw shared:1";
    CHECK(!neozygisk::verify_recursive_private_propagation(records, &error));
    CHECK(error != nullptr);

    records[0].vfs_options = "rw master:1";
    CHECK(!neozygisk::verify_recursive_private_propagation(records, &error));
}

void test_final_propagation() {
    section("final propagation");
    auto records = anchor_records();
    const char *error = nullptr;
    records[0].vfs_options = "rw shared:10";
    CHECK(neozygisk::verify_self_contained_propagation(records, &error));

    records[1].vfs_options = "rw shared:11";
    CHECK(!neozygisk::verify_self_contained_propagation(records, &error));
    records[1].vfs_options = "rw";

    records[4].vfs_options = "rw master:2";
    CHECK(!neozygisk::verify_self_contained_propagation(records, &error));
}

void test_view_verification() {
    section("view verification");
    auto records = anchor_records();
    records[0].vfs_options = "rw shared:10";
    const auto clean = neozygisk::verify_zygote_next_mount_view(records);
    CHECK(clean.ok);
    CHECK_EQ(std::to_string(clean.markers), "0");

    records.push_back(record(20, 6, "/product", "/", "magisk"));
    const auto marker = neozygisk::verify_zygote_next_mount_view(records);
    CHECK(!marker.ok);
    CHECK_EQ(std::to_string(marker.markers), "1");

    records.clear();
    const auto missing_anchor = neozygisk::verify_zygote_next_mount_view(records);
    CHECK(!missing_anchor.ok);

    records = anchor_records();
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [](const mount_info &value) {
                                     return value.target == "/vendor";
                                 }),
                  records.end());
    const auto missing_vendor = neozygisk::verify_zygote_next_mount_view(records);
    CHECK(!missing_vendor.ok);
}

void test_snapshot_comparison() {
    section("snapshot comparison");
    const std::vector<mount_info> before = {
            record(1, 0, "/"),
            record(2, 1, "/system"),
            record(3, 2, "/system", "/adb/modules/x/system", "overlay"),
    };

    const std::vector<mount_info> unchanged = {
            record(1, 0, "/"),
            record(2, 1, "/system"),
    };
    const std::set<unsigned int> removal = {3};
    CHECK(neozygisk::compare_zygote_next_snapshots(before, unchanged, removal).ok);

    auto missing = unchanged;
    missing.pop_back();
    CHECK(!neozygisk::compare_zygote_next_snapshots(before, missing, removal).ok);

    auto changed = unchanged;
    changed[1].source = "erofs";
    CHECK(!neozygisk::compare_zygote_next_snapshots(before, changed, removal).ok);

    auto survived = unchanged;
    survived.push_back(before[2]);
    CHECK(!neozygisk::compare_zygote_next_snapshots(before, survived, removal).ok);
}

}  // namespace

int main() {
    test_mount_option_tokens();
    test_marker_subtrees();
    test_private_propagation();
    test_final_propagation();
    test_view_verification();
    test_snapshot_comparison();
    return test_summary("mount_plan_test");
}
