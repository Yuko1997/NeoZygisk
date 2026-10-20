#include "zygote_next_cleanup.hpp"

#include <elf.h>
#include <fcntl.h>
#include <linux/ptrace.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "logging.hpp"
#include "mount_info.hpp"
#include "mount_marker.hpp"
#include "utils.hpp"
#include "zygote_next_mount_plan.hpp"

namespace {

enum cleanup_stage {
    STAGE_OK = 0,
    STAGE_SETNS = 1,
    STAGE_PRIVATE = 2,
    STAGE_SCAN = 3,
    STAGE_UNMOUNT = 4,
    STAGE_VERIFY = 5,
    STAGE_RESHARE = 6,
    STAGE_GADGET = 10,
    STAGE_UNSHARE = 11,
    STAGE_RESTORE_REGS = 12,
    STAGE_NS_IDENTITY = 13,
    STAGE_HOLD_BEFORE_DETACH = 14,
    STAGE_ROLLBACK_FAILURE = 15,
    STAGE_HELPER_HANG = 16,
};

int requested_failure_stage() {
#ifdef NEOZYGISK_ZYGOTE_NEXT_TESTING
    constexpr const char path[] = "/data/local/tmp/neozygisk_zygote_next_fail_stage";
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1) return -1;
    char buffer[32] = {};
    const ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (bytes <= 0) return -1;
    char *end = nullptr;
    const long requested = strtol(buffer, &end, 10);
    return end != buffer ? static_cast<int>(requested) : -1;
#else
    return -1;
#endif
}

bool injected_failure(int stage) { return requested_failure_stage() == stage; }

int64_t now_ms() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

/// The identity of a mount namespace, taken from the nsfs inode of
/// /proc/<pid>/ns/mnt. A replaced namespace always has a different inode, which
/// is the only proof that unshare() really took effect.
struct ns_identity {
    bool valid = false;
    dev_t dev = 0;
    ino_t ino = 0;

    bool same_as(const ns_identity &other) const {
        return valid && other.valid && dev == other.dev && ino == other.ino;
    }
};

ns_identity read_mount_ns_identity(pid_t pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/ns/mnt";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) return {};
    struct stat st {};
    const bool ok = fstat(fd, &st) == 0;
    close(fd);
    if (!ok) return {};
    ns_identity identity;
    identity.valid = true;
    identity.dev = st.st_dev;
    identity.ino = st.st_ino;
    return identity;
}

/// Reads the single letter process state from /proc/<pid>/stat.
char read_process_state(pid_t pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/stat";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) return '\0';
    char buffer[512];
    const ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (bytes <= 0) return '\0';
    buffer[bytes] = '\0';
    // The command name is parenthesised and may contain spaces, so the state
    // character is the one right behind the closing parenthesis.
    const char *end = strrchr(buffer, ')');
    if (end == nullptr || end[1] == '\0' || end[2] == '\0') return '\0';
    return end[2];
}

bool wait_for_process_state(pid_t pid, const char *accepted, int timeout_ms) {
    const int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        const char state = read_process_state(pid);
        if (state == '\0') return false;
        if (strchr(accepted, state) != nullptr) return true;
        if (now_ms() >= deadline) return false;
        usleep(5000);
    }
}

bool read_ptrace_syscall_info(pid_t pid, struct ptrace_syscall_info *info) {
    const long info_size =
            syscall(__NR_ptrace, PTRACE_GET_SYSCALL_INFO, pid, sizeof(*info), info);
    return info_size > 0;
}

bool is_syscall_stop(int status) {
    return WIFSTOPPED(status) && ((status >> 8) & 0xff) == (SIGTRAP | 0x80);
}

bool wait_for_ptrace_stop(pid_t pid, int *status, int timeout_ms) {
    const int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        const pid_t waited = waitpid(pid, status, __WALL | WNOHANG);
        if (waited == pid) return true;
        if (waited == -1) {
            if (errno == EINTR) continue;
            PLOGE("native zygote: waitpid");
            return false;
        }
        if (now_ms() >= deadline) return false;
        usleep(1000);
    }
}

