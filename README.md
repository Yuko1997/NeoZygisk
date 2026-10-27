# NeoZygisk

NeoZygisk is a Zygisk implementation built on
[`ptrace`](https://man7.org/linux/man-pages/man2/ptrace.2.html). It provides the Zygisk module API for APatch and
KernelSU and can replace Magisk's built-in Zygisk on supported devices.

## Highlights

- Zygisk API compatibility with Magisk's implementation.
- A minimal injection path with short-lived tracing and trace cleanup after module loading.
- DenyList-aware mount isolation for applications that must not see root or module mounts.
- Dedicated Android 17 `zygote_next` handling for native isolated services.
- SELinux rules scoped to the operations required by each cleanup path.

## How It Works

NeoZygisk traces the platform zygote startup path, injects modules into application processes, and removes its
injection traces once the modules have finished loading. The same monitor also supports stub zygote chains used by some
devices and emulators.

For DenyList entries, NeoZygisk prevents the application from inheriting root and module mounts. The existing app
zygote path is unchanged. A process marked for unmounting receives a clean mount namespace before it begins running
application code.

### Android 17 Native Zygote

Android 17 introduced `/system/bin/zygote_next`, which launches native isolated services without entering the Java
zygote. NeoZygisk handles this path with a one-shot session:

1. Stop `zygote_next` at its exec event before it can fork a native isolated service.
2. Move the target into a new mount namespace and verify its namespace inode changed.
3. Use a short-lived helper to make the mount tree private and remove records matching DuckDetector's root-marker
   predicate.
4. Verify the resulting mount view independently, including anchor mounts and non-marker records.
5. Detach and confirm `TracerPid` returns to zero.

If cleanup cannot complete safely, NeoZygisk restores the original namespace or terminates and retries the target.
An unavailable probe must not be treated as a clean result.

## DenyList

Use the root manager's own application setting:

- **APatch/KernelSU:** enable **Umount modules** for the target application.
- **Magisk:** add the application to **Configure DenyList**.

Do not enable Magisk's **Enforce DenyList** option for this module. That option enables Magisk's separate DenyList
implementation, may conflict with NeoZygisk, and does not guarantee the same mount visibility.

## Requirements

- Android 8.0 or newer.
- APatch, KernelSU, or Magisk at a version accepted by the module installer.
- A supported ABI: `arm64-v8a`, `armeabi-v7a`, `x86_64`, or `x86`.

Install the packaged ZIP from the root manager. Recovery-only installation is not supported.

## WebUI

KernelSU and APatch can open the module WebUI directly from the module page. It shows the
current injector and Android 17 native zygote status, active Zygisk modules, and device details.
Status is read from the monitor-owned `/data/adb/neozygisk/status` file; module metadata and
daemon display text remain in the human-readable `module.prop` view.

## Build

The project uses the Gradle wrapper and the Android SDK/NDK versions declared in the root build files.

```powershell
.\gradlew.bat :module:zipRelease
.\gradlew.bat :module:zipDebug
```

The module ZIPs are written to `module/build/outputs/release/`. Release builds disable ptrace file logging and
zygote_next fault injection. Debug builds enable both.

## Tests

Host tests cover mountinfo parsing, the DuckDetector marker predicate, mount subtree planning, propagation checks, and
snapshot preservation. On Windows they can be built with the Android NDK and optionally run on a connected device:

```powershell
.\loader\tests\host\build_and_run.ps1
.\loader\tests\host\build_and_run.ps1 -Device emulator-5554
```

On Linux, the same tests can be built directly with CMake:

```bash
cmake -S loader/tests/host -B build/host-tests
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

The Android 17 mount cleanup design and verification requirements are documented in
[docs/zygote-next-cleanup.md](docs/zygote-next-cleanup.md). Emulator startup instructions are in
[docs/Vmdevice.md](docs/Vmdevice.md).

## Project Layout

- `loader/`: C++ injector, ptrace monitor, mount planning, and Android 17 native zygote cleanup.
- `zygiskd/`: Rust daemon for module loading, process flags, and namespace management.
- `module/`: Flashable module scripts, update metadata, and release packaging.
- `loader/tests/host/`: Host-side differential and unit tests.

## License

NeoZygisk is licensed under the terms in [LICENSE](LICENSE).
