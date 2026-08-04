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

static __forceinline unsigned int _CountLeadingZeros(unsigned long _Val) {
    return _Val ? (unsigned int)__builtin_clz((unsigned int)_Val) : 32u;
}

static __forceinline unsigned int _CountLeadingZeros64(unsigned __int64 _Val) {
    return _Val ? (unsigned int)__builtin_clzll((unsigned long long)_Val) : 64u;
}

#ifdef __cplusplus
}
#endif

#endif /* __clang__ && _M_ARM */