/// PTRACE_EVENT_EXEC is delivered before the execve syscall-exit stop. The
/// exit stop restores the post-exec register file, so it must be consumed
/// before the remote syscall can overwrite RIP and RAX. This transition stays
/// in the kernel and does not execute user instructions.
bool consume_exec_syscall_exit(pid_t pid) {
    int signal_to_deliver = 0;
    const int64_t deadline = now_ms() + 2000;
    for (int step = 0; step < 8; step++) {
        if (ptrace(PTRACE_SYSCALL, pid, 0, signal_to_deliver) == -1) {
            PLOGE("native zygote: resume to exec syscall exit");
            return false;
        }
        signal_to_deliver = 0;

        int status = 0;
        const int remaining = static_cast<int>(std::max<int64_t>(0, deadline - now_ms()));
        if (!wait_for_ptrace_stop(pid, &status, remaining)) {
            LOGE("native zygote: timed out waiting for exec syscall exit");
            return false;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            LOGE("native zygote: PID %d died before its exec syscall exit", pid);
            return false;
        }
        if (!WIFSTOPPED(status)) return false;

        if (!is_syscall_stop(status)) {
            const int signal = WSTOPSIG(status);
            if (WPTEVENT(status) != 0 || signal == SIGTRAP || signal == SIGSTOP) {
                LOGE("native zygote: unexpected stop before exec syscall exit");
                return false;
            }
            signal_to_deliver = signal;
            continue;
        }

        struct ptrace_syscall_info info {};
        if (!read_ptrace_syscall_info(pid, &info)) {
            PLOGE("native zygote: read exec syscall info");
            return false;
        }
        LOGV("native zygote: exec transition status=0x%x op=%u", status, info.op);
        if (info.op == PTRACE_SYSCALL_INFO_EXIT) return true;
    }
    LOGE("native zygote: exec syscall exit was not observed");
    return false;
}

/// Reads the TracerPid of a process, or -1 when it cannot be determined.
long read_tracer_pid(pid_t pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/status";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) return -1;
    std::string content;
    char buffer[1024];
    for (;;) {
        const ssize_t bytes = read(fd, buffer, sizeof(buffer));
        if (bytes <= 0) break;
        content.append(buffer, static_cast<size_t>(bytes));
        if (content.size() > 16384) break;
    }
    close(fd);
    const size_t pos = content.find("TracerPid:");
    if (pos == std::string::npos) return -1;
    return strtol(content.c_str() + pos + 10, nullptr, 10);
}

/// Checks that the register file of a tracee can be read, i.e. that the tracee
/// really is in a ptrace stop. A tracee that is in a job control stop is not:
/// the kernel refuses register accesses there.
bool registers_reachable(pid_t pid) {
#if defined(__x86_64__) || defined(__i386__)
    struct user_regs_struct regs {};
    return ptrace(PTRACE_GETREGS, pid, 0, &regs) == 0;
#elif defined(__aarch64__) || defined(__arm__)
    struct user_regs_struct regs {};
    struct iovec iov = {.iov_base = &regs, .iov_len = sizeof(regs)};
    return ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov) == 0;
#else
    (void)pid;
    return true;
#endif
}

struct removal_result {
    bool ok = false;
    size_t removed = 0;
    const char *error = "";
};

