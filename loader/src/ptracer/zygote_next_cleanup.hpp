#pragma once

#include <sys/types.h>

enum class native_zygote_cleanup_result {
    CLEAN,
    FALLBACK_TO_ORIGINAL_NAMESPACE,
    TERMINATED,
};

/// Runs the one-shot Android 17 zygote_next mount cleanup session.
///
/// The target has to be traced by the caller and stopped at the exec event. It
/// is moved into a private mount namespace of its own and every mount record
/// that DuckDetector would report as a root artifact is removed from that
/// namespace. A successful session detaches and resumes the target itself.
///
/// On failure the session rolls the target back to the namespace it entered
/// from and resumes it when possible. If that rollback cannot be verified, the
/// target is terminated. The result tells the monitor which state was reached.
native_zygote_cleanup_result run_native_zygote_cleanup_session(pid_t pid);
