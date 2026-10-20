#!/system/bin/sh

MODDIR=${0%/*}
if [ "$ZYGISK_ENABLED" ]; then
  exit 0
fi

cd "$MODDIR"

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

if [ "$(which magisk)" ]; then
  for file in ../*; do
    if [ -d "$file" ] && [ -d "$file/zygisk" ] && ! [ -f "$file/disable" ]; then
      if [ -f "$file/post-fs-data.sh" ]; then
        cd "$file"
        log -p i -t "zygisk-sh" "Manually trigger post-fs-data.sh for $file"
        sh "$(realpath ./post-fs-data.sh)"
        cd "$MODDIR"
      fi
    fi
  done
fi

create_sys_perm() {
  mkdir -p $1
  chmod 555 $1
  chcon u:object_r:system_file:s0 $1
}

TMP_PATH=@WORK_DIRECTORY@

if [ -d $TMP_PATH ]; then
  rm -rf $TMP_PATH
fi

create_sys_perm $TMP_PATH

if [ -f $MODDIR/lib64/libzygisk.so ];then
  create_sys_perm $TMP_PATH/lib64
  cp $MODDIR/lib64/libzygisk.so $TMP_PATH/lib64/libzygisk.so
  chcon u:object_r:system_file:s0 $TMP_PATH/lib64/libzygisk.so
fi

if [ -f $MODDIR/lib/libzygisk.so ];then
  create_sys_perm $TMP_PATH/lib
  cp $MODDIR/lib/libzygisk.so $TMP_PATH/lib/libzygisk.so
  chcon u:object_r:system_file:s0 $TMP_PATH/lib/libzygisk.so
fi

[ "$DEBUG" = true ] && export RUST_BACKTRACE=1

if [ -f $MODDIR/bin/zygisk-ptrace64 ];then
$MODDIR/bin/zygisk-ptrace64 monitor &
elif [ -f $MODDIR/bin/zygisk-ptrace32 ];then
$MODDIR/bin/zygisk-ptrace32 monitor &
fi
