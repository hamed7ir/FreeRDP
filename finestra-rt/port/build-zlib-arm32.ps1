# build-zlib-arm32.ps1 -- zlib static library for ARM32 / Windows RT 8.1 (ARMNT).
#
#   powershell -File build-zlib-arm32.ps1 -Source <zlib-1.3.2 source dir> -Build <empty dir> -Out <empty dir>
#
# Produces <Out>\z.lib and <Out>\include\{zlib.h,zconf.h}. Compiles the 15 library sources that zlib's own
# win32/Makefile.msc builds, with the engine toolchain's flags (thumbv7-rt.cmake) and /MT /Zl, so the
# C runtime is chosen by the engine's final link.
param([Parameter(Mandatory)][string]$Source,
      [Parameter(Mandatory)][string]$Build,
      [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'armenv.ps1')
foreach ($d in $Build, $Out) { if ((Test-Path $d) -and (Get-ChildItem $d -Force | Select-Object -First 1)) { throw "not empty: $d" } }
New-Item $Build, (Join-Path $Out 'include') -ItemType Directory -Force | Out-Null
$srcs = 'adler32', 'compress', 'crc32', 'deflate', 'gzclose', 'gzlib', 'gzread', 'gzwrite', 'infback', 'inffast',
        'inflate', 'inftrees', 'trees', 'uncompr', 'zutil'
# /Brepro and bare archive member names make the output byte-identical from run to run, whatever the
# build directory is called.
$flags = '--target=thumbv7-unknown-windows-msvc', '/GS-', '-D_ARM_', '/nologo', '/MT', '/Zl', '/O2', '/W3', '-DNDEBUG',
         '-D_CRT_SECURE_NO_DEPRECATE', '-D_CRT_NONSTDC_NO_DEPRECATE', '-D_WIN32_WINNT=0x0603', '/Brepro'
$Out = (Resolve-Path $Out).Path
Push-Location $Build
try {
    $objs = foreach ($s in $srcs) {
        & clang-cl @flags /c (Join-Path $Source "$s.c") "/Fo$s.obj"
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $s.c" }
        "$s.obj"
    }
    # llvm-lib records member paths relative to the archive's own folder: write it HERE, then copy it.
    & llvm-lib /nologo /out:z.lib @objs
    if ($LASTEXITCODE -ne 0) { throw 'llvm-lib failed' }
}
finally { Pop-Location }
Copy-Item (Join-Path $Build 'z.lib') (Join-Path $Out 'z.lib')
Copy-Item (Join-Path $Source 'zlib.h'), (Join-Path $Source 'zconf.h') (Join-Path $Out 'include')
'zlib ARM32: ' + (Join-Path $Out 'z.lib')
