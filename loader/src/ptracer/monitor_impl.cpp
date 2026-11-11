#include <dirent.h>
#include <fcntl.h>
#include <linux/eventpoll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string_view>

#include "daemon.hpp"
#include "files.hpp"
#include "logging.hpp"
#include "monitor.hpp"
#include "utils.hpp"
#include "zygote_next_cleanup.hpp"

namespace {

constexpr const char *kBootIdPath = "/proc/sys/kernel/random/boot_id";
constexpr const char *kNativeRestartMarker =
        WORK_DIRECTORY "/neozygisk-zygote-next-restarted.boot";

std::string read_small_text_file(const char *path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1) return {};
    char buffer[128] = {};
    const ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (bytes <= 0) return {};
    std::string result(buffer, static_cast<size_t>(bytes));
    while (!result.empty() &&
           (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

bool read_process_start_time(pid_t pid, unsigned long long *start_time) {
    const std::string path = "/proc/" + std::to_string(pid) + "/stat";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) return false;

    char buffer[1024] = {};
    const ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (bytes <= 0) return false;

    char *cursor = strrchr(buffer, ')');
    if (cursor == nullptr) return false;
    cursor += 4;  // Skip ')', the state character, and both separators.

    // Fields 4..21 are numeric; field 22 is the process start time.
    for (int field = 4; field <= 22; field++) {
        while (*cursor == ' ') cursor++;
        char *end = nullptr;
        const unsigned long long value = strtoull(cursor, &end, 10);
        if (end == cursor) return false;
        if (field == 22) {
            *start_time = value;
            return true;
        }
        cursor = end;
    }
    return false;
}

bool is_native_zygote_process(pid_t pid);

bool native_cleanup_handled_for_boot() {
    const std::string boot_id = read_small_text_file(kBootIdPath);
    if (boot_id.empty()) {
        LOGE("cannot read the boot ID while checking zygote_next cleanup state");
        return false;
    }
    const std::string marker = read_small_text_file(kNativeRestartMarker);
    const auto first = marker.find('\n');
    if (first == std::string::npos) return false;
    const auto second = marker.find('\n', first + 1);
    if (second == std::string::npos) return false;

    const std::string marker_boot_id = marker.substr(0, first);
    const std::string pid_text = marker.substr(first + 1, second - first - 1);
    const std::string start_text = marker.substr(second + 1);
    if (marker_boot_id != boot_id || pid_text.empty() || start_text.empty()) return false;

    const long pid_value = strtol(pid_text.c_str(), nullptr, 10);
    const unsigned long long start_value = strtoull(start_text.c_str(), nullptr, 10);
    if (pid_value <= 1 || start_value == 0) return false;

    const pid_t marker_pid = static_cast<pid_t>(pid_value);
    if (!is_native_zygote_process(marker_pid)) return false;
    unsigned long long current_start = 0;
    if (!read_process_start_time(marker_pid, &current_start)) return false;
    return current_start == start_value;
}

bool mark_native_cleanup_handled_for_boot(pid_t pid) {
    const std::string boot_id = read_small_text_file(kBootIdPath);
    if (boot_id.empty()) {
        LOGE("cannot read the boot ID after a clean zygote_next session");
        return false;
    }
    unsigned long long start_time = 0;
    if (!read_process_start_time(pid, &start_time)) {
        LOGE("cannot read the start time of native zygote PID %d", pid);
        return false;
    }
    const int fd = open(kNativeRestartMarker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd == -1) {
        PLOGE("write zygote_next cleanup marker");
        return false;
    }
    const std::string marker =
            boot_id + "\n" + std::to_string(pid) + "\n" + std::to_string(start_time) + "\n";
    const ssize_t written = write(fd, marker.data(), marker.size());
    close(fd);
    if (written != static_cast<ssize_t>(marker.size())) {
        LOGE("could not persist the zygote_next cleanup marker");
        return false;
    }
    return true;
}

bool is_native_zygote_process(pid_t pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/exe";
    char target[256] = {};
    const ssize_t length = readlink(path.c_str(), target, sizeof(target) - 1);
    if (length <= 0) return false;
    return std::string_view(target, static_cast<size_t>(length))
            .starts_with("/system/bin/zygote_next");
}

/// Android can start zygote_next before module post-fs-data runs. Such a
/// process is outside the monitor's fork ancestry and can never produce an exec
/// event. Restart it once after init is attached so the replacement is traced.
int restart_preexisting_native_zygote() {
    DIR *proc = opendir("/proc");
    if (proc == nullptr) {
        PLOGE("open /proc while checking zygote_next");
        return 0;
    }

    int restarted = 0;
    while (dirent *entry = readdir(proc)) {
        char *end = nullptr;
        const long value = strtol(entry->d_name, &end, 10);
        if (value <= 1 || end == entry->d_name || *end != '\0') continue;
        const pid_t pid = static_cast<pid_t>(value);
        if (!is_native_zygote_process(pid)) continue;
        LOGW("restarting pre-existing zygote_next PID %d so it can be intercepted", pid);
        if (kill(pid, SIGKILL) == 0) restarted++;
    }
    closedir(proc);
    return restarted;
}

}  // namespace

// --- AppMonitor Method Implementations ---

AppMonitor::AppMonitor()
    : event_loop_(),
      socket_handler_(*this),
      ptrace_handler_(*this),
#if defined(__LP64__)
      zygote_(*this, true),
#else
      zygote_(*this, false),
#endif
      tracing_state_(TRACING) {
}

ZygoteAbiManager &AppMonitor::get_abi_manager() { return zygote_; }

TracingState AppMonitor::get_tracing_state() const { return tracing_state_; }

void AppMonitor::set_tracing_state(TracingState state) { tracing_state_ = state; }

void AppMonitor::write_monitor_status_section(std::string &status_text) {
    status_text += "\tmonitor: \t";
    switch (tracing_state_) {
    case TRACING:
        status_text += "😋 tracing";
        break;
    case STOPPING:
        [[fallthrough]];
    case STOPPED:
        status_text += "❌ stopped";
        break;
    case EXITING:
        status_text += "❌ exited";
        break;
    }
    if (tracing_state_ != TRACING && !monitor_stop_reason_.empty()) {
        status_text += " (";
        status_text += monitor_stop_reason_;
        status_text += ")";
    }
}

void AppMonitor::write_abi_status_section(std::string &status_text, std::string &daemon_detail,
                                          const Status &daemon_status,
                                          bool include_daemon_detail) {
    auto abi_name = this->zygote_.abi_name_;
    if (!daemon_status.supported) return;

    // "monitor: ", "zygote64:" and "daemon64:" are all nine columns wide, so a single
    // following tab lands every state in the same column.
    status_text += "\n\tzygote";
    status_text += abi_name;
    status_text += ":";
    if (tracing_state_ != TRACING) {
        status_text += "\t❓ unknown";
    } else {
        switch (daemon_status.zygote) {
        case ZygoteState::Injected:
            status_text += "\t😋 injected";
            break;
        case ZygoteState::NeedsRestart:
            // Our library is in there, but nothing answered its heartbeat, and a second
            // injection cannot reach a process that already carries it.
            status_text += "\t⚠️ needs restart";
            break;
        default:
            status_text += "\t❌ not injected";
            break;
        }
    }

    status_text += "\n\tdaemon";
    status_text += abi_name;
    status_text += ":";
    if (daemon_status.daemon_running) {
        status_text += "\t😋 running";
        // What the daemon reports about itself -- the root implementation and the
        // modules it loaded -- is a block of its own, not a continuation of this line.
        if (include_daemon_detail) daemon_detail = daemon_status.daemon_info;
    } else {
        status_text += "\t❌ crashed";
        if (include_daemon_detail && !daemon_status.daemon_error_info.empty()) {
            status_text += " (";
            status_text += daemon_status.daemon_error_info;
            status_text += ")";
        }
    }
}

void AppMonitor::update_status() {
    auto prop_file = xopen_file(prop_path_.c_str(), "w");
    if (!prop_file) {
        PLOGE("open module.prop");
        return;
    }

    // The live state of the monitor and of each ABI, as one aligned block.
    std::string status_text;
    write_monitor_status_section(status_text);

    const auto daemon_status = zygote_.get_status();
    std::string daemon_detail;
    write_abi_status_section(status_text, daemon_detail, daemon_status, true);
    write_native_status_section(status_text);

    // Blocks separated by one blank line, so the layout follows what the information is
    // rather than module.prop's key order. An absent block drops out entirely.
    std::stringstream ss;
    for (const auto &block : {metadata_section_, status_text, daemon_detail, description_}) {
        auto trimmed = std::string_view(block);
        while (!trimmed.empty() && trimmed.back() == '\n') trimmed.remove_suffix(1);
        if (trimmed.empty()) continue;
        if (ss.tellp() > 0) ss << "\n\n";
        ss << trimmed;
    }
    ss << "\n";

    std::string final_output = ss.str();
    fwrite(final_output.c_str(), 1, final_output.length(), prop_file.get());

    // The WebUI consumes only this file, so module-controlled text from the
    // daemon info blob can never be interpreted as an authoritative status key.
    const std::string status_temp_path = status_path_ + ".tmp";
    auto status_file = xopen_file(status_temp_path.c_str(), "w");
    if (!status_file) {
        PLOGE("open status");
        return;
    }
    std::string authoritative_status;
    write_monitor_status_section(authoritative_status);
    std::string ignored_detail;
    write_abi_status_section(authoritative_status, ignored_detail, daemon_status, false);
    write_native_status_section(authoritative_status);
    authoritative_status += "\n";
    if (fwrite(authoritative_status.c_str(), 1, authoritative_status.length(), status_file.get()) !=
        authoritative_status.length()) {
        PLOGE("write status");
        status_file.reset();
        unlink(status_temp_path.c_str());
        return;
    }
    if (fflush(status_file.get()) != 0) {
        PLOGE("flush status");
        status_file.reset();
        unlink(status_temp_path.c_str());
        return;
    }
    status_file.reset();
    if (std::rename(status_temp_path.c_str(), status_path_.c_str()) == -1) {
        PLOGE("rename status");
        unlink(status_temp_path.c_str());
    }
}

void AppMonitor::write_native_status_section(std::string &status_text) {
    if (native_zygote_status_.empty()) return;
    status_text += "\n\tzygote_next:";
    if (native_zygote_status_ == "clean") {
        status_text += "\t😋 cleaned";
    } else {
        status_text += "\t❌ ";
        status_text += native_zygote_status_;
    }
}

bool AppMonitor::prepare_environment() {
    prop_path_ = zygiskd::GetTmpPath() + "/module.prop";
    status_path_ = zygiskd::GetTmpPath() + "/status";
    close(open(prop_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    close(open(status_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    // From the module directory, never the working directory: the late-load entry point is
    // a root shell, whose working directory is wherever the user happened to be.
    auto module_prop = get_module_dir() + "/module.prop";
    auto orig_prop = xopen_file(module_prop.c_str(), "r");
    if (orig_prop == nullptr) {
        PLOGE("open %s", module_prop.c_str());
        return false;
    }
    // Two things worth showing: the metadata, as one block whatever order the file lists
    // it in, and the description, which closes the readout.
    file_readline(false, orig_prop.get(), [&](std::string_view line) -> bool {
        // A release feed URL is of no use in a status readout.
        if (line.starts_with("updateJson=")) return true;
        if (line.starts_with("description=")) {
            description_ += line.substr(sizeof("description"));
        } else {
            metadata_section_ += "\t";
            metadata_section_ += line;
        }
        return true;
    });
    update_status();
    return true;
}

void AppMonitor::run() {
    event_loop_.Init();
    // A handler with no usable fd would leave the monitor blind: `ctl` would appear to
    // succeed while nothing ever heard it. The ptrace handler goes first because it is the
    // one that can lose -- seizing init fails for whoever comes second -- while taking the
    // socket unlinks whatever holds the name, which must not happen before that is settled.
    if (!ptrace_handler_.Init()) {
        LOGE("failed to initialise the ptrace handler, exiting...");
        return;
    }
    event_loop_.RegisterHandler(ptrace_handler_, EPOLLIN | EPOLLET);
    if (!socket_handler_.Init()) {
        LOGE("failed to initialise the control socket handler, exiting...");
        return;
    }
    event_loop_.RegisterHandler(socket_handler_, EPOLLIN | EPOLLET);
    // Both init and the control socket are ours now: the module in the work directory is
    // serving the system, so a previous failed hot install no longer needs the marker.
    unlink((zygiskd::GetTmpPath() + "/pending-reboot").c_str());
    event_loop_.Loop();
}

void AppMonitor::request_start() {
    if (tracing_state_ == STOPPING)
        tracing_state_ = TRACING;
    else if (tracing_state_ == STOPPED) {
        ptrace(PTRACE_SEIZE, 1, 0, PTRACE_O_TRACEFORK);
        LOGI("start tracing init");
        tracing_state_ = TRACING;
    }
    update_status();
}

void AppMonitor::request_stop(std::string reason) {
    if (tracing_state_ == TRACING) {
        LOGI("stop tracing requested");
        tracing_state_ = STOPPING;
        monitor_stop_reason_ = std::move(reason);
        ptrace(PTRACE_INTERRUPT, 1, 0, 0);
        update_status();
    }
}

void AppMonitor::request_exit() {
    LOGI("prepare for exit ...");
    tracing_state_ = EXITING;
    monitor_stop_reason_ = "user requested";
    update_status();
    event_loop_.Stop();
}

void AppMonitor::set_native_zygote_status(const std::string &status) {
    native_zygote_status_ = status;
    update_status();
}

RearmResult AppMonitor::request_rearm(bool ignore_disable) {
    LOGI("rearm requested");

    // A rearm asks for injection, so lift a stop the monitor put itself into after a
    // crash loop or a daemon that died with the previous root session.
    if (tracing_state_ != TRACING) {
        request_start();
    }

    auto result = zygote_.rearm(ignore_disable);
    if (result == RearmResult::Failed) {
        request_stop("rearm failed");
        return result;
    }

    update_status();
    return result;
}

void AppMonitor::notify_init_detached() {
    tracing_state_ = STOPPED;
    LOGI("stop tracing init");
}

// --- SocketHandler Method Implementations ---

int AppMonitor::SocketHandler::GetFd() { return sock_fd_; }

AppMonitor::SocketHandler::~SocketHandler() {
    if (sock_fd_ >= 0) close(sock_fd_);
}

void AppMonitor::SocketHandler::answer(const struct sockaddr_un &peer, socklen_t peerlen,
                                       RearmResult result) {
    // An unbound sender has no address to answer at, which is the normal case: only a
    // caller that bound a name wants a reply.
    if (peerlen <= static_cast<socklen_t>(sizeof(sa_family_t))) return;

    auto byte = static_cast<uint8_t>(result);
    if (sendto(sock_fd_, &byte, sizeof(byte), 0, (const struct sockaddr *) &peer, peerlen) == -1) {
        PLOGE("answer the rearm with %s", name_of(result));
    }
}

bool AppMonitor::SocketHandler::Init() {
    sock_fd_ = socket(PF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sock_fd_ == -1) {
        PLOGE("socket create");
        return false;
    }
    struct sockaddr_un addr{
        .sun_family = AF_UNIX,
        .sun_path = {0},
    };
    sprintf(addr.sun_path, "%s/%s", zygiskd::GetTmpPath().c_str(), AppMonitor::SOCKET_NAME);
    socklen_t socklen = sizeof(sa_family_t) + strlen(addr.sun_path);
    // A filesystem socket outlives whoever bound it, and the work directory outlives a
    // root session, so this name can still be held by a monitor that is gone. A live owner
    // is kept out by the caller, so an unbound name here is ours to claim.
    if (unlink(addr.sun_path) == -1 && errno != ENOENT) {
        PLOGE("unlink stale control socket %s", addr.sun_path);
    }
    if (bind(sock_fd_, (struct sockaddr *) &addr, socklen) == -1) {
        PLOGE("bind socket");
        return false;
    }
    return true;
}

void AppMonitor::SocketHandler::HandleEvent([[maybe_unused]] EventLoop &loop, uint32_t) {
    for (;;) {
        buf_.resize(sizeof(CtlMessage));
        CtlMessage &msg_header = *reinterpret_cast<CtlMessage *>(buf_.data());
        ssize_t nread = recv(sock_fd_, &msg_header, sizeof(CtlMessage), MSG_PEEK | MSG_TRUNC);
        if (nread == -1) {
            if (errno == EAGAIN) break;
            PLOGE("SocketHandler: recv(peek)");
            continue;
        }
        ssize_t real_size;
        // Only these carry a payload; every other command is the bare enum value.
        if (msg_header.cmd == Command::REARM || msg_header.cmd == Command::ZYGOTE_INJECTED ||
            msg_header.cmd == Command::DAEMON_SET_INFO ||
            msg_header.cmd == Command::DAEMON_SET_ERROR_INFO) {
            if (static_cast<size_t>(nread) < sizeof(CtlMessage)) {
                LOGE("SocketHandler: incomplete header for command %s (%d), size %zd",
                     name_of(msg_header.cmd), msg_header.cmd, nread);
                recv(sock_fd_, buf_.data(), buf_.size(), 0);
                continue;
            }
            real_size = sizeof(CtlMessage) + msg_header.length;
        } else {
            if (static_cast<size_t>(nread) != sizeof(Command)) {
                LOGE("SocketHandler: invalid size for command %s (%d), size %zd",
                     name_of(msg_header.cmd), msg_header.cmd, nread);
                recv(sock_fd_, buf_.data(), buf_.size(), 0);
                continue;
            }
            real_size = sizeof(Command);
        }
        buf_.resize(real_size);
        CtlMessage &full_msg = *reinterpret_cast<CtlMessage *>(buf_.data());
        // Read the sender's address along with the message: a `ctl rearm` waits for the
        // verdict, and only an address can carry it back.
        struct sockaddr_un peer{};
        socklen_t peerlen = sizeof(peer);
        nread = recvfrom(sock_fd_, &full_msg, real_size, 0, (struct sockaddr *) &peer, &peerlen);
        if (nread == -1) {
            PLOGE("recv(read)");
            continue;
        }
        if (nread != real_size) {
            LOGE("SocketHandler: expected %zd bytes, but received %zd", real_size, nread);
            continue;
        }

        switch (full_msg.cmd) {
        case START:
            monitor_.request_start();
            break;
        case STOP:
            monitor_.request_stop("user requested");
            break;
        case EXIT:
            monitor_.request_exit();
            break;
        case REARM: {
            // The flag rides the request rather than the daemon's environment, so it holds
            // for the rearm that asked for it and for nothing else.
            bool ignore_disable = full_msg.length == 1 && full_msg.data[0] != 0;
            answer(peer, peerlen, monitor_.request_rearm(ignore_disable));
            break;
        }
        case ZYGOTE_INJECTED: {
            // The daemon names the process whose heartbeat it answered, since that is the
            // only hooked process it can vouch for.
            pid_t injected = -1;
            if (full_msg.length == sizeof(injected)) {
                memcpy(&injected, full_msg.data, sizeof(injected));
            } else {
                LOGW("SocketHandler: zygote_injected carries %d bytes, expected %zu",
                     full_msg.length, sizeof(injected));
            }
            monitor_.get_abi_manager().notify_injected(injected);
            monitor_.update_status();
            break;
        }
        case DAEMON_SET_INFO:
            monitor_.get_abi_manager().set_daemon_info({full_msg.data, (size_t) full_msg.length});
            monitor_.update_status();
            break;
        case DAEMON_SET_ERROR_INFO:
            monitor_.get_abi_manager().set_daemon_crashed(
                {full_msg.data, (size_t) full_msg.length});
            monitor_.update_status();
            break;
        case SYSTEM_SERVER_STARTED:
            LOGV("system server started, module.prop updated");
            break;
        default:
            LOGW("SocketHandler: ignoring unknown command %d", full_msg.cmd);
            break;
        }
    }
}

// --- SigChldHandler Method Implementations ---

int AppMonitor::SigChldHandler::GetFd() { return signal_fd_; }
AppMonitor::SigChldHandler::~SigChldHandler() {
    if (signal_fd_ >= 0) close(signal_fd_);
}

bool AppMonitor::SigChldHandler::Init() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
        PLOGE("set sigprocmask");
        return false;
    }
    signal_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd_ == -1) {
        PLOGE("create signalfd");
        return false;
    }
    // Only one process may trace init, so this is also what settles a race between two
    // monitors starting at once: the one that loses cannot do its job and says so.
    if (ptrace(PTRACE_SEIZE, 1, 0, PTRACE_O_TRACEFORK) == -1) {
        PLOGE("seize init");
        return false;
    }
    if (!native_cleanup_handled_for_boot() && restart_preexisting_native_zygote() > 0) {
        monitor_.set_native_zygote_status("restarting pre-existing instance");
    }
    return true;
}

/**
 * @brief The central dispatcher for process state changes and signal handling.
 *
 * This method is the primary entry point for the monitoring logic.
 * It is invoked by the `EventLoop` when the underlying `signalfd` becomes readable,
 * indicating that the kernel has delivered one or more `SIGCHLD` signals.
 *
 * Its responsibility is to consume the signal notification, reap all pending process
 * events using `waitpid`, and dispatch them according to the new hierarchical architecture.
 *
 * @section Architecture: Hierarchical Monitoring
 *
 * To support complex boot chains (e.g., `init -> stub -> zygote`), this handler implements
 * a recursive monitoring strategy rather than a flat list. It categorizes processes into
 * four distinct roles and processes them in strict priority order:
 *
 * 1.  Process Factories (Init & Stubs)
 *     - Criteria: PID is `1` (init) OR present in `stub_processes_`.
 *     - Role: These are parent nodes in the process tree.
 *     - Action: Delegated to `handleParentEvent()`. We monitor these processes
 *       primarily for `PTRACE_EVENT_FORK` to discover new children (potential Zygotes)
 *       or, in the case of stubs, for their termination.
 *
 * 2.  Helper Daemons
 *     - Criteria: PID matches a known `zygiskd` instance.
 *     - Role: Self-monitoring mechanism.
 *     - Action: Checks if the daemon has crashed or exited unexpectedly and
 *       updates the global status accordingly.
 *
 * 3.  Transitioning Candidates
 *     - Criteria: PID is present in the `process_` set.
 *     - Role: These are newly forked children whose identity is not yet established.
 *       They are being traced while waiting for an `execve` syscall.
 *     - Action: Delegated to `handleTracedProcess()`. This determines if the
 *       process has become a Zygote (triggering injection), an intermediate Stub
 *       (triggering promotion to a Process Factory), or an irrelevant process
 *       (triggering detachment).
 *
 * 4.  New Discoveries
 *     - Criteria: PID is unknown.
 *     - Role: Unexpected or previously unobserved children of a monitored parent.
 *     - Action: Delegated to `handleNewProcess()`. The monitor attaches via
 *       `ptrace` with `PTRACE_O_TRACEEXEC` and adds the PID to the candidate set
 *       (`process_`) to await its biological identity.
 */
void AppMonitor::SigChldHandler::HandleEvent(EventLoop &, uint32_t) {
    for (;;) {
        struct signalfd_siginfo fdsi;
        ssize_t s = read(signal_fd_, &fdsi, sizeof(fdsi));
        if (s == -1) {
            if (errno == EAGAIN) break;
            PLOGE("read signalfd");
            continue;
        }
        if (s != sizeof(fdsi) || fdsi.ssi_signo != SIGCHLD) {
            continue;
        }

        int pid;
        while ((pid = waitpid(-1, &status_, __WALL | WNOHANG)) > 0) {
            handleChildEvent(pid, status_);
        }
        if (pid == -1 && errno != ECHILD && monitor_.get_tracing_state() != STOPPED) {
            PLOGE("waitpid");
        }
    }
}

/**
 * @brief The primary dispatcher for child process state changes.
 *
 * This function routes signals caught by waitpid() to the appropriate specialized
 * handler based on the process's current role in our monitoring hierarchy.
 */
void AppMonitor::SigChldHandler::handleChildEvent(int pid, int &status) {
    LOGV("ptrace event pid=%d status=%s", pid, parse_status(status).c_str());
    // Role 1: Process Factories (Init and Stub Zygotes)
    // These processes are monitored for PTRACE_EVENT_FORK to discover new children.
    if (pid == 1 || stub_processes_.count(pid)) {
        handleParentEvent(pid, status);
        return;
    }

    // Role 2: Helper Daemons
    // Check if this is one of our own zygiskd daemon instances exiting.
    if (monitor_.get_abi_manager().handle_daemon_exit_if_match(pid, status)) {
        return;
    }

    // Role 3 & 4: Transitioning Candidates and New Discoveries
    // If the process is known to be in the pre-exec stage, evaluate its state.
    // Otherwise, treat it as a newly discovered process.
    if (process_.count(pid)) {
        handleTracedProcess(pid, status);
    } else {
        handleNewProcess(pid);
    }
}

/**
 * @brief Handles events for parent processes (Init and Stub Zygotes).
 *
 * This handler manages the discovery of new processes via fork() and acts as a
 * shield to protect fragile parent processes (like stub_zygote) from kernel
 * signals generated by our ptrace manipulation of their children.
 */
void AppMonitor::SigChldHandler::handleParentEvent(int pid, int &status) {
    // Case 1: The parent successfully forked a new child.
    if (stopped_with(status, SIGTRAP, PTRACE_EVENT_FORK)) {
        long child_pid;
        if (ptrace(PTRACE_GETEVENTMSG, pid, 0, &child_pid) != -1) {
            LOGV("parent %d forked %ld", pid, child_pid);
        } else {
            PLOGE("geteventmsg on parent %d", pid);
        }
    }
    // Case 2: Init has paused in response to our PTRACE_INTERRUPT stop request.
    else if (pid == 1 && stopped_with(status, SIGTRAP, PTRACE_EVENT_STOP) &&
             monitor_.get_tracing_state() == STOPPING) {
        LOGI("init process safely paused, detaching");
        if (ptrace(PTRACE_DETACH, 1, 0, 0) == -1) PLOGE("detach init failed");
        monitor_.notify_init_detached();
        return;
    }
    // Case 3: An intermediate stub process died naturally or crashed.
    else if (pid != 1 && (WIFEXITED(status) || WIFSIGNALED(status))) {
        LOGI("stub process %d exited (status: %d)", pid, status);
        stub_processes_.erase(pid);
        return;
    }

    // Case 4: The parent was stopped by a standard POSIX signal.
    // We must act as a proxy: deciding whether to suppress the signal or inject it back.
    if (WIFSTOPPED(status)) {
        // WPTEVENT == 0 guarantees this is a standard signal, not a ptrace internal event.
        if (WPTEVENT(status) == 0) {
            int sig = WSTOPSIG(status);

            // Suppress job-control signals.
            // Injecting these back would physically freeze the parent process,
            // causing the entire boot chain to hang.
            if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
                LOGW("suppressing stop signal %s (%d) sent to parent %d", sigabbrev_np(sig), sig,
                     pid);
            }
            // Protect stub_zygote from SIGCHLD.
            // When we freeze/resume its child for injection, the kernel sends SIGCHLD to the stub.
            // By remaining attached and dropping the signal here, the stub remains safely ignorant.
            else if (pid != 1 && sig == SIGCHLD) {
                LOGV("shielding stub process %d from SIGCHLD to prevent native crash", pid);
            }
            // Pass all other signals (like SIGTERM, SIGUSR1) back to the process unaltered.
            else {
                LOGW("passing signal %s (%d) through to parent %d", sigabbrev_np(sig), sig, pid);
                ptrace(PTRACE_CONT, pid, 0, sig);
                return;
            }
        }

        // Resume the process for suppressed signals or any other benign ptrace stops.
        ptrace(PTRACE_CONT, pid, 0, 0);
    }
}

/**
 * @brief Registers and prepares a newly discovered process for execve tracking.
 */
void AppMonitor::SigChldHandler::handleNewProcess(int pid) {
    LOGV("new process %d discovered and attached", pid);
    process_.emplace(pid);

    // Instruct the kernel to stop this process and notify us when it calls execve().
    if (ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACEEXEC) == -1) {
        PLOGE("set PTRACE_O_TRACEEXEC on new process %d", pid);
    }

