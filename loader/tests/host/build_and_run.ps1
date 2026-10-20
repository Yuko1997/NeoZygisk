<#
    Builds the host tests with the Android NDK and runs them on a connected
    device or emulator.

    The tests are plain host tests: they only need a POSIX libc and the C++
    standard library. Building them with the NDK is a convenience for
    developers on Windows and macOS, where no native toolchain is guaranteed;
    on Linux the CMake project in this directory is the native path.

    Usage: .\build_and_run.ps1 [-Ndk <path>] [-Device <serial>]
#>

param(
    [string]$Ndk = "$env:LOCALAPPDATA\Android\Sdk\ndk\27.2.12479018",
    [string]$Device = ""
)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$loader = (Resolve-Path "$here\..\..").Path
$out = Join-Path $here "build"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$toolchain = Get-ChildItem "$Ndk\toolchains\llvm\prebuilt" -Directory | Select-Object -First 1
$clang = Join-Path $toolchain.FullName "bin\x86_64-linux-android24-clang++.cmd"
if (-not (Test-Path $clang)) { throw "NDK clang++ not found at $clang" }

$tests = @{
    "mount_marker_test" = @(
        "$here\mount_marker_test.cpp",
        "$loader\src\common\mount_marker.cpp"
    )
    "mount_info_test" = @(
        "$here\mount_info_test.cpp",
        "$loader\src\common\mount_info.cpp"
    )
    "mount_plan_test" = @(
        "$here\mount_plan_test.cpp",
        "$loader\src\common\mount_info.cpp",
        "$loader\src\common\mount_marker.cpp",
        "$loader\src\common\zygote_next_mount_plan.cpp"
    )
}

foreach ($name in $tests.Keys) {
    $binary = Join-Path $out "$name.exe"
    & $clang -std=c++20 -O1 -static-libstdc++ -o $binary `
        -I $here -I "$loader\src\include" @($tests[$name])
    if ($LASTEXITCODE -ne 0) { throw "failed to build $name" }
    Write-Output "built $binary"
}

if ($Device) {
    foreach ($name in $tests.Keys) {
        $local = Join-Path $out "$name.exe"
        $remote = "/data/local/tmp/$name.exe"
        & adb -s $Device push $local $remote | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "failed to push $name" }
        & adb -s $Device shell chmod 755 $remote
        if ($LASTEXITCODE -ne 0) { throw "failed to chmod $name" }
        & adb -s $Device shell $remote
        if ($LASTEXITCODE -ne 0) { throw "$name failed" }
    }
}
