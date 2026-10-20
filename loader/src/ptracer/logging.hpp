#pragma once

#include "../include/logging.hpp"

#if defined(NEOZYGISK_PTRACE_INTERNAL_LOG) && !defined(LOG_DISABLED)

#ifdef __cplusplus
extern "C" {
#endif
void neozygisk_ptrace_log_write(int priority, const char *format, ...);
#ifdef __cplusplus
}
#endif

#undef LOGD
#undef LOGV
#undef LOGI
#undef LOGW
#undef LOGE
#undef LOGF

#define NEOZYGISK_PTRACE_LOG(priority, ...)                                                        \
    do {                                                                                           \
        __android_log_print(priority, LOG_TAG, __VA_ARGS__);                                       \
        neozygisk_ptrace_log_write(priority, __VA_ARGS__);                                         \
    } while (0)

#ifndef NDEBUG
#define LOGD(fmt, ...)                                                                             \
    NEOZYGISK_PTRACE_LOG(ANDROID_LOG_DEBUG, "%s:%d#%s: " fmt, __FILE_NAME__, __LINE__,            \
                         __PRETTY_FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define LOGV(...) NEOZYGISK_PTRACE_LOG(ANDROID_LOG_VERBOSE, __VA_ARGS__)
#else
#define LOGD(...) 0
#define LOGV(...) 0
#endif

#define LOGI(...) NEOZYGISK_PTRACE_LOG(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGW(...) NEOZYGISK_PTRACE_LOG(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) NEOZYGISK_PTRACE_LOG(ANDROID_LOG_ERROR, __VA_ARGS__)
#define LOGF(...) NEOZYGISK_PTRACE_LOG(ANDROID_LOG_FATAL, __VA_ARGS__)

#endif