/// Removes the approved marker subtrees from the current mount namespace.
///
/// The path based umount2() always detaches the topmost mount of a mount point,
/// so the topmost record of every target is resolved from the parent relation
/// before anything is unmounted. A target whose topmost record is not part of
/// the approved removal set is left alone and fails the cleanup: the marker
/// underneath it cannot be reached without deleting an unrelated mount.
removal_result remove_marker_subtrees(const std::vector<mount_info> &baseline,
                                      const neozygisk::zygote_next_mount_plan &plan) {
    removal_result result;

    std::set<unsigned int> removal_ids;
    for (const size_t index : plan.removal) removal_ids.insert(baseline[index].id);

    std::vector<size_t> order = plan.removal;
    std::stable_sort(order.begin(), order.end(),
                     [&plan](size_t a, size_t b) { return plan.depth[a] > plan.depth[b]; });

    size_t previous_pending = removal_ids.size() + 1;
    for (;;) {
        const auto scan = scan_mount_info("self");
        if (!scan.clean()) {
            result.error = "the mount view became unreadable during the cleanup";
            return result;
        }

        std::map<unsigned int, size_t> present;
        for (size_t i = 0; i < scan.records.size(); i++) present.emplace(scan.records[i].id, i);

        size_t pending = 0;
        size_t chosen = 0;
        bool found = false;
        for (const size_t index : order) {
            if (removal_ids.count(baseline[index].id) == 0) continue;
            if (present.count(baseline[index].id) == 0) continue;
            pending += 1;
            if (!found) {
                chosen = index;
                found = true;
            }
        }
        if (!found) break;

        if (pending >= previous_pending) {
            result.error = "the cleanup stopped making progress";
            return result;
        }
        previous_pending = pending;

        const std::string &target = baseline[chosen].target;

        // The topmost record of a mount point is the one that no other record
        // of the same mount point lists as its parent.
        std::set<unsigned int> referenced;
        for (const auto &record : scan.records) referenced.insert(record.parent);
        size_t top = scan.records.size();
        for (size_t i = 0; i < scan.records.size(); i++) {
            if (scan.records[i].target != target) continue;
            if (referenced.count(scan.records[i].id) != 0) continue;
            top = i;
            break;
        }
        if (top == scan.records.size()) {
            result.error = "the mount point of a root managed record vanished";
            return result;
        }
        if (removal_ids.count(scan.records[top].id) == 0) {
            result.error = "a root managed record is covered by an unrelated mount";
            return result;
        }

        if (umount2(target.c_str(), 0) != 0) {
            const int error = errno;
            if (error == EBUSY) {
                // A busy subtree is detached as a whole. The verification below
                // still has to prove that the expected records disappeared.
                if (umount2(target.c_str(), MNT_DETACH) != 0) {
                    result.error = "the busy subtree could not be detached";
                    return result;
                }
            } else if (error != ENOENT && error != EINVAL) {
                result.error = "a root managed mount could not be unmounted";
                return result;
            }
        }
        result.removed += 1;
    }

    result.ok = true;
    return result;
}

struct cleanup_report {
    int32_t status = -1;
    int32_t stage = STAGE_OK;
    int32_t markers_before = 0;
    int32_t markers_after = 0;
    int32_t records_before = 0;
    int32_t records_after = 0;
    int32_t removed = 0;
    int32_t reshared = 0;
    char error[96] = {};
};

void set_error(cleanup_report *report, const char *message) {
    if (message == nullptr) return;
    strncpy(report->error, message, sizeof(report->error) - 1);
}

