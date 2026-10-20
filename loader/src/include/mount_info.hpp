#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

/// A single record of /proc/<pid>/mountinfo.
struct mount_info {
    unsigned int id = 0;
    unsigned int parent = 0;
    dev_t device = 0;
    std::string root;
    std::string target;
    std::string vfs_options;
    std::string type;
    std::string source;
    std::string fs_options;
    std::string raw_info;
};

/// The outcome of a mountinfo read.
///
/// An empty record set is never a synonym for "clean". A read that could not be
/// opened, that was cut short, or that contained a line the parser rejected has to
/// be reported as FAILED, otherwise a broken scan silently turns into "no root
/// artifacts found".
enum class mount_scan_status {
    CLEAN,  ///< The whole file was read and every line was parsed.
    FAILED, ///< The file could not be read completely or a line was malformed.
};

/// The result of scanning one mountinfo file.
struct mount_scan {
    mount_scan_status status = mount_scan_status::FAILED;
    std::vector<mount_info> records;
    unsigned int malformed_lines = 0;

    bool clean() const { return status == mount_scan_status::CLEAN; }
};

/// Reads /proc/<pid>/mountinfo completely and parses every line.
mount_scan scan_mount_info(const std::string &pid);

/// Parses mountinfo content that has already been read into memory.
mount_scan parse_mount_info_text(const std::string &content);
