# ============================================================================
# finestra-rt/port/thumbv7-rt.cmake
# FreeRDP 3.28.0 -> thumbv7 (ARMNT) cross toolchain for Windows RT 8.1 / ARM32.
#
# COMPANION: armenv.ps1  --  MUST be dot-sourced in the shell before BOTH cmake
#   and ninja. It sets %INCLUDE% / %LIB% / %PATH% that clang-cl and lld-link read
#   at compile/link time (a toolchain file's set(ENV{}) only reaches configure-
#   time try_compiles, not the ninja build processes).
#
# PATHS: every host path below is overridable, either by -D<VAR> or by an
#   environment variable of the same name. The defaults are the STANDARD install
#   locations of the Microsoft and LLVM prerequisites, not any particular
#   machine's layout -- if yours differ, override rather than edit.
#   See finestra-rt/README.md for the exact component versions required.
# ============================================================================

set(CMAKE_SYSTEM_NAME      Windows)   # desktop RT. NOT WindowsStore -> that flips
                                      # UWP=1 (PlatformDefaults.cmake:11-15), which
                                      # kills the Windows client and native SSPI.
set(CMAKE_SYSTEM_PROCESSOR ARM)       # 32-bit ARM / ARMNT

# ---- tools (LLVM 18.1.8 verified) ------------------------------------------
if(NOT LLVM_ROOT)
  set(LLVM_ROOT "$ENV{LLVM_ROOT}")
endif()
if(NOT LLVM_ROOT)
  set(LLVM_ROOT "C:/Program Files/LLVM")   # stock LLVM installer location
endif()
# Anything arriving from the environment or -D on Windows may use backslashes, which CMake
# then re-emits into generated .cmake files where "\P" is an invalid escape and configure
# dies with a confusing parse error. Normalise every host path to forward slashes.
file(TO_CMAKE_PATH "${LLVM_ROOT}" LLVM_ROOT)
set(_LLVM "${LLVM_ROOT}/bin")
set(CMAKE_C_COMPILER      "${_LLVM}/clang-cl.exe")
set(CMAKE_CXX_COMPILER    "${_LLVM}/clang-cl.exe")   # in case a subdir enables CXX
set(CMAKE_AR              "${_LLVM}/llvm-lib.exe")
set(CMAKE_C_COMPILER_AR   "${_LLVM}/llvm-lib.exe")
set(CMAKE_CXX_COMPILER_AR "${_LLVM}/llvm-lib.exe")
set(CMAKE_RC_COMPILER     "${_LLVM}/llvm-rc.exe")
set(CMAKE_LINKER          "${_LLVM}/lld-link.exe")

