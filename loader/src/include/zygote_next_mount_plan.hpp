#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "mount_info.hpp"

namespace neozygisk {

/// The mount records that form the approved zygote_next cleanup set.
struct zygote_next_mount_plan {
    /// Parent index for every record, or -1 when the parent is outside the snapshot.
    std::vector<int> parent;
    /// Mount tree depth for every record. The root records have depth 1.
    std::vector<int> depth;
    /// Indices into the snapshot that belong to a direct marker or its subtree.
    std::vector<std::size_t> removal;
    /// Direct marker indices that are not already covered by another marker.
    std::vector<std::size_t> roots;
    std::size_t marker_count = 0;
};

/// Builds the marker subtree plan used by the cleanup helper.
zygote_next_mount_plan build_zygote_next_mount_plan(const std::vector<mount_info> &records);

/// Returns true when a mountinfo option list contains an exact token prefix such
/// as "shared:" or "master:".
bool mount_options_have_prefix(const std::string &options, const char *prefix);

/// Verifies that MS_REC|MS_PRIVATE removed every propagation relationship.
bool verify_recursive_private_propagation(const std::vector<mount_info> &records,
                                          const char **error);

/// Verifies that the final view has no incoming propagation. The root may be a
/// self-contained shared peer group after the optional reshare step.
bool verify_self_contained_propagation(const std::vector<mount_info> &records,
                                       const char **error);

struct zygote_next_view_verification {
    bool ok = false;
    std::size_t markers = 0;
    const char *error = "";
};

/// Checks the detector marker predicate, anchor mounts and final propagation.
zygote_next_view_verification verify_zygote_next_mount_view(
        const std::vector<mount_info> &records);

struct zygote_next_snapshot_comparison {
    bool ok = false;
    const char *error = "";
};

/// Proves that cleanup removed exactly the approved records and left every
/// non-marker record unchanged.
zygote_next_snapshot_comparison compare_zygote_next_snapshots(
        const std::vector<mount_info> &before, const std::vector<mount_info> &after,
        const std::set<unsigned int> &removal_ids);

}  // namespace neozygisk
