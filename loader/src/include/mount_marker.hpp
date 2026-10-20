#pragma once

#include <string>
#include <vector>

/// The labels DuckDetector attaches to a single mountinfo record.
///
/// The set and the spelling of the labels mirror
/// `core/native/src/main/cpp/common/mount_artifact_markers.h` of DuckDetector
/// commit d3a085e3eaedeb160eda0566e43c3535675a190b, which is the version this
/// project is verified against. The only label that is not a root artifact is
/// "debug_ramdisk"; it is purely contextual.
struct mount_marker {
    /// True when the record carries at least one label other than "debug_ramdisk".
    /// DuckDetector's ZygoteNext probe reports every such record as dangerous.
    bool dangerous = false;
    /// True when the record carries the contextual "debug_ramdisk" label.
    bool debug_ramdisk = false;
    std::vector<std::string> labels;
};

/// Directory-boundary aware prefix match, identical to the detector's helper.
///
/// "/data/adb_backup" is not within "/data/adb" and "/adb/modules_backup" is not
/// within "/adb/modules".
bool mount_path_is_within(const std::string &path, const std::string &prefix);

/// Classifies one mountinfo record exactly like DuckDetector does.
mount_marker classify_mount_record(const std::string &root, const std::string &target,
                                   const std::string &source);

/// True when the record is a root artifact that the DuckDetector ZygoteNext probe
/// reports as a dangerous marker.
bool is_zygote_next_mount_marker(const std::string &root, const std::string &target,
                                  const std::string &source);