# ---- target + core compile flags -------------------------------------------
# Deliberately NOT here (single-owner rules):
#   * /MT      -> owned by -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded (CMP0091 NEW)
#   * /O2      -> owned by -DCMAKE_BUILD_TYPE=Release
#   * WINVER/_WIN32_WINNT -> owned by the WIN81 patch (-DCMAKE_WINDOWS_VERSION=WIN81)
#   * -fsigned-char -> char is already SIGNED by default on thumbv7-windows-msvc
#                      (verified) and clang-cl rejects the flag anyway.
get_filename_component(_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

# The compatibility shim is vendored beside this file, so it always resolves
# relative to the toolchain itself -- no configuration needed.
set(_SHIM "${_PORT_DIR}/clang-cl-arm-shim.h")
if(NOT EXISTS "${_SHIM}")
  message(FATAL_ERROR "thumbv7-rt.cmake: missing ${_SHIM} (should be vendored beside this file).")
endif()
set(_CFLAGS "--target=thumbv7-unknown-windows-msvc /GS- /bigobj -D_ARM_ -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /FI\"${_SHIM}\"")
set(CMAKE_C_FLAGS_INIT   "${_CFLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${_CFLAGS}")

# ---- linker: lld-link, plus the OpenSSL-3 provider archive ------------------
# RT_DEPS_DIR holds libproviders.lib. It is vendored at finestra-rt/rt-deps, so the
# default needs no configuration; override with -DRT_DEPS_DIR or $env:FINESTRA_RT_DEPS.
if(NOT RT_DEPS_DIR)
  set(RT_DEPS_DIR "$ENV{FINESTRA_RT_DEPS}")
endif()
if(NOT RT_DEPS_DIR)
  set(RT_DEPS_DIR "${_PORT_DIR}/../rt-deps")
endif()
file(TO_CMAKE_PATH "${RT_DEPS_DIR}" RT_DEPS_DIR)
get_filename_component(RT_DEPS_DIR "${RT_DEPS_DIR}" ABSOLUTE)
set(_PROVIDERS "${RT_DEPS_DIR}/libproviders.lib")
if(NOT EXISTS "${_PROVIDERS}")
  message(FATAL_ERROR "thumbv7-rt.cmake: missing ${_PROVIDERS}. See finestra-rt/NOTICE.txt.")
endif()
# WHAT THIS ARCHIVE IS FOR (corrected 2026-08-03): it does NOT define
# ossl_default_provider_init -- that symbol is defined inside libcrypto.lib, verified
# with llvm-nm (libcrypto defines it; libproviders does not). This OpenSSL 3.3.2 build
# split the default provider's ALGORITHM IMPLEMENTATIONS into a separate archive, and
# libproviders.lib is that archive: 4,671 defined symbols -- the cipher, digest, KDF,
# MAC, RAND, keymgmt, signature and encoder/decoder implementations (ossl_gcm_cipher,
# ossl_rand_drbg_new, ossl_rsa_check_key, ...) that libcrypto's provider entry point
# then references. Without it the link fails on those implementation symbols, not on
# the init function. Only the final wfreerdp.exe links, so exe-flags is the right
# home; lld resolves the libcrypto<->libproviders cycle in one multi-pass.
# clang-cl cross only seeds CMAKE_C_STANDARD_LIBRARIES with kernel32+user32; a normal MSVC
# build also links gdi32/shell32/ole32/... . wfreerdp uses GDI + CommandLineToArgvW, so
# restore the full MSVC default Windows import-lib set here (resolved via %LIB% = SDK um\arm).
set(_WINLIBS "gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-fuse-ld=lld ${_PROVIDERS} ${_WINLIBS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
# NOTE: no CRT shim archive is linked here, and none is needed. An earlier dependency
# survey rated one "likely required" on symbol content alone; a real RT link resolves
# with zero undefined symbols without it. Do not add one on the strength of that note.

# ---- fail-fast on every pinned input ---------------------------------------
# CMake does NOT validate -DLIB_EAY_RELEASE / -DSSL_EAY_RELEASE / -DZLIB_LIBRARY* at configure
# time: a wrong or empty value configures cleanly and only surfaces as a ninja "missing and no
# known rule to make it" at LINK time, minutes later, naming the mangled path rather than the
# variable that produced it. (Observed for real: a shell variable collision turned a pin into
# "System.Collections.Specialized.OrderedDictionary/z.lib" and configure said nothing.)
# Check them here instead, where the message can name the variable.
# MUST be skipped inside try_compile: CMake re-reads this toolchain file for every compiler
# probe, and those sub-projects do NOT inherit -D cache entries, so the check would fire on
# every probe and kill an otherwise valid configure. IN_TRY_COMPILE is the documented way to
# tell the two apart (CMAKE_IN_TRY_COMPILE is deprecated).
get_property(_finestra_in_try_compile GLOBAL PROPERTY IN_TRY_COMPILE)
if(NOT _finestra_in_try_compile AND NOT DEFINED ENV{FINESTRA_RT_SKIP_PIN_CHECK})
  foreach(_pin LIB_EAY_RELEASE SSL_EAY_RELEASE ZLIB_LIBRARY_RELEASE)
    if(NOT DEFINED ${_pin} OR "${${_pin}}" STREQUAL "")
      message(FATAL_ERROR
        "thumbv7-rt.cmake: -D${_pin} is not set.\n"
        "  The ARM32 build needs three prebuilt static libraries that are NOT in this repository:\n"
        "    -DLIB_EAY_RELEASE=<dir>/libcrypto.lib   (OpenSSL 3.3.2, ARMNT, static /MT)\n"
        "    -DSSL_EAY_RELEASE=<dir>/libssl.lib      (OpenSSL 3.3.2, ARMNT, static /MT)\n"
        "    -DZLIB_LIBRARY_RELEASE=<dir>/z.lib      (zlib 1.3.1,   ARMNT, static /MT)\n"
        "  See finestra-rt/NOTICE.txt section 2 and finestra-rt/port/rt-configure-pins.txt.")
    endif()
    if(NOT EXISTS "${${_pin}}")
      message(FATAL_ERROR
        "thumbv7-rt.cmake: ${_pin} points at a file that does not exist:\n"
        "    ${${_pin}}\n"
        "  Fix the -D${_pin} value. See finestra-rt/NOTICE.txt section 2.")
    endif()
  endforeach()
  # ZLIB_LIBRARY is what FindZLIB ultimately consumes; check it too when it was supplied.
  if(DEFINED ZLIB_LIBRARY AND NOT "${ZLIB_LIBRARY}" STREQUAL "" AND NOT EXISTS "${ZLIB_LIBRARY}")
    message(FATAL_ERROR
      "thumbv7-rt.cmake: ZLIB_LIBRARY points at a file that does not exist:\n"
      "    ${ZLIB_LIBRARY}\n"
      "  Fix the -DZLIB_LIBRARY value. See finestra-rt/NOTICE.txt section 2.")
  endif()
endif()

# ---- cross hygiene ---------------------------------------------------------
# EXECUTABLE, not STATIC_LIBRARY: the CRT + SDK-arm link environment is complete, and
# link-based checks MUST actually link to be correct. Under STATIC_LIBRARY,
# check_function_exists(strerror_r/getlogin_r/getpwuid_r/timer_*) never links, so every POSIX
# function is a FALSE POSITIVE on Windows -> winpr/utils/debug.c then calls undeclared strerror_r.
set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)         # no clang LTO bitcode vs non-LTO prebuilt libs

# ---- INCLUDE / LIB ---------------------------------------------------------
# Backup for configure-time try_compiles; the build inherits these from armenv.ps1.
# MSVC 14.4x headers (C-only path, no STL) + Windows SDK 10.0.19041.0 ucrt/shared/um.
# MSVC 14.16 (v141) lib\arm has the ARM32 CRT -- later toolsets dropped it.
if(NOT MSVC_STL_INCLUDE_DIR)
  set(MSVC_STL_INCLUDE_DIR "$ENV{MSVC_STL_INCLUDE_DIR}")
endif()
if(NOT MSVC_STL_INCLUDE_DIR)
  set(MSVC_STL_INCLUDE_DIR "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/include")
endif()
if(NOT WINSDK_ROOT)
  set(WINSDK_ROOT "$ENV{WINSDK_ROOT}")
endif()
if(NOT WINSDK_ROOT)
  set(WINSDK_ROOT "C:/Program Files (x86)/Windows Kits/10")
endif()
if(NOT WINSDK_VER)
  set(WINSDK_VER "$ENV{WINSDK_VER}")
endif()
if(NOT WINSDK_VER)
  set(WINSDK_VER "10.0.19041.0")
endif()
file(TO_CMAKE_PATH "${MSVC_STL_INCLUDE_DIR}" MSVC_STL_INCLUDE_DIR)
file(TO_CMAKE_PATH "${WINSDK_ROOT}" WINSDK_ROOT)
set(ENV{INCLUDE} "${MSVC_STL_INCLUDE_DIR};${WINSDK_ROOT}/Include/${WINSDK_VER}/ucrt;${WINSDK_ROOT}/Include/${WINSDK_VER}/shared;${WINSDK_ROOT}/Include/${WINSDK_VER}/um")
if(NOT MSVC_ARM32_LIB_DIR)
  set(MSVC_ARM32_LIB_DIR "$ENV{MSVC_ARM32_LIB_DIR}")
endif()
if(NOT MSVC_ARM32_LIB_DIR)   # v141/14.16 - later toolsets dropped the ARM32 CRT
  set(MSVC_ARM32_LIB_DIR "C:/Program Files (x86)/Microsoft Visual Studio/2017/BuildTools/VC/Tools/MSVC/14.16.27023/lib/arm")
endif()
file(TO_CMAKE_PATH "${MSVC_ARM32_LIB_DIR}" MSVC_ARM32_LIB_DIR)
set(ENV{LIB}     "${MSVC_ARM32_LIB_DIR};${WINSDK_ROOT}/Lib/${WINSDK_VER}/ucrt/arm;${WINSDK_ROOT}/Lib/${WINSDK_VER}/um/arm")

# ---- find behaviour: honour host paths for libs/headers --------------------
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