    // Always resume the process.
    ptrace(PTRACE_CONT, pid, 0, 0);
}

/**
 * @brief Evaluates the state of processes waiting to execute a program.
 *
 * This handler manages the critical race condition window between a process
 * being forked and it calling execve().
 */
void AppMonitor::SigChldHandler::handleTracedProcess(int pid, int &status) {
    bool keep_attached = false;
    // handleExecEvent() overwrites the status while it waits for the target to
    // stop, so the exec event has to be remembered here.
    const bool was_exec = stopped_with(status, SIGTRAP, PTRACE_EVENT_EXEC);

    // The process has called execve(). We must now identify it.
    if (was_exec) {
        keep_attached = handleExecEvent(pid, status);
    }
    // The kernel auto-attaches the forked child and pauses it with PTRACE_EVENT_STOP.
    else if (stopped_with(status, SIGTRAP, PTRACE_EVENT_STOP)) {
        LOGV("process %d acknowledged auto-attach trap, configuring execve tracking", pid);

        // Safely apply the execve trap now that the process is guaranteed stopped and reaped.
        if (ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACEEXEC) == -1) {
            PLOGE("set PTRACE_O_TRACEEXEC on process %d", pid);
        }

        ptrace(PTRACE_CONT, pid, 0, 0);
        keep_attached = true;
    }
    // Unexpected state during the pre-exec phase.
    else {
        LOGW("traced process %d stopped with unexpected status: %s", pid,
             parse_status(status).c_str());
    }

    // Determine lifecycle routing based on the handlers above.
    if (keep_attached) {
        // If handleExecEvent promoted it to a stub or initiated injection,
        // it no longer belongs in the pre-exec candidate pool.
        if (was_exec) {
            process_.erase(pid);
        }
        return;
    }

    // If the process is irrelevant (e.g., a random system daemon), clean up and detach.
    process_.erase(pid);
    if (WIFSTOPPED(status)) {
        LOGV("detaching irrelevant process %d", pid);
        ptrace(PTRACE_DETACH, pid, 0, 0);
    }
}

