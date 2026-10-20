#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char *kLogPath = "/data/local/tmp/neozygisk-ptrace-debug.log";
constexpr off_t kMaxLogSize = 32 * 1024 * 1024;

struct log_state {
    int fd = -1;
    off_t size = 0;
};

log_state &get_log_state() {
    static log_state state = [] {
        log_state value;
        value.fd = open(kLogPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
        if (value.fd < 0) return value;

        struct stat st {};
        if (fstat(value.fd, &st) == 0) value.size = st.st_size;
        static const char header[] = "\n=== NeoZygisk ptrace internal log start ===\n";
        write(value.fd, header, sizeof(header) - 1);
        value.size += sizeof(header) - 1;
        return value;
    }();
    return state;
}

void write_all(int fd, const char *buffer, size_t length) {
    size_t written = 0;
    while (written < length) {
        const ssize_t result = write(fd, buffer + written, length - written);
        if (result <= 0) return;
        written += static_cast<size_t>(result);
    }
}

char level_name(int priority) {
    switch (priority) {
        case 2:
            return 'V';
        case 3:
            return 'D';
        case 4:
            return 'I';
        case 5:
            return 'W';
        case 6:
            return 'E';
        case 7:
            return 'F';
        default:
            return '?';
    }
}

}  // namespace

extern "C" void neozygisk_ptrace_log_write(int priority, const char *format, ...) {
    log_state &state = get_log_state();
    if (state.fd < 0 || state.size >= kMaxLogSize) return;

    char message[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);

    char line[2304];
    const int length = snprintf(line, sizeof(line), "%lld.%03ld pid=%d %c %s\n",
                                static_cast<long long>(ts.tv_sec), ts.tv_nsec / 1000000, getpid(),
                                level_name(priority), message);
    if (length <= 0) return;
    const size_t line_length =
            static_cast<size_t>(length) < sizeof(line) ? static_cast<size_t>(length)
                                                       : sizeof(line) - 1;
    write_all(state.fd, line, line_length);
    state.size += static_cast<off_t>(line_length);
}