/// Performs the actual cleanup inside the target's mount namespace.
///
/// This runs in a short lived helper process. The tracer itself never changes
/// its own mount namespace, so a crash of this helper cannot leave the tracer
/// with a foreign view of the system.
cleanup_report run_cleanup_in_namespace(int ns_fd) {
    cleanup_report report;
    const int failure_stage = requested_failure_stage();
    auto fail_if_requested = [&report, failure_stage](int stage, const char *message) {
        if (failure_stage != stage) return false;
        report.stage = stage;
        set_error(&report, message);
        return true;
    };

    if (setns(ns_fd, CLONE_NEWNS) != 0) {
        report.stage = STAGE_SETNS;
        set_error(&report, "the helper could not enter the target mount namespace");
        return report;
    }
    if (failure_stage == STAGE_HELPER_HANG) {
        for (;;) sleep(1);
    }
    if (failure_stage == STAGE_ROLLBACK_FAILURE) {
        report.stage = STAGE_UNMOUNT;
        set_error(&report, "injected cleanup and rollback failure");
        return report;
    }
    if (fail_if_requested(STAGE_SETNS, "injected helper setns failure")) return report;

    // Cut the tree off from every external peer group before anything is
    // enumerated. From here on the namespace cannot be refilled from the
    // outside.
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        report.stage = STAGE_PRIVATE;
        set_error(&report, "the mount tree could not be made private");
        return report;
    }
    if (fail_if_requested(STAGE_PRIVATE, "injected recursive-private failure")) return report;

    const auto baseline = scan_mount_info("self");
    if (!baseline.clean()) {
        report.stage = STAGE_SCAN;
        set_error(&report, "the mount view of the target could not be read");
        return report;
    }

    const char *propagation_error = nullptr;
    if (!neozygisk::verify_recursive_private_propagation(baseline.records, &propagation_error)) {
        report.stage = STAGE_PRIVATE;
        set_error(&report, propagation_error);
        return report;
    }
    report.records_before = static_cast<int32_t>(baseline.records.size());
    if (fail_if_requested(STAGE_SCAN, "injected baseline scan failure")) return report;

    const auto plan = neozygisk::build_zygote_next_mount_plan(baseline.records);
    report.markers_before = static_cast<int32_t>(plan.marker_count);

    std::set<unsigned int> removal_ids;
    for (const size_t index : plan.removal) removal_ids.insert(baseline.records[index].id);

    if (!plan.removal.empty()) {
        if (fail_if_requested(STAGE_UNMOUNT, "injected marker unmount failure")) return report;
        const auto removal = remove_marker_subtrees(baseline.records, plan);
        if (!removal.ok) {
            report.stage = STAGE_UNMOUNT;
            set_error(&report, removal.error);
            return report;
        }
        report.removed = static_cast<int32_t>(removal.removed);
    }

    const auto cleaned = scan_mount_info("self");
    if (!cleaned.clean()) {
        report.stage = STAGE_VERIFY;
        set_error(&report, "the cleaned mount view could not be read");
        return report;
    }
    report.records_after = static_cast<int32_t>(cleaned.records.size());

    const auto comparison =
            neozygisk::compare_zygote_next_snapshots(baseline.records, cleaned.records, removal_ids);
    if (!comparison.ok) {
        report.stage = STAGE_VERIFY;
        set_error(&report, comparison.error);
        return report;
    }
    if (fail_if_requested(STAGE_VERIFY, "injected final snapshot failure")) return report;

    auto verification = neozygisk::verify_zygote_next_mount_view(cleaned.records);
    if (!verification.ok) {
        report.stage = STAGE_VERIFY;
        set_error(&report, verification.error);
        return report;
    }
    report.markers_after = static_cast<int32_t>(verification.markers);

    if (fail_if_requested(STAGE_RESHARE, "injected root reshare failure")) return report;

    // Make the root a self contained peer group. This does not restore any
    // relation to init, it only gives the namespace the shape of a stock init
    // managed namespace so that a namespace comparison stays meaningful.
    if (mount(nullptr, "/", nullptr, MS_SHARED, nullptr) != 0) {
        // Staying private is safe. The namespace observation is informational.
        report.reshared = 0;
    } else {
        report.reshared = 1;
    }

    const auto final_scan = scan_mount_info("self");
    if (!final_scan.clean()) {
        report.stage = STAGE_RESHARE;
        set_error(&report, "the final mount view could not be read");
        return report;
    }
    report.records_after = static_cast<int32_t>(final_scan.records.size());

    const auto final_verification = neozygisk::verify_zygote_next_mount_view(final_scan.records);
    if (!final_verification.ok) {
        report.stage = STAGE_RESHARE;
        set_error(&report, final_verification.error);
        return report;
    }
    report.markers_after = static_cast<int32_t>(final_verification.markers);

    report.status = 0;
    report.stage = STAGE_OK;
    return report;
}

