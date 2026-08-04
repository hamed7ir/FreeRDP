# armenv.ps1 -- dot-source before cmake AND ninja for the thumbv7/ARMNT cross build.
#   . <repo>\finestra-rt\port\armenv.ps1
# Sets the session %INCLUDE% / %LIB% / %PATH% that clang-cl + lld-link read at
# compile/link time (the toolchain file only covers configure-time try_compiles).
# Paths mirror thumbv7-rt.cmake exactly.
#
# Every path below is overridable by an environment variable of the same name set BEFORE
# dot-sourcing. The defaults are the STANDARD install locations of the Microsoft and LLVM
# prerequisites -- override them rather than editing this file. The VERSIONS are not
# arbitrary; see finestra-rt/README.md:
#   * MSVC 14.16 (v141) is required for lib\arm -- later toolsets dropped the ARM32 CRT.
#   * Windows SDK 10.0.19041.0 provides the arm ucrt/um import libraries.
#   * LLVM 18.1.8 is the verified clang-cl / lld-link version.

function Def($n, $f) { $v = [Environment]::GetEnvironmentVariable($n); if ($v) { $v } else { $f } }

$MsvcStlInclude = Def 'MSVC_STL_INCLUDE_DIR' 'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\include'
$MsvcArm32Lib   = Def 'MSVC_ARM32_LIB_DIR'   'C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools\VC\Tools\MSVC\14.16.27023\lib\arm'
$WinSdkRoot     = Def 'WINSDK_ROOT'          'C:\Program Files (x86)\Windows Kits\10'
$WinSdkVer      = Def 'WINSDK_VER'           '10.0.19041.0'
$LlvmRoot       = Def 'LLVM_ROOT'            'C:\Program Files\LLVM'
$NinjaDir       = Def 'NINJA_DIR'            'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'

$env:INCLUDE = @($MsvcStlInclude,
                 "$WinSdkRoot\Include\$WinSdkVer\ucrt",
                 "$WinSdkRoot\Include\$WinSdkVer\shared",
                 "$WinSdkRoot\Include\$WinSdkVer\um") -join ';'

$env:LIB     = @($MsvcArm32Lib,
                 "$WinSdkRoot\Lib\$WinSdkVer\ucrt\arm",
                 "$WinSdkRoot\Lib\$WinSdkVer\um\arm") -join ';'

# LLVM (clang-cl/lld-link/llvm-lib/llvm-rc/llvm-readobj) + Ninja on PATH.
$env:PATH = "$LlvmRoot\bin;$NinjaDir;" + $env:PATH

# Fail loudly rather than emitting megabytes of clang errors from a silent misconfiguration.
$missing = @()
foreach ($p in ($env:INCLUDE -split ';') + ($env:LIB -split ';') + @("$LlvmRoot\bin\clang-cl.exe")) {
  if (-not (Test-Path $p)) { $missing += $p; Write-Warning "[armenv] MISSING: $p" }
}
if ($missing.Count) {
  Write-Warning "[armenv] $($missing.Count) path(s) missing -- set the matching environment variable(s) and dot-source again."
} else {
  Write-Host "[armenv] INCLUDE/LIB set for thumbv7-ARMNT; LLVM+Ninja on PATH."
}
