**用 PowerShell 启动**
```powershell
$env:ANDROID_AVD_HOME = 'D:\Android\AVD'

& "$env:LOCALAPPDATA\Android\Sdk\emulator\emulator.exe" `
  -avd Pixel_Android17 `
  -no-snapshot-load
```

不冷启动时可以去掉 `-no-snapshot-load`。
关闭模拟器：

```powershell
& "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" -s emulator-5554 emu kill
```

**连接和进入系统**
```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"

# 查看连接
& $adb devices -l

# 进入 Android Shell
& $adb -s emulator-5554 shell

# 直接执行 ROOT 命令
& $adb -s emulator-5554 shell su -c id
```

正常会返回：

```text
uid=0(root) gid=0(root) groups=0(root) context=u:r:ksu:s0
```

Android Studio 的 `Logcat`、`Running Devices` 和运行目标菜单中也都能看到 `emulator-5554`。