/// Runs the helper and collects its report.
bool spawn_cleanup_helper(int ns_fd, cleanup_report *report) {
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
        PLOGE("native zygote: pipe2");
        return false;
    }

    const pid_t helper = fork();
    if (helper == 0) {
        const pid_t parent = getppid();
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(1);
        close(pipe_fds[0]);
        const cleanup_report child_report = run_cleanup_in_namespace(ns_fd);
        const ssize_t written = write(pipe_fds[1], &child_report, sizeof(child_report));
        (void)written;
        close(pipe_fds[1]);
        _exit(child_report.status == 0 ? 0 : 1);
    }
    if (helper == -1) {
        PLOGE("native zygote: fork cleanup helper");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }

    close(pipe_fds[1]);
    *report = cleanup_report();
    size_t received = 0;
    const int64_t deadline = now_ms() + 5000;
    while (received < sizeof(*report) && now_ms() < deadline) {
        struct pollfd pfd {
            .fd = pipe_fds[0],
            .events = POLLIN | POLLHUP,
            .revents = 0,
        };
        const int remaining = static_cast<int>(std::max<int64_t>(0, deadline - now_ms()));
        const int polled = poll(&pfd, 1, remaining);
        if (polled < 0) {
            if (errno == EINTR) continue;
            PLOGE("native zygote: poll cleanup helper");
            break;
        }
        if (polled == 0) break;

        const ssize_t bytes = read(pipe_fds[0], reinterpret_cast<char *>(report) + received,
                                   sizeof(*report) - received);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0) break;
        received += static_cast<size_t>(bytes);
    }
    close(pipe_fds[0]);

    if (received != sizeof(*report)) {
        LOGE("native zygote: the cleanup helper did not report a result");
        kill(helper, SIGKILL);
    }

    int helper_status = 0;
    bool reaped = false;
    for (int attempt = 0; attempt < 5000; attempt++) {
        const pid_t waited = waitpid(helper, &helper_status, __WALL | WNOHANG);
        if (waited == helper) {
            reaped = true;
            break;
        }
        if (waited == -1) {
            if (errno == EINTR) continue;
            break;
        }
        usleep(1000);
    }
    if (!reaped) {
        LOGE("native zygote: the cleanup helper did not finish");
        kill(helper, SIGKILL);
        waitpid(helper, &helper_status, 0);
        return false;
    }

    if (received != sizeof(*report)) return false;
    return report->status == 0;
}

bool run_remote_syscall_with_backup(pid_t pid, uintptr_t syscall_addr, long syscall_nr,
                                    const std::vector<long> &args, uintptr_t *result) {
    struct user_regs_struct backup {};
    if (!get_regs(pid, backup)) return false;
    struct user_regs_struct regs = backup;
    const bool called = remote_syscall(pid, regs, syscall_nr, args, syscall_addr, result);
    const bool restored = set_regs(pid, backup);
    if (!called) {
        LOGE("native zygote: remote syscall %ld did not complete", syscall_nr);
        return false;
    }
    if (!restored) {
        LOGE("native zygote: registers were not restored after syscall %ld", syscall_nr);
        return false;
    }
    return true;
}

bool open_target_original_namespace(pid_t pid, uintptr_t syscall_addr, int *target_fd) {
    struct user_regs_struct backup {};
    if (!get_regs(pid, backup)) return false;

    struct user_regs_struct regs = backup;
    const uintptr_t path = push_string(pid, regs, "/proc/self/ns/mnt");
    if (path == 0) return false;

    uintptr_t result = 0;
    const bool called =
            remote_syscall(pid, regs, __NR_openat,
                           {AT_FDCWD, static_cast<long>(path), O_RDONLY | O_CLOEXEC, 0},
                           syscall_addr, &result);
    const bool restored = set_regs(pid, backup);
    if (!called || !restored) {
        LOGE("native zygote: could not open the original namespace inside the target");
        return false;
    }

    const long fd = static_cast<long>(result);
    if (fd < 0) {
        LOGE("native zygote: target openat(/proc/self/ns/mnt) failed with %ld", fd);
        return false;
    }
    *target_fd = static_cast<int>(fd);
    return true;
}

