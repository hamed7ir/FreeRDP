# FreeRDP — Finestra's ARM32 / Windows RT fork

A lightly-patched **FreeRDP 3.28.0** whose Windows client (`wfreerdp.exe`) is the RDP engine embedded
by [**Finestra**](https://github.com/hamed7ir/Finestra), a themed RDP / SSH / SFTP·FTP·FTPS manager
whose flagship target is **Windows RT 8.1 on ARM32** (Surface RT / Surface 2) — a platform upstream
does not ship binaries for.

> Not affiliated with, or endorsed by, the FreeRDP project. **Report anything you find here to this
> repository, never to upstream FreeRDP**, unless you have first reproduced it against unmodified
> upstream.

## Provenance

| | |
|---|---|
| upstream | `https://github.com/FreeRDP/FreeRDP` |
| fork point | **`5370fb26fbf034ecd11d3026b6ad639b5fff493f`** — tag **`3.28.0`** (2026-06-30) |
| application | [Finestra](https://github.com/hamed7ir/Finestra) |

`git merge-base HEAD 5370fb2` should return `5370fb2` exactly; if it does not, this README is stale.

This branch is deliberately small. Its diff against `3.28.0` touches **exactly the four source changes
below and nothing else** — not even `.gitignore`. Separately, it **adds** one new top-level directory,
[`finestra-rt/`](finestra-rt/), containing the ARM32 cross-build material (toolchain, generated headers,
one prebuilt archive, licences). No upstream file is modified to accommodate it.

## What this fork changes

1. **`winpr/sspi`: dispatch by package index instead of a mixed-width name.** A genuine upstream
   defect, not a port change. The internal SSPI stored the security package's *name* in the
   `SecHandle` upper pointer and read it back at a different character width, so under `-DUNICODE` the
   lookup could not match the name it had itself stored — and with `WITH_NATIVE_SSPI=OFF`, NLA failed
   with `SEC_E_SECPKG_NOT_FOUND`. Replaced with a width-agnostic 1-based package index.
   **This commit is standalone and is offered upstream separately.**
2. **`client/windows`: `/parent-window` embedding fixes + a local stats/control pipe.** Finestra hosts
   each session as a child window. Adds an opt-in per-process named pipe publishing the RTT/bandwidth
   figures FreeRDP's autodetect already computes, and accepting PAUSE/RESUME (suppress-output).
3. **`cmake`: the missing `WIN81` case for `CMAKE_WINDOWS_VERSION`.** Upstream accepts WIN7, WIN8 and
   WIN10 but not 8.1, so targeting the 8.1 API floor meant overshooting or setting the macros by hand.
4. **`client/windows`: scale the session pointer to the window's DPI.** Pointer bitmaps were used at
   their protocol pixel size, so above 100% display scaling the session cursor rendered visibly
   smaller than every other cursor. `GetDpiForWindow` is resolved dynamically via `GetProcAddress` so
   it never becomes a load-time import (Windows RT 8.1's loader lacks that Windows-10-only export),
   falling back to `GetDeviceCaps(LOGPIXELSX)`.

Camera redirection (a Media Foundation `rdpecam` backend with hardware H.264 encode) and a windowed
Windows-key passthrough exist as **local x64 debug spikes**. They are deliberately **not** on this
branch and are in no shipped artifact.

## Building

Build directories are siblings of the source tree, never inside it. Target: `wfreerdp`.

### x64 / x86 — reproducible from public sources; dependencies **not** vendored

MSVC (`vcvars64.bat` / `vcvars32.bat`) + Ninja + **vcpkg static OpenSSL and zlib**
(`x64-windows-static` / `x86-windows-static`). Nothing needs to be vendored here and nothing is: these
are a standard vcpkg triplet that anyone can install.

**Requirement:** static **OpenSSL 3.6.x** and **zlib 1.3.x**.
**Our exact pins**, for anyone who wants byte-comparable output: **OpenSSL 3.6.3** and **zlib 1.3.2**.
Note that `vcpkg install` gives you whatever the registry points at today, so matching those exact
versions requires pinning the vcpkg baseline — a plain install will build fine but will not reproduce
identical binaries.

Shipping engines are configured with `WITH_NATIVE_SSPI=OFF`, `WITH_INTERNAL_RC4=ON`,
`WITH_INTERNAL_MD4=ON` (so NLA rides WinPR's internal NTLM — hence change 1 above), `WITH_WINMM=ON`,
static `/MT`, and the server/SDL/sample components off.

On x86 specifically, MSVC overwrites the public `FindOpenSSL`/`FindZLIB` result variables, so the
libraries must be pinned through the internal ones (`LIB_EAY_RELEASE`, `SSL_EAY_RELEASE`,
`ZLIB_LIBRARY_RELEASE` and the `_DEBUG` pair). A configure line relying on the public variables fails.

### ARM32 / Windows RT 8.1 — reproducible from documented sources

The ARM32 path is a clang-cl cross-compile to `thumbv7-unknown-windows-msvc`. **The toolchain, the
compatibility shim, the generated OpenSSL/zlib headers and one prebuilt archive are now in this
repository**, under [`finestra-rt/`](finestra-rt/). Start from
[`finestra-rt/port/rt-configure-pins.txt`](finestra-rt/port/rt-configure-pins.txt), which is the exact
configure → build → verify sequence.

**What is here:**

| Component | What it is |
|---|---|
| `finestra-rt/port/thumbv7-rt.cmake` | CMake toolchain: `--target=thumbv7-unknown-windows-msvc`, `/GS-`, `-D_ARM_`, lld-link, the ARM32 `INCLUDE`/`LIB`, and the provider archive on the link line |
| `finestra-rt/port/armenv.ps1` | companion environment script — **must be dot-sourced** in the same shell as both `cmake` and `ninja` |
| `finestra-rt/port/clang-cl-arm-shim.h` | 936-byte force-include. MSVC's STL `<bit>` uses the ARM `_CountLeadingZeros` intrinsics in its `_M_ARM` branch without guarding for clang, which does not declare them; this supplies them over `__builtin_clz`. Apache-2.0. |
| `finestra-rt/deps-headers/openssl-3.3.2/`, `zlib-1.3.1/` | the **generated** headers from the OpenSSL configure run. Stock upstream headers are **not** a safe substitute — they must match the configuration the libraries were built with. |
| `finestra-rt/rt-deps/libproviders.lib` | prebuilt **OpenSSL 3.3.2** provider archive for ARM32. Vendored because it is the one component that cannot currently be regenerated from source — see [`finestra-rt/NOTICE.txt`](finestra-rt/NOTICE.txt) §3. |

**What you must supply — three prebuilt ARM32 static libraries, not redistributed here:**

| Library | Version | How it was produced |
|---|---|---|
| `libcrypto.lib` | OpenSSL 3.3.2, ARMNT, static `/MT` | `perl Configure VC-WIN32-ARM no-asm`, compiled with clang-cl thumbv7 |
| `libssl.lib` | OpenSSL 3.3.2, ARMNT, static `/MT` | same run |
| `z.lib` | zlib 1.3.1, ARMNT, static `/MT` | same toolchain and flags |

⚠ That `Configure` invocation is **recorded from the original build and has not been re-verified** —
nobody has yet re-run it from scratch and confirmed it reproduces linkable libraries. Treat it as the
best available record, not a tested procedure. `no-asm` is not optional, and the vendored headers are
configured to match it. Full detail in [`finestra-rt/NOTICE.txt`](finestra-rt/NOTICE.txt) §2.

**Non-redistributable prerequisites** (install these yourself; the toolchain resolves them at their
standard locations and every path is overridable by an environment variable of the same name):

| Prerequisite | Version | Variable | Default |
|---|---|---|---|
| CMake | **3.20+** (verified with 4.4) | — | on `PATH` |
| LLVM (clang-cl, lld-link, llvm-lib, llvm-rc, llvm-readobj) | **18.1.8** | `LLVM_ROOT` | `C:\Program Files\LLVM` |
| MSVC toolset headers | **14.44.35207** | `MSVC_STL_INCLUDE_DIR` | `…\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\include` |
| MSVC **v141 / 14.16.27023** ARM32 CRT | **14.16.27023** — later toolsets dropped the ARM32 CRT, so this specific one is required | `MSVC_ARM32_LIB_DIR` | `…\Microsoft Visual Studio\2017\BuildTools\VC\Tools\MSVC\14.16.27023\lib\arm` |
| Windows SDK | **10.0.19041.0** | `WINSDK_ROOT`, `WINSDK_VER` | `C:\Program Files (x86)\Windows Kits\10` |
| Ninja | any recent | `NINJA_DIR` | the copy shipped with VS Build Tools |

The MSVC and Windows SDK components reach the compiler through `%INCLUDE%` / `%LIB%`, which
`armenv.ps1` sets — they are not named in the generated build files, so a missing one surfaces as a
compiler error rather than a CMake error. That is why `armenv.ps1` existence-checks every one of them
and warns per path before you start.

**Import gate — mandatory for any ARM32 binary.** Run
`llvm-readobj --file-headers --coff-imports wfreerdp.exe` and require all three: machine type
`IMAGE_FILE_MACHINE_ARMNT (0x1C4)`; **zero** `api-ms-win-*` imports; **no** `secur32.dll`. Windows RT
8.1 lacks the API-set umbrella DLLs and native SSPI, and a violation makes the binary fail to load
outright rather than degrade.

## Modification notice (Apache-2.0 §4(b))

This repository contains a **modified** version of FreeRDP, which is licensed under the Apache
License, Version 2.0; this fork remains under the same licence. Relative to upstream `3.28.0`, the
changed files are exactly:

```
CMakeLists.txt
client/Windows/wf_client.c
client/Windows/wf_event.c
client/Windows/wf_event.h
client/Windows/wf_graphics.c
winpr/libwinpr/sspi/sspi.h
winpr/libwinpr/sspi/sspi_winpr.c
winpr/libwinpr/sspi/NTLM/ntlm.c
winpr/libwinpr/sspi/Negotiate/negotiate.c
```

No other upstream file is modified. The `finestra-rt/` directory is **added**, not modified: it
contains this project's own build material plus redistributed third-party components, each with its
own licence and modification notice — see [`finestra-rt/NOTICE.txt`](finestra-rt/NOTICE.txt),
[`LICENSE.OpenSSL-3.3.2.txt`](finestra-rt/LICENSE.OpenSSL-3.3.2.txt) and
[`LICENSE.zlib-1.3.1.txt`](finestra-rt/LICENSE.zlib-1.3.1.txt).

Upstream's original README is preserved verbatim as [`README-UPSTREAM.md`](README-UPSTREAM.md).

## Licence

Apache License 2.0 — see [`LICENSE`](LICENSE), unchanged from upstream.
