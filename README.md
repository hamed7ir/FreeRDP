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

This branch is deliberately small: **its diff against `3.28.0` is exactly the four changes below and
nothing else** — not even `.gitignore`.

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

### x64 / x86 — reproducible from public sources

MSVC (`vcvars64.bat` / `vcvars32.bat`) + Ninja + **vcpkg static OpenSSL and zlib**
(`x64-windows-static` / `x86-windows-static`). Shipping engines are configured with
`WITH_NATIVE_SSPI=OFF`, `WITH_INTERNAL_RC4=ON`, `WITH_INTERNAL_MD4=ON` (so NLA rides WinPR's internal
NTLM — hence change 1 above), `WITH_WINMM=ON`, static `/MT`, and the server/SDL/sample components off.

### ARM32 / Windows RT 8.1 — ⚠ NOT reproducible from this repository alone

**Read this before assuming the RT engine builds.** The ARM32 path is a clang-cl cross-compile to
`thumbv7-unknown-windows-msvc`, and its toolchain file, environment script and prebuilt ARM32
dependencies are **not currently part of this repository**. They live in the maintainer's build tree.
This README will not pretend otherwise.

To reproduce it today you would need, in addition to this source:

| Prerequisite | What it is | Licence | Notes |
|---|---|---|---|
| CMake toolchain file + env script | Sets `--target=thumbv7-unknown-windows-msvc`, `/GS-`, `-D_ARM_`, lld-link, and the `INCLUDE`/`LIB` for an ARM32 build | project's own | not in this repo |
| `clang-cl-arm-shim.h` | 936-byte force-include. MSVC's STL `<bit>` uses the ARM `_CountLeadingZeros` intrinsics in its `_M_ARM` branch without guarding for clang, which does not declare them; this supplies them over `__builtin_clz`. | project's own | not in this repo |
| `libcrypto.lib`, `libssl.lib`, `libproviders.lib` | **OpenSSL 3.3.2** built for ARM32 (`Configure VC-WIN32-ARM no-asm`, compiled with clang-cl thumbv7 `/MT`). `libproviders.lib` is OpenSSL's `providers/` subtree archived separately; lld needs it on the link line to resolve the default provider's algorithm implementations. | Apache-2.0 | not in this repo |
| `z.lib` | **zlib 1.3.1** built with the same clang-cl flags | zlib licence | easiest to rebuild yourself |
| OpenSSL/zlib headers | The **generated** headers from that same `Configure` run (`configuration.h`, `opensslconf.h`, `opensslv.h`). Substituting stock upstream headers is **not** safe — they must match the configuration the libraries were built with. | — | not in this repo |

Toolchain versions this engine was actually built and verified with: **LLVM 18.1.8**, MSVC STL
**14.44.35207**, the **v141 / 14.16.27023** ARM32 CRT (later MSVC toolsets dropped the ARM32 CRT), and
Windows SDK **10.0.19041.0**.

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

Upstream's original README is preserved verbatim as [`README-UPSTREAM.md`](README-UPSTREAM.md).

## Licence

Apache License 2.0 — see [`LICENSE`](LICENSE), unchanged from upstream.
