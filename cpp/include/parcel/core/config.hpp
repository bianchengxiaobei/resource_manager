#pragma once

#if !defined(PARCEL_HAS_EXCEPTIONS)
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define PARCEL_HAS_EXCEPTIONS 1
#else
#define PARCEL_HAS_EXCEPTIONS 0
#endif
#endif

#if !defined(PARCEL_HAS_RTTI)
#if defined(__cpp_rtti) || defined(_CPPRTTI)
#define PARCEL_HAS_RTTI 1
#else
#define PARCEL_HAS_RTTI 0
#endif
#endif

#include <cassert>
#include <cstdio>
#include <cstdlib>

#define PARCEL_ASSERT(condition) assert(condition)

#define PARCEL_VERIFY(condition)                                                                   \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ::parcel::core::detail::AssertionFailure(#condition, __FILE__, __LINE__);              \
        }                                                                                          \
    } while (false)

namespace parcel::core::detail {

[[noreturn]] inline void AssertionFailure(const char* expression, const char* file, int line) {
    std::fprintf(stderr, "Parcel assertion failed: %s\n  at %s:%d\n", expression, file, line);
    std::abort();
}

} // namespace parcel::core::detail
