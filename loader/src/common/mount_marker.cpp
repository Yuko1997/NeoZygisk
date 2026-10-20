#include "mount_marker.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

bool mount_path_is_within(const std::string &path, const std::string &prefix) {
    return path == prefix ||
           (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
            path[prefix.size()] == '/');
}

mount_marker classify_mount_record(const std::string &root, const std::string &target,
                                   const std::string &source) {
    // The source is matched case-insensitively against a fixed set of names, but the
    // path rules below look at the original spelling.
    const std::string source_path = source;
    std::string source_lower = source;
    std::transform(source_lower.begin(), source_lower.end(), source_lower.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });

    mount_marker marker;
    auto add_label = [&marker](const char *label) {
        if (std::find(marker.labels.begin(), marker.labels.end(), label) == marker.labels.end()) {
            marker.labels.emplace_back(label);
        }
    };

    if (source_lower == "magisk") add_label("Magisk");
    if (source_lower == "ksu" || source_lower == "kernelsu") add_label("KernelSU");
    if (source_lower == "zygisk") add_label("Zygisk");
    if (source_lower == "apatch") add_label("APatch");
    if (source_lower == "magic_mount") add_label("Magic mount");

    for (const auto &path : {root, target, source_path}) {
        if (mount_path_is_within(path, "/data/adb")) add_label("data/adb");
        if (mount_path_is_within(path, "/adb/modules")) add_label("ADB modules");
        if (mount_path_is_within(path, "/.magisk") ||
            mount_path_is_within(path, "/sbin/.magisk") ||
            mount_path_is_within(path, "/dev/.magisk") ||
            mount_path_is_within(path, "/debug_ramdisk/.magisk")) {
            add_label("Magisk");
        }
        if (mount_path_is_within(path, "/.magic_mount")) add_label("Magic mount");
        if (mount_path_is_within(path, "/debug_ramdisk")) add_label("debug_ramdisk");
    }

    std::sort(marker.labels.begin(), marker.labels.end());
    marker.labels.erase(std::unique(marker.labels.begin(), marker.labels.end()),
                        marker.labels.end());

    marker.debug_ramdisk = std::find(marker.labels.begin(), marker.labels.end(),
                                      "debug_ramdisk") != marker.labels.end();
    marker.dangerous = std::any_of(
            marker.labels.begin(), marker.labels.end(),
            [](const std::string &label) { return label != "debug_ramdisk"; });
    return marker;
}

bool is_zygote_next_mount_marker(const std::string &root, const std::string &target,
                                  const std::string &source) {
    return classify_mount_record(root, target, source).dangerous;
}