bool close_target_fd(pid_t pid, uintptr_t syscall_addr, int fd) {
    if (fd < 0) return true;
    uintptr_t result = 0;
    if (!run_remote_syscall_with_backup(pid, syscall_addr, __NR_close, {fd}, &result)) {
        return false;
    }
    const long close_result = static_cast<long>(result);
    if (close_result != 0) {
        LOGE("native zygote: target close(%d) failed with %ld", fd, close_result);
        return false;
    }
    return true;
}

bool restore_target_original_namespace(pid_t pid, uintptr_t syscall_addr, int target_fd,
                                       const ns_identity &original) {
    uintptr_t result = 0;
    if (!run_remote_syscall_with_backup(pid, syscall_addr, __NR_setns,
                                        {target_fd, CLONE_NEWNS}, &result)) {
        return false;
    }
    const long setns_result = static_cast<long>(result);
    if (setns_result != 0) {
        LOGE("native zygote: target setns(original) failed with %ld", setns_result);
        return false;
    }
    const ns_identity current = read_mount_ns_identity(pid);
    if (!current.valid || !current.same_as(original)) {
        LOGE("native zygote: the original namespace was not restored");
        return false;
    }
    return true;
}

void terminate_native_zygote(pid_t pid) {
    ptrace(PTRACE_DETACH, pid, 0, SIGKILL);
    kill(pid, SIGKILL);
}

native_zygote_cleanup_result recover_failed_native_session(
        pid_t pid, const ns_identity &original, int target_original_fd, bool namespace_changed,
        bool registers_safe) {
    if (!registers_safe || read_process_state(pid) != 't' || !registers_reachable(pid)) {
        LOGE("native zygote: the failed session cannot be recovered safely");
        terminate_native_zygote(pid);
        return native_zygote_cleanup_result::TERMINATED;
    }

    const auto maps = MapInfo::Scan(std::to_string(pid));
    const uintptr_t syscall_addr = find_syscall_gadget(pid, maps);
    if (syscall_addr == 0) {
        LOGE("native zygote: no syscall gadget is available for rollback");
        terminate_native_zygote(pid);
        return native_zygote_cleanup_result::TERMINATED;
    }

    if (namespace_changed && injected_failure(STAGE_ROLLBACK_FAILURE)) {
        LOGE("native zygote: injected namespace rollback failure");
        terminate_native_zygote(pid);
        return native_zygote_cleanup_result::TERMINATED;
    }

    if (namespace_changed &&
        (target_original_fd < 0 ||
         !restore_target_original_namespace(pid, syscall_addr, target_original_fd, original))) {
        LOGE("native zygote: namespace rollback failed; terminating PID %d", pid);
        terminate_native_zygote(pid);
        return native_zygote_cleanup_result::TERMINATED;
    }

    if (!close_target_fd(pid, syscall_addr, target_original_fd)) {
        LOGE("native zygote: could not close the rollback namespace fd; terminating PID %d", pid);
        terminate_native_zygote(pid);
        return native_zygote_cleanup_result::TERMINATED;
    }

    if (ptrace(PTRACE_DETACH, pid, 0, 0) == -1) {
        PLOGE("native zygote: detach after rollback");
        terminate_native_zygote(pid);
        return native_zygote_cleanup_result::TERMINATED;
    }

    if (!wait_for_process_state(pid, "RSDIWZ", 3000) || read_tracer_pid(pid) != 0) {
        LOGE("native zygote: PID %d did not resume after rollback", pid);
        terminate_native_zygote(pid);
        return native_zygote_cleanup_result::TERMINATED;
    }

    LOGW("native zygote: PID %d resumed in its original namespace after cleanup failure", pid);
    return native_zygote_cleanup_result::FALLBACK_TO_ORIGINAL_NAMESPACE;
}

