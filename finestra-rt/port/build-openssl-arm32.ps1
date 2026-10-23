# build-openssl-arm32.ps1 -- OpenSSL 3.5.x static libraries for ARM32 / Windows RT 8.1 (ARMNT).
#
#   powershell -File build-openssl-arm32.ps1 -Source <openssl-3.5.9 source dir> -Build <empty dir> -Out <empty dir>
#
# Produces <Out>\lib\libcrypto.lib, <Out>\lib\libssl.lib and <Out>\include\openssl\*.h (with the GENERATED
# configuration.h for this target). libcrypto.lib is self-contained: the default provider is inside it,
# so no separate provider archive is needed.
#
# How: OpenSSL's own VC-WIN32-ARM target, driven by clang-cl + llvm-lib instead of MSVC's ARM compiler,
# with the same compile flags the engine's toolchain (thumbv7-rt.cmake) uses and the same option set as
# the x64 engine's OpenSSL. Notes:
#   * no-asm: there is no ARM32 assembly path through clang-cl.
#   * _WIN32_WINNT=0x0603 matches the engine's WIN81 target. It selects BCryptGenRandom from bcrypt.dll
#     (present on Windows RT 8.1, and already imported by WinPR).
#   * VC-WIN32-ARM defines OPENSSL_SYS_WIN_CORE; in OpenSSL 3.5 that only makes it skip GUI and
#     Event Log calls (crypto/cryptlib.c) -- fewer imports, nothing newer than Windows 8.1.
#   * clang-cl-arm-shim.h is force-included for _InterlockedAdd64 (crypto/threads_win.c), an MSVC ARM32
#     intrinsic clang does not provide.
#
# Prerequisites, beyond armenv.ps1's: a native Windows Perl (e.g. Strawberry Perl; MSYS/Git perl will not
# do) and MSVC's host nmake.exe, either on PATH or named by PERL_DIR / NMAKE_DIR.
param([Parameter(Mandatory)][string]$Source,
      [Parameter(Mandatory)][string]$Build,
      [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$port = $PSScriptRoot
. (Join-Path $port 'armenv.ps1')
foreach ($v in 'PERL_DIR', 'NMAKE_DIR') { $d = [Environment]::GetEnvironmentVariable($v); if ($d) { $env:PATH = "$d;" + $env:PATH } }
foreach ($tool in 'perl', 'nmake', 'clang-cl', 'llvm-lib') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found on PATH (see the prerequisites at the top of this script)." }
}
& perl -e 'exit($^O eq q{MSWin32} ? 0 : 1)'
if ($LASTEXITCODE -ne 0) { throw 'perl is not a native Windows perl ($^O is not MSWin32); use Strawberry Perl.' }
foreach ($d in $Build, $Out) { if ((Test-Path $d) -and (Get-ChildItem $d -Force | Select-Object -First 1)) { throw "not empty: $d" } }
New-Item $Build -ItemType Directory -Force | Out-Null
$shim = Join-Path $port 'clang-cl-arm-shim.h'
if ($shim -match '\s') { throw "the shim path must not contain spaces (it becomes part of CC): $shim" }

Push-Location $Build
try {
    & perl (Join-Path $Source 'Configure') VC-WIN32-ARM no-asm enable-static-engine enable-capieng `
        no-tests no-docs no-shared no-module no-apps no-uplink no-makedepend `
        "CC=clang-cl --target=thumbv7-unknown-windows-msvc /GS- -D_ARM_ /Brepro /FI$shim" 'AR=llvm-lib' 'LD=lld-link' 'RC=llvm-rc' `
        '-D_WIN32_WINNT=0x0603'
    if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
    & nmake /nologo build_libs
    if ($LASTEXITCODE -ne 0) { throw 'build_libs failed' }
}
finally { Pop-Location }

# Collect the two libraries and the public headers (generated ones from the build dir, the rest from the source).
New-Item (Join-Path $Out 'lib') -ItemType Directory -Force | Out-Null
New-Item (Join-Path $Out 'include\openssl') -ItemType Directory -Force | Out-Null
Copy-Item (Join-Path $Build 'libcrypto.lib'), (Join-Path $Build 'libssl.lib') (Join-Path $Out 'lib')
Copy-Item (Join-Path $Source 'include\openssl\*.h') (Join-Path $Out 'include\openssl')
Copy-Item (Join-Path $Build 'include\openssl\*.h') (Join-Path $Out 'include\openssl') -Force
'OpenSSL ARM32: ' + (Join-Path $Out 'lib')
