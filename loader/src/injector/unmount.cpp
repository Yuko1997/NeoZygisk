#include <algorithm>  // For std::sort
#include <string>
#include <vector>

#include "logging.hpp"
#include "module.hpp"
#include "zygisk.hpp"

static bool starts_with(const std::string& str, const std::string& prefix) {
    return str.rfind(prefix, 0) == 0;
}

std::vector<mount_info> check_zygote_traces(uint32_t info_flags) {
    std::vector<mount_info> traces;

    const auto scan = scan_mount_info("self");
    if (!scan.clean()) {
        // This is not an error if the parsing simply found no mounts.
        // It could be an error if parsing failed, which is logged in the function itself.
        LOGV("mount info is empty or could not be parsed.");
        return traces;
    }
    const auto &mount_infos = scan.records;

    const char* mount_source_name = nullptr;
    bool is_kernelsu = false;

    if (info_flags & PROCESS_ROOT_IS_APATCH) {
        mount_source_name = "APatch";
    } else if (info_flags & PROCESS_ROOT_IS_KSU) {
        mount_source_name = "KSU";
        is_kernelsu = true;
    } else if (info_flags & PROCESS_ROOT_IS_MAGISK) {
        mount_source_name = "magisk";
    } else {
        LOGE("could not determine root implementation, aborting unmount.");
        return traces;
    }

    std::string kernel_su_module_source;
    if (is_kernelsu) {
        for (const auto& info : mount_infos) {
            if (info.target == "/data/adb/modules" && starts_with(info.source, "/dev/block/loop")) {
                kernel_su_module_source = info.source;
                LOGV("detected KernelSU loop device module source: %s",
                     kernel_su_module_source.c_str());
                break;
            }
        }
    }

    for (const auto& info : mount_infos) {
        const bool should_unmount =
            starts_with(info.root, "/adb/modules") ||
            starts_with(info.target, "/data/adb/modules") || (info.source == mount_source_name) ||
            (!kernel_su_module_source.empty() && info.source == kernel_su_module_source);

        if (should_unmount) {
            traces.push_back(info);
        }
    }

    if (traces.empty()) {
        LOGV("no relevant mount points found to unmount.");
        return traces;
    }

    // Sort the collected traces by mount ID in descending order for safe unmounting
    std::sort(traces.begin(), traces.end(),
              [](const mount_info& a, const mount_info& b) { return a.id > b.id; });

    LOGV("found %zu mounting traces in zygote.", traces.size());

    return traces;
}