/// Moves the target into a new mount namespace of its own and proves that the
/// namespace really changed.
bool unshare_target_namespace(pid_t pid, const ns_identity &before, ns_identity *after,
                              int *target_original_fd, bool *namespace_changed,
                              bool *registers_safe) {
    *target_original_fd = -1;
    *namespace_changed = false;
    *registers_safe = true;
    if (injected_failure(STAGE_GADGET)) {
        LOGE("native zygote: injected syscall gadget failure");
        return false;
    }
    const auto maps = MapInfo::Scan(std::to_string(pid));
    const uintptr_t syscall_addr = find_syscall_gadget(pid, maps);
    if (syscall_addr == 0) {
        LOGE("native zygote: no syscall gadget found for PID %d", pid);
        return false;
    }

    struct user_regs_struct backup {};
    if (!get_regs(pid, backup)) {
        LOGE("native zygote: cannot read the registers of PID %d", pid);
        *registers_safe = false;
        return false;
    }

    if (!open_target_original_namespace(pid, syscall_addr, target_original_fd)) {
        *registers_safe = registers_reachable(pid);
        return false;
    }

    struct user_regs_struct regs = backup;

    uintptr_t result = 0;
    const bool called =
            remote_syscall(pid, regs, __NR_unshare, {CLONE_NEWNS}, syscall_addr, &result);
    const bool restored = set_regs(pid, backup);
    if (restored && called && static_cast<long>(result) == 0) {
        *namespace_changed = true;
    }
    if (!called) {
        LOGE("native zygote: the remote unshare did not run to completion");
        *registers_safe = restored;
        return false;
    }
    if (!restored) {
        LOGE("native zygote: the registers of PID %d could not be restored", pid);
        *registers_safe = false;
        return false;
    }
    if (injected_failure(STAGE_RESTORE_REGS)) {
        LOGE("native zygote: injected register restoration failure");
        return false;
    }
    if (injected_failure(STAGE_UNSHARE)) {
        LOGE("native zygote: injected unshare failure");
        return false;
    }
    if (static_cast<long>(result) != 0) {
        LOGE("native zygote: unshare(CLONE_NEWNS) failed with %ld", static_cast<long>(result));
        return false;
    }

    const ns_identity current = read_mount_ns_identity(pid);
    if (!current.valid) {
        LOGE("native zygote: the mount namespace of PID %d cannot be read", pid);
        return false;
    }
    if (injected_failure(STAGE_NS_IDENTITY) || current.same_as(before)) {
        LOGE("native zygote: the mount namespace of PID %d did not change", pid);
        *namespace_changed = !current.same_as(before);
        return false;
    }

    *after = current;
    *namespace_changed = true;
    return true;
}

}  // namespace

