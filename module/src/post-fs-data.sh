#!/system/bin/sh

DEBUG=@DEBUG@

# Debug builds capture every Android log buffer from the earliest module stage.
# Keep a bounded set of rotated files so an unattended boot cannot fill storage.
if [ "$DEBUG" = true ]; then
  DEBUG_LOG_DIR=/data/local/tmp/neozygisk-debug-logs
  DEBUG_LOG_PID="$DEBUG_LOG_DIR/logcat.pid"
  mkdir -p "$DEBUG_LOG_DIR"

  if [ -f "$DEBUG_LOG_PID" ]; then
    old_logcat_pid="$(cat "$DEBUG_LOG_PID" 2>/dev/null)"
    if [ -n "$old_logcat_pid" ] && [ -r "/proc/$old_logcat_pid/cmdline" ] &&
      tr '\000' ' ' <"/proc/$old_logcat_pid/cmdline" | grep -q logcat; then
      kill "$old_logcat_pid" 2>/dev/null
    fi
  fi

  rm -f "$DEBUG_LOG_DIR"/neozygisk-full.log* "$DEBUG_LOG_PID"
  chmod 0755 "$DEBUG_LOG_DIR"
  nohup logcat -b all -v threadtime -f "$DEBUG_LOG_DIR/neozygisk-full.log" \
    -r 16384 -n 8 >/dev/null 2>&1 &
  echo $! >"$DEBUG_LOG_PID"
  (sleep 1; chmod 0664 "$DEBUG_LOG_DIR"/neozygisk-full.log* "$DEBUG_LOG_PID" 2>/dev/null) &
  log -p i -t NeoZygisk "started full debug log capture in $DEBUG_LOG_DIR"
fi

# A shim: the rules live in the tracer, which a manager cannot call directly because it
# invokes boot stages by file name. One tracer is installed, for the ABI it injects.
TRACER=${0%/*}/bin/zygisk-ptrace64
[ -f "$TRACER" ] || TRACER=${0%/*}/bin/zygisk-ptrace32
exec "$TRACER" stage post-fs-data
