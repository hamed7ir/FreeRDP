# FreeRDP — Finestra's ARM32 / Windows RT fork

A lightly-patched **FreeRDP 3.32.2** whose **SDL3 client** (`sdl-freerdp.exe`) is the RDP engine embedded
by [**Finestra**](https://github.com/hamed7ir/Finestra), a themed RDP / SSH / SFTP·FTP·FTPS manager
whose flagship target is **Windows RT 8.1 on ARM32** (Surface RT / Surface 2) — a platform upstream
does not ship binaries for. Finestra 2.0 switched from the Windows client (`wfreerdp.exe`) to the SDL3
client for its touch support; this branch still carries Finestra's changes to the Windows client too.

> Not affiliated with, or endorsed by, the FreeRDP project. **Report anything you find here to this
> repository, never to upstream FreeRDP**, unless you have first reproduced it against unmodified
> upstream.

## Provenance

| | |
|---|---|
| upstream | `https://github.com/FreeRDP/FreeRDP` |
| fork point | **`97eff6ddfbbfaa8e42a126d362686bd001ff8bdc`** — tag **`3.32.2`** (2026-10-12) |
| application | [Finestra](https://github.com/hamed7ir/Finestra) |

`git merge-base HEAD 97eff6d` should return `97eff6d` exactly; if it does not, this README is stale.

This branch is deliberately small. Its diff against `3.32.2` consists of **exactly the six source changes
below and nothing else** — not even `.gitignore`. Separately, it **adds** one new top-level directory,
[`finestra-rt/`](finestra-rt/), containing the ARM32 cross-build material (toolchain, build scripts for
the ARM32 libraries, generated headers, licences). No upstream file is modified to accommodate it, and
no prebuilt binary is in the tree.

## What this fork changes

**In the SDL3 client** (the engine Finestra ships):

1. **`client/SDL`: running inside Finestra.** With `/parent-window:<hwnd>` the session window is created
   as a child of that window and handed to SDL, and keystrokes are sent to the server from it. A
   per-process local named pipe reports live RTT, bandwidth and the input mode, and accepts PAUSE/RESUME
   (suppress-output) and TOUCH ON/OFF. In **mouse mode** Windows' own touch-to-mouse promotion drives the
   pointer (a tap clicks, press-and-hold is the right button); in **touch mode** touch goes to the server
   as multi-touch input (MS-RDPEI, `/multitouch`). Without `/parent-window` the client behaves as
   upstream's, with one exception in the ARM32 build: it defaults to SDL's Direct3D 9 renderer, because
   Direct3D 11 at feature level 9_1 draws a black screen on Windows RT (an `SDL_RENDER_DRIVER` setting
   still takes precedence). New files `sdl_finestra.cpp` and `sdl_finestra.hpp`; small hooks in
   `sdl_context`, `sdl_window`, `sdl_disp` and `sdl_freerdp`.

**In code shared by every client** (both are upstream bugs, offered upstream):

2. **`client/common`: fill in the active contact count in `freerdp_client_touch_update`.** The
   touch-to-mouse emulation picks the mouse button from the contact count, which nothing ever set.
3. **`locale`: use `UINT32` for the keyboard remapping key and value.** One caller of
   `freerdp_extract_key_value` passed `DWORD` pointers where the function takes `UINT32` (its other caller
   already uses `UINT32`); recent clang rejects that. Same values, matching types.

**In the Windows client** (`wfreerdp`, which Finestra 1.x shipped):

4. **`client/windows`: a local stats/control pipe** like the one above (live RTT and bandwidth from
   FreeRDP's network autodetect, and PAUSE/RESUME). The commit's message also mentions `/parent-window`
   embedding fixes; as applied to 3.32.2 the change contains only the pipe.
5. **`client/windows`: scale the session pointer to the window's DPI.** `GetDpiForWindow` is resolved
   dynamically via `GetProcAddress` so it never becomes a load-time import (Windows RT 8.1's loader lacks
   that Windows-10-only export), falling back to `GetDeviceCaps(LOGPIXELSX)`.

**In the build:**

6. **`cmake`: the missing `WIN81` case for `CMAKE_WINDOWS_VERSION`.** Upstream accepts WIN7, WIN8 and
   WIN10 but not 8.1, so targeting the 8.1 API floor meant overshooting or setting the macros by hand.

Each is one commit; `git log --oneline 3.32.2..finestra-arm32-rt-3.32` lists them with the documentation
and `finestra-rt/` commits:

| # | commit | change | files |
|---|---|---|---|
| 1 | `37e03d1f0` | SDL3 client: run inside Finestra (embedding, touch and mouse modes, stats pipe) | 7 |
| 2 | `57fd8b939` | shared client code: fill in the active contact count in `freerdp_client_touch_update` | 1 |
| 3 | `7b2147e75` | libfreerdp: use `UINT32` for the keyboard remapping key and value | 1 |
| 4 | `bc8568940` | Windows client: a local stats/control pipe | 3 |
| 5 | `f345d2ac4` | Windows client: scale the session pointer to the window's DPI | 1 |
| 6 | `30e98430d` | CMake: add the missing `WIN81` case to `CMAKE_WINDOWS_VERSION` | 1 |

Check it rather than take it on trust:

```
git diff --name-status 3.32.2 finestra-arm32-rt-3.32 -- . ':(exclude)finestra-rt' ':(exclude)README*.md'
```

That lists **exactly 14 files and nothing else**: 12 modified and the 2 added SDL files. Every other
commit in `git log --oneline 3.32.2..finestra-arm32-rt-3.32` touches only `README*.md` and
`finestra-rt/`, which is what the `--name-status` command above demonstrates.

**Earlier branch.** Finestra 1.0.3 and earlier shipped engines from `finestra-arm32-rt-3.28` (FreeRDP
3.28.0), which carried a further change: a WinPR internal-SSPI fix (dispatch by package index instead of
a mixed-width name). It was accepted upstream as FreeRDP pull request #13136 and is part of 3.32.2, so
this branch does not modify WinPR.

## Building

Build directories are siblings of the source tree, never inside it. Target: **`sdl3-freerdp`** (the
binary is `client\SDL\SDL3\sdl-freerdp.exe`). The Windows client still builds as target `wfreerdp`.

Shipping engines are configured with `WITH_CLIENT_SDL=ON`, `WITH_CLIENT_SDL3=ON`,
`WITH_CLIENT_SDL2=OFF`, `WITH_SDL_LINK_SHARED=OFF` (SDL3 and SDL3_ttf linked statically),
`WITH_NATIVE_SSPI=OFF`, `WITH_INTERNAL_RC4=ON`, `WITH_INTERNAL_MD4=ON` (so NLA rides WinPR's internal
NTLM), `WITH_WINMM=ON`, `WITH_VERBOSE_WINPR_ASSERT=OFF`, static `/MT`, and the server and sample
components off.

The SDL3 client needs three more static libraries, built from their official source releases with the
options below (both architectures):

| Library | Version | Configure options (beyond Release, static, `/MT`) |
|---|---|---|
| SDL | **3.2.14** | `SDL_SHARED=OFF SDL_STATIC=ON SDL_TESTS=OFF SDL_EXAMPLES=OFF SDL_AUDIO=ON SDL_OPENGL=OFF SDL_OPENGLES=OFF SDL_RENDER_D3D=ON SDL_RENDER_D3D11=ON SDL_RENDER_D3D12=OFF SDL_VULKAN=OFF SDL_GPU=OFF SDL_CAMERA=OFF SDL_JOYSTICK=OFF SDL_HAPTIC=OFF SDL_HIDAPI=OFF SDL_DIALOG=OFF SDL_POWER=OFF SDL_SENSOR=OFF` (ARM32 adds `SDL_ASSEMBLY=OFF` and every SIMD option OFF) |
| FreeType | **2.15.0** | `FT_DISABLE_ZLIB=ON FT_DISABLE_BZIP2=ON FT_DISABLE_PNG=ON FT_DISABLE_HARFBUZZ=ON FT_DISABLE_BROTLI=ON` |
| SDL3_ttf | **3.2.2** | `SDLTTF_VENDORED=OFF SDLTTF_HARFBUZZ=OFF SDLTTF_PLUTOSVG=OFF SDLTTF_SAMPLES=OFF`, against the SDL and FreeType above |

Pass them to the engine build through `CMAKE_PREFIX_PATH` (the three install prefixes).

### x64 — reproducible from public sources; dependencies **not** vendored

MSVC (`vcvars64.bat`) + Ninja + static **OpenSSL**, **zlib** and the three SDL3-client libraries above.

**Our exact pins**, for anyone who wants byte-comparable output: **OpenSSL 3.5.9** (the LTS line) and
**zlib 1.3.2**. zlib comes from a vcpkg `x64-windows-static` install. OpenSSL is built from the official
3.5.9 source release with OpenSSL's own `VC-WIN64A` target, using the options vcpkg's port uses for a
static triplet: `enable-static-engine enable-capieng no-tests no-docs no-shared no-module no-apps`
(assembly enabled, so NASM and a native Windows Perl are needed). Pass it to the engine build through
`OPENSSL_INCLUDE_DIR`, `LIB_EAY_RELEASE` and `SSL_EAY_RELEASE`, so the vcpkg tree's own OpenSSL, if it has
one, cannot be picked up instead. FreeType and SDL3_ttf were built with MSVC; SDL with clang-cl.

When the vcpkg toolchain file is used with a vcpkg tree that has no `vcpkg.exe` (a copied, pinned
`installed\` tree), also pass `-DVCPKG_APPLOCAL_DEPS=OFF`: the post-link step that copies DLLs beside the
executable calls `vcpkg.exe`, and a fully static build has no DLLs to copy.

On x86 (not shipped by Finestra), MSVC overwrites the public `FindOpenSSL`/`FindZLIB` result variables, so
the libraries must be pinned through the internal ones (`LIB_EAY_RELEASE`, `SSL_EAY_RELEASE`,
`ZLIB_LIBRARY_RELEASE` and the `_DEBUG` pair).

### ARM32 / Windows RT 8.1 — reproducible from documented sources

The ARM32 path is a clang-cl cross-compile to `thumbv7-unknown-windows-msvc`. **The toolchain, the
compatibility shim, the build scripts for the OpenSSL and zlib libraries and their generated headers are
in this repository**, under [`finestra-rt/`](finestra-rt/). Start from
[`finestra-rt/port/rt-configure-pins.txt`](finestra-rt/port/rt-configure-pins.txt), which is the exact
build-the-libraries → configure → build → verify sequence.

**The compiler matters.** The SDL3 client is C++, and stock LLVM 18 miscompiles some ARM32 C++ code
(virtual-call thunks) and rejects `__try`, which SDL uses. The shipped ARM32 engine and **every** library
it links were compiled with **rt2.1**, a patched LLVM 23.1.1 from the
[hamed7ir/llvm-rt1](https://github.com/hamed7ir/llvm-rt1) series (`clang-cl --version` prints
`23.1.1-rt2.1 (e240ad76…)`), against **MSVC 14.44**'s ARM32 libraries (C++ objects must match the
14.44 STL headers). Its predecessor **rt2** is published there now; rt2.1 is the next release in that
series. The C-only Windows client still builds with stock LLVM 18.1.8 and MSVC 14.16, the toolchain
file's defaults.

**What is here:**

| Component | What it is |
|---|---|
| `finestra-rt/port/thumbv7-rt.cmake` | CMake toolchain: `--target=thumbv7-unknown-windows-msvc`, `/GS-`, `-D_ARM_`, lld-link, the ARM32 `INCLUDE`/`LIB`; for C++ also `/EHsc` and `/UWINPR_DEFINE_ATTR_NODISCARD` (clang rejects the attribute where FreeRDP's headers place it after `__declspec`) |
| `finestra-rt/port/armenv.ps1` | companion environment script — **must be dot-sourced** in the same shell as both `cmake` and `ninja` |
| `finestra-rt/port/clang-cl-arm-shim.h` | force-include supplying MSVC ARM32 intrinsics older clangs do not declare: `_CountLeadingZeros(64)` (MSVC's STL `<bit>`) and `_InterlockedAdd64` (OpenSSL 3.5's `threads_win.c`). Each stands down where the compiler has it built in (rt2.1 has all three). Apache-2.0. |
| `finestra-rt/port/build-openssl-arm32.ps1` | builds `libcrypto.lib` + `libssl.lib` (OpenSSL 3.5.9, ARMNT, static) from the official source release |
| `finestra-rt/port/build-zlib-arm32.ps1` | builds `z.lib` (zlib 1.3.2, ARMNT, static) from the official source release |
| `finestra-rt/deps-headers/openssl-3.5.9/`, `zlib-1.3.2/` | the headers matching those libraries, including OpenSSL's **generated** ones. Stock upstream OpenSSL headers are **not** a safe substitute — they must match the configuration the libraries were built with. |

**What you build yourself — six ARM32 static libraries, not redistributed here:**

| Library | Version | Produced by |
|---|---|---|
| `libcrypto.lib` | OpenSSL 3.5.9, ARMNT, static `/MT` | `build-openssl-arm32.ps1` — OpenSSL's `VC-WIN32-ARM` target with clang-cl, `no-asm` |
| `libssl.lib` | OpenSSL 3.5.9, ARMNT, static `/MT` | same run |
| `z.lib` | zlib 1.3.2, ARMNT, static `/MT` | `build-zlib-arm32.ps1` |
| `SDL3-static.lib` | SDL 3.2.14, ARMNT | CMake, options above; see `rt-configure-pins.txt` STEP 0b |
| `freetype.lib` | FreeType 2.15.0, ARMNT | CMake, options above; STEP 0b |
| `SDL3_ttf-static.lib` | SDL3_ttf 3.2.2, ARMNT | CMake, options above; STEP 0b |

All six were rebuilt from scratch with rt2.1 on 2026-10-21 to produce the libraries the shipped engine
links; the zlib script's output is byte-identical between runs. The OpenSSL `libcrypto.lib` is
self-contained (the default provider is inside it), so no separate provider archive is needed. Full
detail, including OpenSSL's use of `BCryptGenRandom`, is in [`finestra-rt/NOTICE.txt`](finestra-rt/NOTICE.txt).

**Non-redistributable prerequisites** (install these yourself; the toolchain resolves them at their
standard locations and every path is overridable by an environment variable of the same name):

| Prerequisite | Version | Variable | Default |
|---|---|---|---|
| CMake | **3.20+** (verified with 4.4) | — | on `PATH` |
| LLVM (clang-cl, lld-link, llvm-lib, llvm-rc, llvm-readobj) | **rt2.1** (23.1.1-rt2.1) for the SDL3 client; 18.1.8 suffices for the Windows client | `LLVM_ROOT` | `C:\Program Files\LLVM` |
| MSVC toolset headers | **14.44.35207** | `MSVC_STL_INCLUDE_DIR` | `…\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\include` |
| MSVC ARM32 libraries | **14.44.35207** `lib\arm` for the SDL3 client; 14.16.27023 is the Windows client's verified default | `MSVC_ARM32_LIB_DIR` | `…\Microsoft Visual Studio\2017\BuildTools\VC\Tools\MSVC\14.16.27023\lib\arm` |
| Windows SDK | **10.0.19041.0** (the last with ARM32 import libraries) | `WINSDK_ROOT`, `WINSDK_VER` | `C:\Program Files (x86)\Windows Kits\10` |
| Ninja | any recent | `NINJA_DIR` | the copy shipped with VS Build Tools |
| Perl (native Windows, e.g. Strawberry) — OpenSSL script only | 5.x | `PERL_DIR` | on `PATH` |
| MSVC host `nmake.exe` — OpenSSL script only | from the 14.44 toolset | `NMAKE_DIR` | on `PATH` |

The MSVC and Windows SDK components reach the compiler through `%INCLUDE%` / `%LIB%`, which
`armenv.ps1` sets — they are not named in the generated build files, so a missing one surfaces as a
compiler error rather than a CMake error. That is why `armenv.ps1` existence-checks every one of them
and warns per path before you start.

**Import gate — mandatory for any ARM32 binary.** Run
`llvm-readobj --file-headers --coff-imports sdl-freerdp.exe` and require all three: machine type
`IMAGE_FILE_MACHINE_ARMNT (0x1C4)`; **zero** `api-ms-win-*` imports; **no** `secur32.dll`. Windows RT
8.1 lacks the API-set umbrella DLLs and native SSPI, and a violation makes the binary fail to load
outright rather than degrade. Also check that `BCryptGenRandom` is imported from `bcrypt.dll`: the
toolchain lists `bcrypt.lib` first for exactly that reason (otherwise it resolves through a forwarder in
`ncrypt.dll` that nothing proves Windows RT 8.1 has).

## Modification notice (Apache-2.0 §4(b))

This repository contains a **modified** version of FreeRDP, which is licensed under the Apache
License, Version 2.0; this fork remains under the same licence. Relative to upstream `3.32.2`, the
changed files are exactly:

```
CMakeLists.txt
client/SDL/SDL3/CMakeLists.txt
client/SDL/SDL3/sdl_context.cpp
client/SDL/SDL3/sdl_disp.cpp
client/SDL/SDL3/sdl_finestra.cpp      (added)
client/SDL/SDL3/sdl_finestra.hpp      (added)
client/SDL/SDL3/sdl_freerdp.cpp
client/SDL/SDL3/sdl_window.cpp
client/Windows/wf_client.c
client/Windows/wf_event.c
client/Windows/wf_event.h
client/Windows/wf_graphics.c
client/common/client.c
libfreerdp/locale/keyboard.c
```

No other upstream file is modified. The `finestra-rt/` directory is **added**, not modified: it
contains this project's own build material plus redistributed third-party headers, each with its
own licence and modification notice — see [`finestra-rt/NOTICE.txt`](finestra-rt/NOTICE.txt),
[`LICENSE.OpenSSL-3.5.9.txt`](finestra-rt/LICENSE.OpenSSL-3.5.9.txt) and
[`LICENSE.zlib-1.3.2.txt`](finestra-rt/LICENSE.zlib-1.3.2.txt).

Upstream's original README is preserved verbatim as [`README-UPSTREAM.md`](README-UPSTREAM.md).

## Licence

Apache License 2.0 — see [`LICENSE`](LICENSE), unchanged from upstream.
