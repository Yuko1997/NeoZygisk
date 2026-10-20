#include "mount_info.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#if defined(__linux__) || defined(__ANDROID__)
#include <sys/sysmacros.h>
#define NEOZYGISK_MAKEDEV(major, minor) makedev((major), (minor))
#else
// Keep the same 32:12 split that glibc's makedev() uses so the value stays
// comparable across the platforms the host tests run on.
#define NEOZYGISK_MAKEDEV(major, minor)                                            \
    (static_cast<dev_t>(((major) << 8) | ((minor) & 0xff) |                        \
                         (((major) & ~0xff) << 12) | (((minor) & ~0xff) << 12)))
#endif

namespace {

/// Parses one "id parent major:minor root target options - type source super" record.
///
/// The kernel always terminates every record with a newline, so anything that does
/// not fit this shape means the reader saw something it cannot reason about.
bool parse_mount_info_line(const std::string &line, mount_info *info) {
    // The " - " separator is the only guaranteed, unambiguous delimiter on a valid line.
    const size_t separator = line.find(" - ");
    if (separator == std::string::npos) return false;

    size_t pos = 0;
    auto next_field = [&](std::string *out) -> bool {
        while (pos < separator && line[pos] == ' ') pos++;
        if (pos >= separator) return false;
        const size_t start = pos;
        while (pos < separator && line[pos] != ' ') pos++;
        out->assign(line, start, pos - start);
        return !out->empty();
    };

    std::string id_field;
    std::string parent_field;
    std::string device_field;
    if (!next_field(&id_field) || !next_field(&parent_field) || !next_field(&device_field) ||
        !next_field(&info->root) || !next_field(&info->target)) {
        return false;
    }

    char *end = nullptr;
    errno = 0;
    const unsigned long id = strtoul(id_field.c_str(), &end, 10);
    if (errno != 0 || end == id_field.c_str() || *end != '\0') return false;
    info->id = static_cast<unsigned int>(id);

    errno = 0;
    const unsigned long parent = strtoul(parent_field.c_str(), &end, 10);
    if (errno != 0 || end == parent_field.c_str() || *end != '\0') return false;
    info->parent = static_cast<unsigned int>(parent);

    const size_t colon = device_field.find(':');
    if (colon == std::string::npos) return false;
    errno = 0;
    const unsigned long major = strtoul(device_field.c_str(), &end, 10);
    if (errno != 0 || end != device_field.c_str() + colon) return false;
    errno = 0;
    const unsigned long minor = strtoul(device_field.c_str() + colon + 1, &end, 10);
    if (errno != 0 || end == device_field.c_str() + colon + 1 || *end != '\0') return false;
    info->device = NEOZYGISK_MAKEDEV(static_cast<unsigned int>(major),
                                     static_cast<unsigned int>(minor));

    // Everything left on the left-hand side is the mount options plus the optional
    // propagation fields ("shared:N", "master:N", ...).
    while (pos < separator && line[pos] == ' ') pos++;
    info->vfs_options.assign(line, pos, separator - pos);

    pos = separator + 3;
    auto next_right_field = [&](std::string *out) -> bool {
        while (pos < line.size() && line[pos] == ' ') pos++;
        if (pos >= line.size()) return false;
        const size_t start = pos;
        while (pos < line.size() && line[pos] != ' ') pos++;
        out->assign(line, start, pos - start);
        return !out->empty();
    };

    if (!next_right_field(&info->type) || !next_right_field(&info->source)) return false;
    while (pos < line.size() && line[pos] == ' ') pos++;
    info->fs_options.assign(line, pos, line.size() - pos);

    info->raw_info = line;
    return true;
}

}  // namespace

mount_scan parse_mount_info_text(const std::string &content) {
    mount_scan scan;
    scan.records.reserve(64);

    size_t pos = 0;
    while (pos < content.size()) {
        const size_t newline = content.find('\n', pos);
        if (newline == std::string::npos) {
            // Trailing bytes without a terminating newline mean the read was cut short.
            scan.malformed_lines++;
            break;
        }
        const std::string line = content.substr(pos, newline - pos);
        pos = newline + 1;
        if (line.empty()) continue;

        mount_info info;
        if (!parse_mount_info_line(line, &info)) {
            scan.malformed_lines++;
            continue;
        }
        scan.records.push_back(std::move(info));
    }

    // An empty snapshot is never a clean result: it means the reader saw nothing at all.
    scan.status = (scan.malformed_lines == 0 && !scan.records.empty())
                       ? mount_scan_status::CLEAN
                       : mount_scan_status::FAILED;
    return scan;
}

mount_scan scan_mount_info(const std::string &pid) {
    const std::string path = "/proc/" + pid + "/mountinfo";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) return {};

    std::string content;
    bool failed = false;
    char buffer[8192];
    for (;;) {
        const ssize_t bytes = read(fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            if (errno == EINTR) continue;
            failed = true;
            break;
        }
        if (bytes == 0) break;
        content.append(buffer, static_cast<size_t>(bytes));
    }
    close(fd);
    if (failed) return {};

    return parse_mount_info_text(content);
}
