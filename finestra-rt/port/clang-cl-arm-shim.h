/*
 * clang-cl-arm-shim.h -- supplies the MSVC-proprietary ARM32 intrinsics that clang-cl
 * does not declare. Force-included into every translation unit by the toolchain file's
 * /FI, ahead of the STL headers. Active only for clang-cl targeting 32-bit ARM; a no-op
 * for every other compiler and architecture.
 *
 * Background: the MSVC STL's <bit> implementation (__msvc_bit_utils.hpp) uses
 * _CountLeadingZeros / _CountLeadingZeros64 -- the ARM CLZ intrinsics -- inside its
 * _M_ARM branch, without a !__clang__ guard. clang-cl provides no such declarations,
 * so the header fails to compile. These definitions map them onto the corresponding
 * clang builtins.
 *
 * Copyright 2026 Hamed Ghorbani
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#if defined(__clang__) && defined(_M_ARM)

#ifdef __cplusplus
extern "C" {
#endif

/* Each definition stands down when the compiler has the intrinsic built in: the rt2.1 clang
 * (LLVM 23.1.1-rt2.1) provides all three on ARM32 and rejects a redefinition of a builtin in C++,
 * as MSVC does. Stock LLVM 18 and rt1/rt2 have none of them and keep these definitions. */
#if !__has_builtin(_CountLeadingZeros)
static __forceinline unsigned int _CountLeadingZeros(unsigned long _Val) {
    return _Val ? (unsigned int)__builtin_clz((unsigned int)_Val) : 32u;
}
#endif

#if !__has_builtin(_CountLeadingZeros64)
static __forceinline unsigned int _CountLeadingZeros64(unsigned __int64 _Val) {
    return _Val ? (unsigned int)__builtin_clzll((unsigned long long)_Val) : 64u;
}
#endif

/* winnt.h maps InterlockedAdd64 onto _InterlockedAdd64 for 32-bit ARM, without a prototype: MSVC
 * provides it as an intrinsic, clang declares it only for ARM64. OpenSSL 3.5's crypto/threads_win.c
 * (CRYPTO_atomic_add64) calls it, so the ARM32 OpenSSL build force-includes this header too.
 * Returns the NEW value, as the intrinsic does; ARMv7 does the 64-bit add with LDREXD/STREXD. */
#if !__has_builtin(_InterlockedAdd64)
static __forceinline __int64 _InterlockedAdd64(__int64 volatile *_Addend, __int64 _Value) {
    return __atomic_add_fetch(_Addend, _Value, __ATOMIC_SEQ_CST);
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* __clang__ && _M_ARM */
