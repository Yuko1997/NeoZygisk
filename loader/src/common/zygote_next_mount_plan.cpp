#include "zygote_next_mount_plan.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mount_marker.hpp"

namespace neozygisk {
namespace {

bool same_mount_record(const mount_info &a, const mount_info &b) {
    return a.id == b.id && a.parent == b.parent && a.device == b.device && a.root == b.root &&
           a.target == b.target && a.vfs_options == b.vfs_options && a.type == b.type &&
           a.source == b.source && a.fs_options == b.fs_options;
}

bool mount_covers_anchor(const mount_info &record, const std::string &anchor) {
    if (record.target == anchor) return true;
    // Android's system-as-root devices have no separate /system mount.
    return anchor == "/system" && record.target == "/";
}

}  // namespace

bool mount_options_have_prefix(const std::string &options, const char *prefix) {
    if (prefix == nullptr || *prefix == '\0') return false;
    const std::string needle(prefix);
    std::size_t pos = 0;
    while (pos < options.size()) {
        while (pos < options.size() &&
               (options[pos] == ',' || std::isspace(static_cast<unsigned char>(options[pos])))) {
            pos++;
        }
        if (pos >= options.size()) break;
        std::size_t token_end = pos;
        while (token_end < options.size() && options[token_end] != ',' &&
               !std::isspace(static_cast<unsigned char>(options[token_end]))) {
            token_end++;
        }
        if (token_end - pos >= needle.size() &&
            options.compare(pos, needle.size(), needle) == 0) {
            return true;
        }
        pos = token_end;
    }
    return false;
}

zygote_next_mount_plan build_zygote_next_mount_plan(const std::vector<mount_info> &records) {
    zygote_next_mount_plan plan;
    const std::size_t count = records.size();

    plan.parent.assign(count, -1);
    plan.depth.assign(count, 0);

    std::map<unsigned int, std::size_t> by_id;
    for (std::size_t i = 0; i < count; i++) by_id.emplace(records[i].id, i);
    for (std::size_t i = 0; i < count; i++) {
        const auto it = by_id.find(records[i].parent);
        if (it != by_id.end()) plan.parent[i] = static_cast<int>(it->second);
    }

    for (std::size_t i = 0; i < count; i++) {
        std::vector<std::size_t> chain;
        std::size_t current = i;
        while (current < count && plan.depth[current] == 0) {
            chain.push_back(current);
            const int parent = plan.parent[current];
            current = parent >= 0 ? static_cast<std::size_t>(parent) : count;
        }
        int depth = current < count ? plan.depth[current] : 0;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            plan.depth[*it] = ++depth;
        }
    }

    std::vector<bool> is_marker(count, false);
    for (std::size_t i = 0; i < count; i++) {
        is_marker[i] = is_zygote_next_mount_marker(records[i].root, records[i].target,
                                                   records[i].source);
        if (is_marker[i]) plan.marker_count += 1;
    }

    // Resolve each record's nearest strict marker ancestor.
    std::vector<int> nearest(count, -2);
    std::function<int(std::size_t)> resolve = [&](std::size_t index) -> int {
        if (nearest[index] != -2) return nearest[index];
        nearest[index] = -1;
        int result = -1;
        const int parent = plan.parent[index];
        if (parent >= 0) {
            result = is_marker[parent] ? parent : resolve(static_cast<std::size_t>(parent));
        }
        nearest[index] = result;
        return result;
    };

    for (std::size_t i = 0; i < count; i++) {
        const int ancestor = resolve(i);
        if (is_marker[i] || ancestor >= 0) plan.removal.push_back(i);
        if (is_marker[i] && ancestor < 0) plan.roots.push_back(i);
    }
    return plan;
}

bool verify_recursive_private_propagation(const std::vector<mount_info> &records,
                                          const char **error) {
    for (const auto &record : records) {
        if (mount_options_have_prefix(record.vfs_options, "shared:") ||
            mount_options_have_prefix(record.vfs_options, "master:")) {
            if (error != nullptr) *error = "recursive private left a propagation relationship";
            return false;
        }
    }
    return true;
}

bool verify_self_contained_propagation(const std::vector<mount_info> &records,
                                       const char **error) {
    for (const auto &record : records) {
        if (mount_options_have_prefix(record.vfs_options, "master:")) {
            if (error != nullptr) *error = "the final mount tree receives external propagation";
            return false;
        }
        if (mount_options_have_prefix(record.vfs_options, "shared:") && record.target != "/") {
            if (error != nullptr) *error = "a non-root mount remained shared";
            return false;
        }
    }
    return true;
}

zygote_next_view_verification verify_zygote_next_mount_view(
        const std::vector<mount_info> &records) {
    zygote_next_view_verification result;
    for (const auto &record : records) {
        if (is_zygote_next_mount_marker(record.root, record.target, record.source)) {
            result.markers += 1;
        }
    }
    if (result.markers != 0) {
        result.error = "root managed mount records are still present";
        return result;
    }

    static const char *const kAnchors[] = {"/",      "/dev",     "/proc",   "/sys",
                                           "/data",  "/system",  "/vendor", "/product"};
    for (const char *anchor : kAnchors) {
        const bool found = std::any_of(records.begin(), records.end(),
                                       [anchor](const mount_info &record) {
                                           return mount_covers_anchor(record, anchor);
                                       });
        if (!found) {
            result.error = "an anchor mount disappeared";
            return result;
        }
    }

    const char *propagation_error = nullptr;
    if (!verify_self_contained_propagation(records, &propagation_error)) {
        result.error = propagation_error;
        return result;
    }

    result.ok = true;
    return result;
}

zygote_next_snapshot_comparison compare_zygote_next_snapshots(
        const std::vector<mount_info> &before, const std::vector<mount_info> &after,
        const std::set<unsigned int> &removal_ids) {
    std::map<unsigned int, const mount_info *> after_by_id;
    for (const auto &record : after) after_by_id.emplace(record.id, &record);

    for (const auto &record : before) {
        if (removal_ids.count(record.id) != 0) continue;
        const auto it = after_by_id.find(record.id);
        if (it == after_by_id.end()) {
            return {false, "a mount record outside the removal set disappeared"};
        }
        if (!same_mount_record(record, *it->second)) {
            return {false, "a mount record outside the removal set changed"};
        }
    }

    for (const auto &record : after) {
        if (removal_ids.count(record.id) != 0) {
            return {false, "a root managed mount record survived the cleanup"};
        }
    }

    return {true, ""};
}

}  // namespace neozygisk