native_zygote_cleanup_result run_native_zygote_cleanup_session(pid_t pid) {
    // The monitor has already consumed the exec-event stop. The tracee must
    // remain in that same ptrace stop until the verified namespace is released.
    const char state = read_process_state(pid);
    if (state != 't' || !registers_reachable(pid)) {
        LOGE("native zygote: PID %d is not in a usable ptrace stop (state %c)", pid, state);
        terminate_native_zygote(pid);
        return native_zygote_cleanup_result::TERMINATED;
    }

    const ns_identity original = read_mount_ns_identity(pid);
    if (!original.valid) {
        LOGE("native zygote: the mount namespace of PID %d cannot be read", pid);
        terminate_native_zygote(pid);
        return native_zygote_cleanup_result::TERMINATED;
    }

    int target_original_fd = -1;
    bool namespace_changed = false;
    bool registers_safe = true;
    auto fail_session = [&](const char *reason) {
        LOGE("native zygote: %s", reason);
        return recover_failed_native_session(pid, original, target_original_fd, namespace_changed,
                                             registers_safe);
    };

    if (!consume_exec_syscall_exit(pid)) {
        return fail_session("cleanup could not consume the exec syscall exit");
    }

    ns_identity after;
    if (!unshare_target_namespace(pid, original, &after, &target_original_fd, &namespace_changed,
                                  &registers_safe)) {
        return fail_session("cleanup could not enter a verified private namespace");
    }

    const std::string ns_path = "/proc/" + std::to_string(pid) + "/ns/mnt";
    const int ns_fd = open(ns_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (ns_fd == -1) {
        PLOGE("native zygote: open %s", ns_path.c_str());
        return fail_session("cleanup helper namespace fd could not be opened");
    }
    {
        struct stat ns_stat {};
        if (fstat(ns_fd, &ns_stat) != 0 || ns_stat.st_ino != after.ino) {
            LOGE("native zygote: the namespace handle does not match the new namespace");
            close(ns_fd);
            return fail_session("cleanup helper namespace fd did not match the target");
        }
    }

    cleanup_report report;
    const bool cleaned = spawn_cleanup_helper(ns_fd, &report);
    close(ns_fd);
    if (!cleaned) {
        LOGE("native zygote: the mount cleanup failed (stage %d): %s", report.stage,
             report.error[0] != '\0' ? report.error : "unknown error");
        return fail_session("cleanup helper reported a failure");
    }

    LOGI("native zygote: removed %d root managed mount(s) out of %d record(s)", report.removed,
         report.records_before);

    // Independent verification: the tracer reads the target's mount view
    // itself instead of trusting the helper's report.
    const ns_identity current = read_mount_ns_identity(pid);
    if (!current.valid || !current.same_as(after)) {
        LOGE("native zygote: the mount namespace of PID %d changed during the cleanup", pid);
        return fail_session("the target namespace changed during cleanup");
    }
    const auto scan = scan_mount_info(std::to_string(pid));
    if (!scan.clean()) {
        LOGE("native zygote: the mount view of PID %d could not be read", pid);
        return fail_session("the cleaned target mount view could not be read");
    }
    const auto verification = neozygisk::verify_zygote_next_mount_view(scan.records);
    if (!verification.ok) {
        LOGE("native zygote: the target still exposes %zu root managed mount(s)",
             verification.markers);
        return fail_session("the final target mount view failed verification");
    }

    if (injected_failure(STAGE_HOLD_BEFORE_DETACH)) {
        LOGI("native zygote: injected hold before detach for PID %d", pid);
        for (;;) sleep(1);
    }

    const auto target_maps = MapInfo::Scan(std::to_string(pid));
    const uintptr_t syscall_addr = find_syscall_gadget(pid, target_maps);
    if (syscall_addr == 0 ||
        !close_target_fd(pid, syscall_addr, target_original_fd)) {
        return fail_session("the original namespace fd could not be closed before release");
    }
    target_original_fd = -1;

    // The target is still at the original exec-event stop. Delivering SIGCONT
    // would fabricate a group-stop transition that no longer exists.
    if (ptrace(PTRACE_DETACH, pid, 0, 0) == -1) {
        PLOGE("native zygote: PTRACE_DETACH");
        return fail_session("the clean target could not be detached");
    }

    // The target must be running again and no tracer may be left attached.
    for (int attempt = 0; attempt < 200; attempt++) {
        const long tracer_pid = read_tracer_pid(pid);
        if (tracer_pid == 0) break;
        if (tracer_pid < 0) {
            LOGE("native zygote: the tracer state of PID %d cannot be read", pid);
            terminate_native_zygote(pid);
            return native_zygote_cleanup_result::TERMINATED;
        }
        usleep(10000);
    }
    if (read_tracer_pid(pid) != 0) {
        LOGE("native zygote: PID %d still reports a tracer", pid);
        terminate_native_zygote(pid);
        return native_zygote_cleanup_result::TERMINATED;
    }
    if (!wait_for_process_state(pid, "RSDIWZ", 3000)) {
        LOGE("native zygote: PID %d did not resume", pid);
        terminate_native_zygote(pid);
        return native_zygote_cleanup_result::TERMINATED;
    }

    LOGI("native zygote: PID %d released with a clean mount view", pid);
    return native_zygote_cleanup_result::CLEAN;
}