/**
 * @brief Identifies the biological identity of a process post-execve.
 *
 * @return true if the process was promoted (Stub) or handed off (Zygote).
 *         false if the process is irrelevant and should be detached.
 */
bool AppMonitor::SigChldHandler::handleExecEvent(int pid, int &status) {
    auto program = get_program(pid);
    LOGV("process %d executed program: %s", pid, program.c_str());

    bool handled = false;

    do {
        // --- Intermediate Stub Identification ---
        // If this program is a stub_zygote, we must promote it to a Process Factory.
        // It will remain attached forever so we can shield it from SIGCHLD.
        if (program.find("stub_zygote") != std::string::npos) {
            LOGI("detected stub zygote at %d, promoting to parent monitor", pid);
            stub_processes_.insert(pid);

            // Upgrade tracing options to catch when it forks the real zygote.
            ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACEFORK | PTRACE_O_TRACEEXEC);
            ptrace(PTRACE_CONT, pid, 0, 0);

            handled = true;
            break;
        }

        // --- Zygote Target Validation ---
        if (monitor_.get_tracing_state() != TRACING) {
            LOGW("ignoring potential target %d, tracing state is %s", pid,
                 name_of(monitor_.get_tracing_state()));
            break;
        }

        // Android 17's native zygote has no Java/ART hooks, so it needs a dedicated cleanup path.
        const bool is_native_zygote = program == "/system/bin/zygote_next";
        const bool is_app_zygote = program == monitor_.get_abi_manager().program_path_;
        if (!is_app_zygote && !is_native_zygote) {
            break;  // Irrelevant program, exit block and return false.
        }

        // Android 17's native zygote cannot be injected into. Its mount
        // namespace is repaired in place, while the target is stopped.
        if (is_native_zygote) {
            if (native_zygote_fallback_) {
                LOGW("native zygote %d starts in recovery fallback mode", pid);
                if (ptrace(PTRACE_DETACH, pid, 0, 0) == -1) {
                    PLOGE("release native zygote fallback target %d", pid);
                    kill(pid, SIGKILL);
                }
                monitor_.set_native_zygote_status("cleanup disabled; fallback active");
                handled = true;
                break;
            }

            LOGI("native zygote %d intercepted at exec; starting one-shot mount cleanup", pid);

            // The monitor already owns this automatically attached tracee and
            // has consumed the exec event. Keep the target in that ptrace stop
            // for the whole session; detach/re-seize would create a window in
            // which zygote_next could fork the native isolated service.
            constexpr unsigned long kNativeOptions =
                    PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD;
            if (ptrace(PTRACE_SETOPTIONS, pid, 0, kNativeOptions) == -1) {
                PLOGE("set native zygote ptrace options on %d", pid);
                kill(pid, SIGKILL);
                handle_native_zygote_failure();
            } else {
                const auto result = run_native_zygote_cleanup_session(pid);
                if (result == native_zygote_cleanup_result::CLEAN) {
                    native_zygote_failures_ = 0;
                    monitor_.set_native_zygote_status("clean");
                    if (!mark_native_cleanup_handled_for_boot(pid)) {
                        LOGW("native zygote: clean session completed, but the boot marker was not written");
                    }
                } else if (result ==
                           native_zygote_cleanup_result::FALLBACK_TO_ORIGINAL_NAMESPACE) {
                    native_zygote_failures_ = 0;
                    LOGW("native zygote: cleanup failed for PID %d; original namespace restored",
                         pid);
                    monitor_.set_native_zygote_status("cleanup failed; namespace restored");
                } else {
                    LOGE("native zygote: cleanup failed for PID %d; target terminated", pid);
                    handle_native_zygote_failure();
                }
            }
            handled = true;
            break;
        }

        const char *tracer = monitor_.get_abi_manager().check_and_prepare_injection();
        if (tracer == nullptr) {
            LOGE("failed to prepare injector for target %d", pid);
            break;
        }

        // --- Zygote Handover Sequence ---
        LOGV("intercepted target zygote %d, halting for injector hand-off", pid);

        // Force the process into a standard SIGSTOP state.
        kill(pid, SIGSTOP);
        ptrace(PTRACE_CONT, pid, 0, 0);
        waitpid(pid, &status, __WALL);

        if (stopped_with(status, SIGSTOP, 0)) {
            LOGV("target %d halted, detaching monitor to allow injector seize", pid);

            // Detach, but leave the process frozen (SIGSTOP) for the injector daemon.
            if (ptrace(PTRACE_DETACH, pid, 0, SIGSTOP) == -1) {
                PLOGE("detach target %d", pid);
            }

            // Fork and execute the external injector daemon.
            const pid_t p = fork_dont_care();
            if (p == 0) {
                execl(tracer, basename(tracer), "trace", std::to_string(pid).c_str(), "--spawn",
                      nullptr);
                PLOGE("execute injector daemon");
                // The target is parked in SIGSTOP waiting for an injector that will not
                // arrive, so release it. Killing it feeds a zygote restart loop, which
                // RescueParty answers by rebooting into the "wipe data" prompt.
                kill(pid, SIGCONT);
                _exit(1);
            } else if (p == -1) {
                PLOGE("fork injector daemon");
                kill(pid, SIGCONT);
            }

            handled = true;
        } else {
            LOGE("target %d failed to enter SIGSTOP, status: %s", pid,
                 parse_status(status).c_str());
        }

    } while (false);

    // Ensure state transitions (like zygote_injected flags) are flushed to disk.
    monitor_.update_status();
    return handled;
}

void AppMonitor::SigChldHandler::handle_native_zygote_failure() {
    native_zygote_failures_ += 1;
    if (native_zygote_failures_ >= 2) {
        native_zygote_fallback_ = true;
        monitor_.set_native_zygote_status("cleanup failed twice; fallback armed");
    } else {
        monitor_.set_native_zygote_status("cleanup failed; retrying after restart");
    }
}
