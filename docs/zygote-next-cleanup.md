# zygote_next 挂载清理改进

## 结论

本文只讨论 Android 17 的 `/system/bin/zygote_next` 清理，不改变 Java zygote、DenyList、已授权 root 以及缓存命名空间的现有路径。

App Profile DenyList 继续由 root manager 的 UID 查询决定：Java app 进程通过
`uid_should_umount` 得到 `non_root_profile.umount_modules`/`allow_su` 的结果。`zygote_next`
承载的是 isolated native service，KernelSU 对它本身始终执行 unmount，因此这里保留父进程的
一次性清理；清理标记额外记录 PID 和 `/proc/<pid>/stat` 的 starttime，避免同一 boot 内
`zygote_next` 重启后被旧标记误判为已处理。

WebUI 的权威状态单独写入 `/data/adb/neozygisk/status`，不解析 `module.prop` 中由
`DAEMON_SET_INFO` 拼接的模块名和 daemon 展示文本。这样即使模块名包含 `monitor:` 或
`zygote_next:` 等前缀，也只能作为展示内容出现，不能覆盖监控器写入的状态。

检测基准固定为 DuckDetector 提交
[`d3a085e3eaedeb160eda0566e43c3535675a190b`](https://github.com/eltavine/Duck-Detector-Refactoring/tree/d3a085e3eaedeb160eda0566e43c3535675a190b)，
即工作区中的 `Duck Detector-2026.10.19-d3a085e3eaed.apk` 对应的源码。后续讨论不再使用“Magisk 通常会怎样”“overlay 通常会怎样”之类推测作为验收规则。

源码结论如下：

1. `Zygote next mount view` 卡片的直接危险信号，只来自 Android 17 隔离 native 服务读到的
   `/proc/self/mountinfo` 中的结构化 root marker。
2. 隔离服务里任意一条非 `debug_ramdisk` marker，都会产生一次
   `ROOT_MOUNT_EXPOSURE`，卡片判为 `DANGER`。
3. 挂载传播方式、命名空间是否私密、mount ID 新旧和 anchor 数量只用于解释当前视图像不像
   AOSP 启动布局，不产生 `DANGER`。`private` root 也只是 informational coverage observation。
4. 当前实现按路径名和 root 方案猜卸载目标，与检测器谓词既不充分也不必要；同时保留长期
   `ptrace`、按 fork 重复扫描、失败后继续放行，代价和风险都高于一次性清理。
5. 正确方向不是“多卸几条”，而是：在 `zygote_next` 第一次运行前，只创建一个自包含 mount
   namespace，按检测器同构谓词证明 marker 已清空，保留所有非 marker 挂载，然后立即 detach。

## 检测器实际测什么

### 调用链

1. SDK 37 及以上，`ZygoteNextProbeManager` 使用
   [`bindIsolatedService()`](https://github.com/eltavine/Duck-Detector-Refactoring/blob/d3a085e3eaedeb160eda0566e43c3535675a190b/feature/mount/data/src/main/kotlin/com/eltavine/duckdetector/features/mount/data/zygotenext/ZygoteNextProbeManager.kt#L44-L51)
   绑定一个隔离服务。
2. [`AndroidManifest.xml`](https://github.com/eltavine/Duck-Detector-Refactoring/blob/d3a085e3eaedeb160eda0566e43c3535675a190b/feature/mount/data/src/main/AndroidManifest.xml#L23-L36)
   同时声明 `isolatedProcess="true"` 和 `nativeService="true"`，并指定加载
   `libmain.so` 的 `ANativeService_onCreate`。
3. Android 17 由 `zygote_next` 启动该 native service；服务没有进入 ART。
4. [`native_service.cpp`](https://github.com/eltavine/Duck-Detector-Refactoring/blob/d3a085e3eaedeb160eda0566e43c3535675a190b/feature/mount/data/src/main/cpp/mount/zygotenext/native_service.cpp#L135-L209)
   在服务自己的进程里读取 `/proc/self/mountinfo`，把结构和 marker 经 Binder 返回。
5. Kotlin 只把 `isolatedProcess.markers` 送入 ZygoteNext 报告。应用主进程里的同名 marker
   不参与这条 ZygoteNext 判定。
6. [`MountZygoteNextReport.leakDetected`](https://github.com/eltavine/Duck-Detector-Refactoring/blob/d3a085e3eaedeb160eda0566e43c3535675a190b/feature/mount/domain/src/main/kotlin/com/eltavine/duckdetector/features/mount/domain/MountZygoteNextReport.kt#L79-L90)
   只要 state 为 `READY` 且存在一条 `dangerous` marker，就判定 `ROOT_MOUNT_EXPOSURE`。
7. [`MountFindingBuilders.kt`](https://github.com/eltavine/Duck-Detector-Refactoring/blob/d3a085e3eaedeb160eda0566e43c3535675a190b/feature/mount/data/src/main/kotlin/com/eltavine/duckdetector/features/mount/data/repository/MountFindingBuilders.kt#L103-L137)
   将该 exposure 转成一个 `DANGER` finding；`MountMethods.kt` 同时把方法行判为 `DANGER`。

因此，本项目真正需要处理的时刻是：`zygote_next` 的 mount namespace 被后续 native isolated
服务继承之前。清理早于这个时刻才有意义；晚于 isolated service 启动再修已经来不及。

服务绑定失败、payload 解析失败或 mountinfo 不可读时，检测器显示 `UNAVAILABLE`/`SUPPORT`，
这不是 `CLEAN`。本项目不能用“阻止该 native service 启动”的方式规避 `ROOT_MOUNT_EXPOSURE`；
验收必须包含一次成功的 isolated service 快照。

### 精确 marker 谓词

检测器的唯一实现是
[`mount_artifact_markers.h`](https://github.com/eltavine/Duck-Detector-Refactoring/blob/d3a085e3eaedeb160eda0566e43c3535675a190b/core/native/src/main/cpp/common/mount_artifact_markers.h#L10-L41)。
它只检查 `mountinfo` 的 `root`、`target`、`source` 三个结构化字段。

一条记录产生危险 marker，当且仅当满足下面任一条件：

- `source` 转为小写后，完整等于 `magisk`、`ksu`、`kernelsu`、`zygisk`、`apatch`
  或 `magic_mount`。
- `root`、`target`、原始大小写的 `source` 中任一字段，恰好等于以下目录或位于其子路径：
  - `/data/adb`
  - `/adb/modules`
  - `/.magisk`
  - `/sbin/.magisk`
  - `/dev/.magisk`
  - `/debug_ramdisk/.magisk`
  - `/.magic_mount`
- 命中 `/debug_ramdisk` 本身只增加 `debug_ramdisk` 标签；只有该标签不是危险信号，混入其它标签后
  才会随整条记录变成危险。

路径匹配必须按目录边界判断。`/data/adb_backup` 不匹配 `/data/adb`，
`/adb/modules_backup` 不匹配 `/adb/modules`。

检测器不检查：

- `fs_options` 中的 `lowerdir`、`upperdir`、`workdir`；
- 文件系统类型是否为 `overlay`、`ext4` 或 `f2fs`；
- source 是否为 `/dev/block/loop*`；
- mount ID、设备号、parent ID；
- `/data/adb` 以外的 `/data/adb/modules` 专用特例。

所以直接把“overlay 选项命中”或“KSU loop source”作为 ZygoteNext 的硬门槛，会扩大清理范围，却
无法弥补真正漏掉的 `root`/`target`/`source` 标记。当前 native 实现已经在 root-source 和 KSU loop
推断上扩大了范围，同时仍有结构化路径漏删；overlay option 规则也不应再被加入这层谓词。

### namespace 结论不是 DANGER

[`MountZygoteNextReport.namespaceAssessment`](https://github.com/eltavine/Duck-Detector-Refactoring/blob/d3a085e3eaedeb160eda0566e43c3535675a190b/feature/mount/domain/src/main/kotlin/com/eltavine/duckdetector/features/mount/domain/MountZygoteNextReport.kt#L92-L221)
会观察以下内容：

- 两个 namespace inode 是否不同；
- native isolated 进程的 UID 是否在 `90000..99999`；
- `/` 的 `shared:` / `master:` 传播组合；
- root、minimum mount ID 和至少三个公共 anchor mount ID 的新旧关系；
- 双方 mountinfo 记录数是否至少为 8。

只有该视图像一个 AOSP init-managed namespace 时，方法行才显示 `CLEAN`；否则显示
`Coverage unverified` 或 `Namespace observation`，outcome 为 `SUPPORT`。这些结果都不是危险。

工程上仍建议尽量维持原生布局，避免不必要的 `PRIVATE_ANOMALY`；但不能为了伪造 `LIKELY_INIT`
而保留挂载传播，也不能只为了 mount ID 排序去重挂载或篡改 namespace 拓扑。

### `TracerPid` 属于另一条检测链

DuckDetector 的 Zygisk 卡片单独读取应用进程
[`/proc/self/status` 的 `TracerPid`](https://github.com/eltavine/Duck-Detector-Refactoring/blob/d3a085e3eaedeb160eda0566e43c3535675a190b/feature/zygisk/data/src/main/cpp/zygisk/probes/thread_probe.cpp#L27-L52)。
非零即产生 `TracerPid` 强信号；[`ZygiskRepository.kt`](https://github.com/eltavine/Duck-Detector-Refactoring/blob/d3a085e3eaedeb160eda0566e43c3535675a190b/feature/zygisk/data/src/main/kotlin/com/eltavine/duckdetector/features/zygisk/data/repository/ZygiskRepository.kt#L200-L213)
把该信号判为 `DETECTED`。

这不是 `Zygote next mount view` 的判定条件，但仍是整个 DuckDetector 基线中的独立危险信号。因此：

- 长期 trace `zygote_next` 不应保留；
- 清理完成后必须 detach，并让后续 native 应用进程的 `TracerPid` 回落为 0；
- 不能把“mount marker 已清掉”当作“允许永久附着 tracer”的理由。

## 当前实现的具体问题

以下位置均指当前工作树，而不是 `HEAD` 中的旧实现。

### 1. marker 识别与检测器不一致

[`collect_native_umount_targets()`](loader/src/ptracer/ptracer.cpp#L42-L64) 现在使用：

- 无边界前缀 `/adb/modules`；
- 无边界前缀 `/data/adb`；
- source 完整等于 `APatch`、`KSU` 或 `magisk`；
- 额外推断 KSU loop source。

这会同时漏删和误删：

- 漏删 `source == "KernelSU"`、`"Zygisk"`、`"magic_mount"` 或大小写变体；
- 漏删 `root`/`target`/`source` 位于 `/.magisk`、`/sbin/.magisk`、`/dev/.magisk`、
  `/debug_ramdisk/.magisk`、`/.magic_mount`；
- 漏删 `/system` 等目标上、但由 `/adb/modules/...` 作为 `root` 盖上的模块挂载；
- 误把 `/data/adb_backup`、`/adb/modules_backup` 当作 marker；
- 仅因 source 是 KSU loop 就卸载，检测器本身不会仅凭 loop source 报警；
- 对三个 root 方案使用混合规则，没有保证与当前 root 方案的实际 mount 形状一致。

`mount_info` 已包含 `root`、`target`、`source`，应直接实现与检测器相同的谓词，不需要改内核接口，
也不需要读取敏感 option。

### 2. 关键目标被主动跳过

[`is_critical_mount_point()`](loader/src/ptracer/ptracer.cpp#L66-L77) 会跳过
`/`、`/system`、`/vendor`、`/product`、`/system_ext`、`/apex`、`/data`、`/dev`、`/proc`、`/sys`
上恰好同名的整个 mount record。

检测器的 marker 不关心 target 名称。例如 `target=/system`、`root=/adb/modules/foo/system` 必须卸载；
`target=/product`、`source=magisk` 同样是危险记录。按“目标看起来重要”跳过，属于直接漏检。

[`abort_native_unmount()`](loader/src/ptracer/ptracer.cpp#L119-L129) 还会因 `/product` 下的候选整单
放弃，随后 [`clean_native_zygote_namespace()`](loader/src/ptracer/ptracer.cpp#L242-L254) 把它当成功。
这与本任务的前提冲突：native 路径没有第二套干净命名空间可以继续回退。

正确保护对象是“非 marker 的真实底层挂载”，不是“路径名属于系统分区”。

### 3. “没有计划”没有区分“干净”和“解析失败”

[`clean_native_zygote_namespace()`](loader/src/ptracer/ptracer.cpp#L242-L254) 在
`plan.targets.empty()` 时直接返回成功。这个空集合可能来自：

- 确实没有 marker；
- `mountinfo` 打开失败；
- 所有行解析失败；
- 候选因关键路径、缺少 parent 或 `/product` 保护被跳过；
- 读取过程中发生截断。

必须把扫描结果建模为 `CLEAN`、`FAILED` 和 `NOT_ATTEMPTED`，不能用一个空数组表示成功。

### 4. `unshare` 成功没有独立证明

[`unshare_native_zygote_mount_namespace()`](loader/src/ptracer/ptracer.cpp#L131-L153) 只检查远程
`unshare(CLONE_NEWNS)` 的返回值：

```text
called == true && result == 0
```

这不足以证明目标进入了新 namespace：

- 没有在调用前后比较目标 `/proc/<pid>/ns/mnt` 的 `dev:ino`；
- `set_regs()` 的返回值被忽略，寄存器恢复失败仍继续；
- remote syscall 失败后，上层仍然 `PTRACE_CONT`；
- 目标可能退出、换 namespace 或 syscall 停点异常，当前代码没有闭环验证。

应把“syscall 返回 0”和“namespace inode 已改变”同时作为前置条件。任一失败都禁止进入卸载。

### 5. 先清理、后切断传播

[`unmount_traces_in_current_namespace()`](loader/src/ptracer/ptracer.cpp#L162-L204) 只在 selected
parent 带 `shared:` 时对 parent 执行非递归 `MS_SLAVE`，或者对 `/` 执行非递归 `MS_PRIVATE`，然后
再把 `/` 恢复成 `MS_SHARED`。

风险是：

- 父挂载和祖先仍可能接收或传播事件；
- 在 unshare 后、执行 private 前存在传播窗口；
- 非递归操作没有覆盖整棵挂载树；
- 恢复共享可能创建新的 peer group，使视图状态与清理前并不等价；
- 如果 marker 漏选，传播根本没有切断，后续仍可能被重新填充。

应在确认 unshare 后，先对 `/` 执行一次 `MS_REC | MS_PRIVATE`，再读取候选快照并挂载清理。
如果需要维持检测器的“stock-like” root 形态，可在所有 marker 消失后只对 `/` 执行一次
`MS_SHARED`，让它是自包含的新 peer group，绝不能恢复与 init 的 slave/master 关系。

### 6. 在 tracer 自身执行 `setns`

[`unmount_native_zygote_traces()`](loader/src/ptracer/ptracer.cpp#L206-L240) 让长期存在的 tracer
线程进入目标 namespace，执行卸载后再尝试回到原 namespace。恢复失败时，tracer 后续所有操作都发生
在错误视图里。

应改为短命 helper：

- tracer 保持原 namespace；
- helper `setns(target_ns_fd, CLONE_NEWNS)`；
- helper 完成 propagation 修改、卸载和自检，只通过 pipe/exit status 回报；
- helper 退出后 namespace 引用自然收敛，不需要恢复 tracer 的 namespace。

### 7. 卸载成功的定义不成立

当前将 `umount2(..., MNT_DETACH)` 返回 0 当作该条记录已清除，并把 `ENOENT`/`EINVAL` 计为 skipped
后返回成功。没有重新读取同一 mount namespace 的 `mountinfo` 验证结果。

`MNT_DETACH` 本身不是错误：它适合移除带子挂载的 module subtree，也能避免为了普通 `umount2`
在 busy subtree 上做大量脆弱工作。问题在于不能把系统调用返回值当作最终证据。

建议规则：

- 叶子挂载优先普通 `umount2(path, 0)`；
- 有后代、目标路径被多层覆盖或普通卸载 `EBUSY` 时，允许一次 `MNT_DETACH`；
- 每次操作后重新读取同一个 namespace 的 `mountinfo`；
- 只有目标 mount ID 以及计划移除的后代都已消失，该步才算成功；
- `ENOENT`/`EINVAL` 只有在校验阶段证明目标已不存在时才能接受。

### 8. 按 mount ID 降序不等于按树深度卸载

mount ID 是分配标识，不是稳定的父子顺序。检测器自己的证据记录也明确把 ID 连续性视为
informational，不作为安全契约。

清理计划应以 `parent` 建树：

- 找出所有 marker record；
- 把 marker 及其后代归入待移除 subtree；
- 普通卸载按 depth 从深到浅；
- `MNT_DETACH` 作用于 subtree 根后再复查整个 subtree；
- 对同一 target 有多层覆盖时，逐次重读，不能假设一次路径卸载对应哪一条 marker。

### 9. 失败路径仍然放行

[`native_zygote_session()`](loader/src/ptracer/ptracer.cpp#L272-L366) 在准备失败时只打印 warning，
随后仍 `PTRACE_CONT`。这等于把已知可能未清理的 namespace 交给后续 native isolated service。

[`handle_trace()`](loader/src/ptracer/main.cpp#L165-L188) 在 native trace 失败时还发送 `SIGCONT`；
[`monitor_impl.cpp`](loader/src/ptracer/monitor_impl.cpp#L603-L624) 在 exec/fork 失败时也按 native
路径发送 `SIGCONT`。这些分支都必须删除。

### 10. 长期 fork 监控没有必要，而且是负优化

[`native_zygote_session()`](loader/src/ptracer/ptracer.cpp#L272-L366) 使用
`PTRACE_O_TRACEFORK|TRACEVFORK|TRACECLONE`，对每个 child 再扫描父 namespace 并 detach。这带来：

- zygote 生命周期内持续可见的 `TracerPid`；
- 每个 child 至少一次 wait、一次 mountinfo 扫描和一次 ptrace 操作；
- fork 停止点与第二次清理之间的传播窗口；
- tracer 与 monitor 之间的额外生命周期和信号复杂度。

一旦 private namespace 成功建立，后续 child 继承或复制的是这棵已隔离的树。持续监控无法弥补 private
转换失败；转换失败应该在启动前直接失败。

## 目标实现

### 状态机

建议把 native 清理实现为一次性 session，并在任何挂载修改前先完成所有可失败的安全检查。

```text
MONITOR_STOPPED_TARGET
  -> SEIZE_ONLY_TARGET
  -> VERIFY_TARGET_STILL_STOPPED
  -> SNAPSHOT_PRE_NAMESPACE
  -> REMOTE_UNSHARE
  -> VERIFY_REGS_RESTORED
  -> VERIFY_MOUNT_NS_CHANGED
  -> OPEN_VERIFIED_TARGET_NS_FD
  -> SPAWN_CLEANUP_HELPER
       -> SETNS_TARGET_NS
       -> MS_REC|MS_PRIVATE /
       -> READ + PLAN
       -> UNMOUNT + RE-READ
       -> FINAL_PREDICATE
       -> OPTIONAL SELF-CONTAINED MS_SHARED /
       -> FINAL_FULL_SNAPSHOT
  -> INDEPENDENT_PARENT_VERIFY
  -> PTRACE_DETACH
  -> VERIFY_TRACER_PID_ZERO
  -> EXIT
```

### 1. 只在已经停止的目标上 seize

- monitor 开始跟踪 init 后，如果发现设备已经提前启动了 `/system/bin/zygote_next`，
  必须终止这个未被追踪的旧实例，让 init 在当前 ptrace 关系下重新 fork；否则 OEM 启动时序
  会导致 monitor 永远收不到它的 exec 事件；
- monitor 仍按现有方式把目标停在 `SIGSTOP`；
- tracer 只 `PTRACE_SEIZE` 目标线程，不注册 fork/vfork/clone 事件；
- 初始状态必须是目标停止，不能先 `PTRACE_CONT` 再 `PTRACE_INTERRUPT` 补抓；
- tracer 进程在 `exec` 前设置 `PR_SET_PDEATHSIG`，并复查父进程仍是 monitor；
- 在危险窗口内使用 `PTRACE_O_EXITKILL`，使 tracer 崩溃时目标被终止而不是带着未知 namespace 继续运行。

`PTRACE_O_EXITKILL` 会让失败更硬，但它符合“不能带脏 namespace 放行”的优先级。若产品不能接受目标被杀，
就必须用等价的独立 watchdog 保证目标始终不能恢复执行；不能只依赖一次 `SIGSTOP`。

### 2. unshare 必须有前后证据

在 remote syscall 前后分别对目标 `/proc/<pid>/ns/mnt` 做 `fstat()`：

```text
before_ns = open + fstat(/proc/<pid>/ns/mnt)
remote_syscall(pid, __NR_unshare, CLONE_NEWNS)
restore_regs_or_fail(pid)
after_ns  = open + fstat(/proc/<pid>/ns/mnt)
```

进入下一步的必要条件：

- remote syscall 在预期 syscall-exit 停点结束；
- 返回值是 0；
- 所有通用寄存器恢复成功；
- `before_ns.dev:ino != after_ns.dev:ino`；
- 目标仍存活且仍处于 tracer 控制下。

不满足时不得读取候选、不得卸载、不得 detach 后放行。

验证后重新打开一段时间内保持有效的目标 namespace fd，并把该 fd 交给 helper。不要只传 PID 让
helper 二次解析 `/proc/<pid>/ns/mnt`，否则目标退出或切换 namespace 后可能清理到另一棵树。

### 3. 先切断传播，再枚举

短命 helper 进入目标 namespace 后第一步执行：

```cpp
mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
```

这一步必须成功并重新读取 mountinfo 验证：整个树不存在仍与外部 peer group 相连的共享传播。
之后才允许枚举和卸载。这样即使 init 或 root daemon 在清理期间新增 mount，也不会传播进目标树。

如果需要让 DuckDetector 的方法行接近 `CLEAN` 而不是 `Coverage unverified`，可在所有 marker 消失、
最终复查仍为干净后执行一次非递归的：

```cpp
mount(nullptr, "/", nullptr, MS_SHARED, nullptr);
```

这只创建自包含 peer group，不重新建立到 init 的 master/slave 关系。若该操作失败，应保持 private，
记录 `PRIVATE_ANOMALY` 覆盖状态，但不能因此恢复为可传播状态。

### 4. 在 helper 内只按检测器谓词选目标

建议抽成一个纯函数，输入已解析的 `mount_info`，输出 marker 和待移除 subtree：

```text
is_zygote_next_marker(root, target, source)
  -> source_exact_casefold in {
       magisk, ksu, kernelsu, zygisk, apatch, magic_mount
     }
  OR any path in {root, target, source_original}
       is_equal_or_child_of {
         /data/adb,
         /adb/modules,
         /.magisk,
         /sbin/.magisk,
         /dev/.magisk,
         /debug_ramdisk/.magisk,
         /.magic_mount
       }
```

`debug_ramdisk` 单独出现时只记为 contextual，不进入待移除集合。

不要在这个谓词里加入：

- target 路径白名单；
- `/product` 整单 abort；
- 当前 root 方案的额外 loop source；
- overlay option 通配；
- “看起来像关键分区”的豁免。

如果以后需要防御其它 DuckDetector Mount 子检测路径，可以把附加规则做成独立、次级、可关闭的
cleanup profile；ZygoteNext 的发布门禁必须首先与当前提交的谓词一致。

### 5. 生成 subtree，而不是单条路径列表

计划阶段需要：

1. 解析所有 record 的 `id`、`parent`、`root`、`target`；
2. 标记直接 marker；
3. 沿 `parent` 关系把 marker 的后代纳入该 subtree；
4. 记录每个 subtree 的根、深度和原始 record 集合；
5. 计算预期删除集合，保留所有不属于任一 subtree 的 record。

普通卸载按 depth 降序；如果 subtree 过深或普通卸载遇到 `EBUSY`，对 subtree 根使用
`MNT_DETACH`。两种方式都必须重读 mountinfo，确认预期 subtree 已从目标 namespace 消失。

不要在第一步就删除 marker 数组里的尾部元素。当前 `remove_if` 式实现会让失败项留在列表中，
但成功和失败都缺少事后状态证明，最终无法区分“删了”“被覆盖”和“路径指向了另一层”。

如果某个 marker 必须通过先卸载非 marker 的上层覆盖才能触及，而该上层不在已批准的 marker subtree
中，应直接失败，不能为了“清干净”顺手删除无关系统挂载。

### 6. 最终 release invariant

只有 tracer 独立读取的最终目标视图同时满足以下条件，才允许 detach：

1. 目标 mount namespace 与进入前相比已经改变，且不等于 init namespace。
2. `/` 不存在仍在接收外部传播的 `master:` 关系；若执行了可选 reshare，只能是 self-contained
   `shared:` peer group，不能同时出现 `shared:` 和 `master:`。
3. 按检测器谓词扫描，目标 namespace 的危险 marker 数为 0。
4. 原快照中的全部非 marker 根挂载仍存在；因 marker subtree 合理消失的后代必须能由 parent 关系解释。
5. DuckDetector 使用的八个 anchor 仍有可用记录：
   `/`、`/dev`、`/proc`、`/sys`、`/data`、`/system`、`/vendor`、`/product`。
6. mountinfo 能完整读到 EOF，没有截断、解析失败或空快照。
7. 寄存器已恢复；detach 后目标进程重新可运行，`TracerPid` 为 0，tracer 进程退出。

第 4 条建议比较不可变字段集合：

```text
id | parent | device | root | target | vfs_options | type | source | fs_options
```

预期差异只能是：

- 被删除 subtree 中的 record；
- 如果执行了 reshare，`/` 的 propagation options 按预期变化。

任何其它 record 消失、id 重排、anchor 换 source、`/system` 露出错误底层或 mount 数异常下降，
都按失败处理。

检测器的方法行显示 `Coverage unverified` 本身不等于 mount exposure；不能为了强制显示 `CLEAN`
而保留 marker、伪造 mount ID 或改变系统挂载。`CLEAN` 只应作为保留 AOSP 布局后的自然结果。

## 建议改动顺序

### 第一步：先做谓词与解析器

- 在 [`mount_info.hpp`](loader/src/include/mount_info.hpp) 增加明确的扫描状态，而不是用空 vector
  表示成功；
- 在 [`mount_info.cpp`](loader/src/common/mount_info.cpp) 增加完整读取、EOF 检查和目录边界匹配；
- 新增 `is_zygote_next_mount_marker(root, target, source)`；
- 用 DuckDetector 的 fixtures 写宿主测试，先证明谓词完全一致，再改执行路径。

验证条件：所有 marker 正反例通过，且编译器不再需要在 `ptracer.cpp` 中重复路径逻辑。

### 第二步：拆出一次性 native session

- 把 [`native_zygote_session()`](loader/src/ptracer/ptracer.cpp#L272-L366) 缩成只清理一次的短 session；
- 删除 `PTRACE_O_TRACEFORK|TRACEVFORK|TRACECLONE` 和 `waitpid(-1)` 循环；
- unshare 后验证 namespace inode、寄存器恢复和目标存活；
- 准备失败时不执行 `PTRACE_CONT`。

验证条件：目标要么进入已验证的新 namespace，要么保持停止/被终止；不会出现第三种“继续运行但结果未知”。

### 第三步：增加短命 cleanup helper

- 从 tracer 传入已验证的 namespace fd；
- helper 先执行 `MS_REC | MS_PRIVATE`，再计划和卸载；
- tracer 不改变自己的 namespace；
- helper 通过 pipe 返回结构化错误码，父进程仍独立读取 mountinfo 复查。

验证条件：故意让 helper 在每一步崩溃，init 和其它 namespace 的 mountinfo 都不变。

### 第四步：用 subtree 与复查替换路径式卸载

- 按 parent 关系生成 marker subtree；
- 删除 critical target 和 `/product` 整单放弃分支；
- `umount2` 只作为动作，最终成功只看 mount ID 消失和最终谓词；
- 比较清理前后的非 marker record，拒绝任何无法解释的删除。

验证条件：`target=/system`、`target=/product`、多层 target、marker 带后代等 fixture 均清零，
且真实底层 anchor mount 保持不变。

### 第五步：收紧失败路径并跑检测器

- 删除 [`handle_trace()`](loader/src/ptracer/main.cpp#L165-L188) 和
  [`monitor_impl.cpp`](loader/src/ptracer/monitor_impl.cpp#L603-L624) 中 native 路径的 `SIGCONT`；
- 把 native tracer 的非零退出和超时纳入 monitor 状态；
- 用固定提交的 DuckDetector 在 Android 17 上跑完整验收矩阵。

验证条件：任意注入故障只会留下停止或终止的 `zygote_next`，DuckDetector 不会读到
`ROOT_MOUNT_EXPOSURE`，Java zygote 路径不受影响。

## 失败策略

失败必须按“是否已经修改 namespace”分类：

### unshare 或寄存器恢复失败

- 目标 namespace 可能根本没有改变；
- 禁止进入 helper 和卸载；
- 不要对 `zygote_next` 的当前 namespace 补刀；
- 寄存器可安全恢复时，detach 后让目标继续运行在原始 namespace；
- 寄存器状态不可信时终止目标，不能留下长期 `T` 状态。

### unshare 成功，但 propagation、扫描或卸载失败

- 所有修改只发生在目标的新 namespace 内；
- init 和其它进程不应受影响；
- 目标仍可能有 marker 或不完整 mount 树；
- 在 unshare 前通过目标自身的 fd 保存原始 namespace；
- 失败后先在目标内执行 `setns(original_fd, CLONE_NEWNS)` 并验证 inode 已恢复；
- 回滚成功后 detach，让目标继续运行在原始 namespace，并把状态标记为
  `cleanup failed; namespace restored`；
- 回滚失败时终止目标并重试一次；连续两次无法回滚才进入明确的 fallback 模式；
- 不得把目标无限留在 ptrace/job-control stop，否则所有 native isolated service 都会超时。

### 清理成功，但 detach 或 `TracerPid` 验证失败

- 不得假定进程安全；
- 尝试重新 seize/停止；
- 若无法恢复确定状态，终止目标比继续运行安全；
- monitor 必须把 native tracer 失败从“仅日志”提升为可观测的启动失败状态。

`Unavailable`、binding timeout 和 fallback 运行都不能作为 `CLEAN`。它们必须显示为
coverage failure 或真实 mount exposure，且不能通过发布门禁。

不能沿用当前的三个行为：

- `prepare_native_zygote_namespace()` 失败后继续 `PTRACE_CONT`；
- native tracer 失败后 `kill(pid, SIGCONT)`；
- `/product` 保护命中后把未清理 namespace 当成功。

## 性能与稳定性

一次性方案的目标成本：

- `unshare`：1 次；
- recursive private：1 次；
- `setns`：1 次短命 helper；
- mountinfo 全量读取：基线、卸载后、最终各 1 次；
- 普通卸载：仅 marker subtree；
- 可选 root reshare：1 次；
- detach：1 次。

相比当前实现，这消灭了每个 fork 的 wait、mountinfo 扫描和 ptrace 唤醒，也消除了长期 `TracerPid`。
清理成本随 mount 数量线性增长，但只发生一次。

稳定性边界：

- 所有卸载只在 unshare 后的私有 namespace 中进行；
- `MS_REC | MS_PRIVATE` 必须在枚举前完成；
- 不修改真实底层 `/system`、`/vendor`、`/product` mount；
- `MNT_DETACH` 只用于 marker subtree 或 busy fallback，且必须经 mountinfo 复查；
- 不输出完整 mountinfo 或模块路径到日志；
- 失败时不能伪装为 Clean；优先回滚原始 namespace，无法回滚时进入可观测的 fallback。

## 验收

### 单元与宿主测试

至少覆盖以下 case：

- `source` 的六种完整值及大小写变体；
- 七个路径前缀的 exact match 与 descendant match；
- `/data/adb_backup`、`/adb/modules_backup` 不匹配；
- `debug_ramdisk` 单独存在时为 contextual；
- `debug_ramdisk/.magisk` 同时产生危险 Magisk marker；
- `root=/adb/modules/foo/system, target=/system`；
- `source=magisk, target=/product`；
- `root=/data/adb/...` 或 `source=/data/adb/...`；
- overlay options 有可疑字符串但结构化字段干净时不触发 ZygoteNext marker；
- 同一 target 的多层挂载；
- marker 带非 marker 后代、marker 被非 marker 父覆盖；
- mountinfo 打开失败、短读、坏行、EOF 前错误；
- unshare 前后 namespace inode 相同、`set_regs` 失败、helper `setns` 失败；
- 普通 unmount 失败、`MNT_DETACH` 后仍可见；
- 清理后非 marker record 少一条或 anchor 换 source。

测试谓词应直接引用与 DuckDetector `mount_artifact_markers.h` 相同的 fixture 表。若上游检测器规则变化，
更新 fixture 和本文版本号，而不是继续沿用旧宏。

### Android 17 设备

使用与源码版本一致的 DuckDetector `d3a085e3eaed`，在 Android 17 上至少验证：

- Magisk、KernelSU、APatch 各一套带 module overlay 的环境；
- 普通 native isolated service 能启动；
- `Zygote next mount view` 不出现 `ROOT_MOUNT_EXPOSURE`；
- Mount 报告的 isolated markers 为空；
- 如果执行安全 reshare，namespace assessment 自然落在 `LIKELY_INIT`；否则允许
  `PRIVATE_ANOMALY` 作为 informational，但不能有 root marker；
- Zygisk 卡片的 `TracerPid` 为 0；
- `/system`、`/vendor`、`/product`、`/apex`、`/data` 的真实底层 mount 在清理后仍可访问；
- zygote_next 后续 fork 出的进程看不到 init 中后来新增的 module bind；
- Java zygote、DenyList、已授权 root 的现有行为没有变化。

### 故障注入

故意制造以下故障，并检查 init 的 mountinfo 完全不变：

- gadget 扫描失败；
- remote unshare 返回非 0；
- namespace inode 未变化；
- 寄存器恢复失败；
- helper `setns` 失败；
- `MS_REC | MS_PRIVATE` 失败；
- marker subtree 卸载后仍可见；
- 最终快照出现非预期删除；
- tracer 在 detach 前被信号终止。

每个故障的最终状态只能是：

- `zygote_next` 保持停止；或
- `zygote_next` 被明确终止；

不能出现“带着 marker 继续执行”，也不能出现 init 或其它 root 进程的挂载被误删。

## 明确不采用

- 不保留长期 fork 监控。
- 不按 `/product`、`/system` 等 target 名称跳过 marker。
- 不把 KSU loop source 或 overlay option 当作 ZygoteNext 的唯一判定。
- 不在 unshare 未证实成功时清理当前 namespace。
- 不在 tracer 主线程反复 `setns`。
- 不把 `umount2` 返回值当作完成证据。
- 不用持续 ptrace 弥补失败的 propagation 隔离。
- 不为了 `LIKELY_INIT` 标签保留真实传播关系或伪造 mount ID。
- 不把 `Coverage unverified` 与 `ROOT_MOUNT_EXPOSURE` 混为一谈。
- 不扩大 `app_zygote` 的 `proc`/`nsfs` SELinux 权限来服务 `zygote_next`。
