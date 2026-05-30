// clang_cl_obs_compat.h
//
// Force-included into every TU on Windows when building with -T ClangCL.
// OBS's libobs/util/util_uint64.h calls _udiv128 / _umul128 under
// (defined(_MSC_VER) && defined(_M_X64) && _MSC_VER >= 1920). clang-cl
// matches all three feature checks but doesn't actually expose those
// intrinsics, so the include fails to compile.
//
// Provide trivial __int128-backed replacements as macros so the OBS header
// expands to something clang can codegen. cl.exe wouldn't see this header
// because it gets compiled by clang-cl only.

#pragma once

#if defined(__clang__) && defined(_MSC_VER) && defined(_M_X64)

#include <stdint.h>

static inline unsigned __int64 _obsrive_umul128(unsigned __int64 a,
                                                unsigned __int64 b,
                                                unsigned __int64* hi)
{
    unsigned __int128 r = (unsigned __int128)a * (unsigned __int128)b;
    *hi = (unsigned __int64)(r >> 64);
    return (unsigned __int64)r;
}

static inline unsigned __int64 _obsrive_udiv128(unsigned __int64 high,
                                                unsigned __int64 low,
                                                unsigned __int64 divisor,
                                                unsigned __int64* rem)
{
    unsigned __int128 dividend = ((unsigned __int128)high << 64) | low;
    *rem = (unsigned __int64)(dividend % divisor);
    return (unsigned __int64)(dividend / divisor);
}

#define _umul128 _obsrive_umul128
#define _udiv128 _obsrive_udiv128

#endif
