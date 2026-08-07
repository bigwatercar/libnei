#pragma once

#ifndef NEI_MACROS_COMPILER_SPECIFIC_H
#define NEI_MACROS_COMPILER_SPECIFIC_H

// =============================================================================
// nei compiler-specific macros (inline, deprecation, warning suppression, ...)
// =============================================================================

// ---------------------------------------------------------------------------
// inline / force-inline (works in both C and C++)
// ---------------------------------------------------------------------------
// Example:
//   NEI_INLINE int add(int a, int b) { return a + b; }
//   NEI_FORCE_INLINE void hot_path()    { /* always inlined */ }
//
// Intel compilers are intentionally omitted: classic icc defines __GNUC__,
// LLVM-based icx defines __clang__, so no separate branch is needed.
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#define NEI_INLINE __inline
#define NEI_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define NEI_INLINE __inline__
#define NEI_FORCE_INLINE inline __attribute__((__always_inline__))
#else
#define NEI_INLINE inline
#define NEI_FORCE_INLINE inline
#endif

// ---------------------------------------------------------------------------
// MSVC warning suppression helpers
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#define NEI_SUPPRESS_MSC_WARNING_BEGIN(code) __pragma(warning(push)) __pragma(warning(disable : code))
#define NEI_SUPPRESS_MSC_WARNING_END() __pragma(warning(pop))
#else
#define NEI_SUPPRESS_MSC_WARNING_BEGIN(code)
#define NEI_SUPPRESS_MSC_WARNING_END()
#endif

#if defined(_MSC_VER) && defined(__cplusplus)
#define NEI_SUPPRESS_MSC_WARNING_4251_BEGIN NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
#define NEI_SUPPRESS_MSC_WARNING_4251_END NEI_SUPPRESS_MSC_WARNING_END()
#else
#define NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
#define NEI_SUPPRESS_MSC_WARNING_4251_END
#endif

// ---------------------------------------------------------------------------
// deprecation attribute (works in both C and C++)
// ---------------------------------------------------------------------------
// Example:
//   NEI_DEPRECATED("use bar() instead") void foo();
//   NEI_DEPRECATED("use NewType")        struct OldType {};
//
// NEI_CPP_DEPRECATED — legacy alias; prefer NEI_DEPRECATED.
// ---------------------------------------------------------------------------

// Standard [[deprecated]] (C++14 / C23)
#if (defined(__cplusplus) && (__cplusplus >= 201402L)) || \
    (defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L))
#define NEI_DEPRECATED(msg) [[deprecated(msg)]]

// MSVC (both C and C++)
#elif defined(_MSC_VER)
#define NEI_DEPRECATED(msg) __declspec(deprecated(msg))

// GCC / Clang (both C and C++)
#elif defined(__GNUC__) || defined(__clang__)
#define NEI_DEPRECATED(msg) __attribute__((__deprecated__(msg)))

#else
#define NEI_DEPRECATED(msg)
#endif

// Legacy C++-only alias (kept for backward compatibility)
#define NEI_CPP_DEPRECATED(msg) NEI_DEPRECATED(msg)

// ---------------------------------------------------------------------------
// nodiscard — warn when return value is ignored (C and C++)
// ---------------------------------------------------------------------------
// Example:
//   NEI_WARN_UNUSED_RESULT bool Open(const char *path);
//   Open("foo");  // compiler warns: return value discarded
// ---------------------------------------------------------------------------
#if (defined(__cplusplus) && (__cplusplus >= 201703L)) || \
    (defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L))
#define NEI_WARN_UNUSED_RESULT [[nodiscard]]
#elif defined(_MSC_VER)
#define NEI_WARN_UNUSED_RESULT _Check_return_
#elif defined(__GNUC__) || defined(__clang__)
#define NEI_WARN_UNUSED_RESULT __attribute__((__warn_unused_result__))
#else
#define NEI_WARN_UNUSED_RESULT
#endif

// ---------------------------------------------------------------------------
// fallthrough — suppress -Wimplicit-fallthrough (C and C++)
// ---------------------------------------------------------------------------
// Example:
//   switch (state) {
//     case A:
//       do_a();
//       NEI_FALLTHROUGH;    // intentional fall-through to case B
//     case B:
//       do_b();
//       break;
//   }
// ---------------------------------------------------------------------------
#if (defined(__cplusplus) && (__cplusplus >= 201703L))
#define NEI_FALLTHROUGH [[fallthrough]]
#elif defined(__clang__)
#define NEI_FALLTHROUGH __attribute__((__fallthrough__))
#elif defined(__GNUC__) && (__GNUC__ >= 7)
#define NEI_FALLTHROUGH __attribute__((__fallthrough__))
#elif defined(_MSC_VER)
#define NEI_FALLTHROUGH __fallthrough
#else
#define NEI_FALLTHROUGH ((void)0)
#endif

// ---------------------------------------------------------------------------
// LIKELY / UNLIKELY — branch prediction hints (C and C++)
// ---------------------------------------------------------------------------
// Example:
//   if (NEI_LIKELY(ptr != NULL))  { fast_path();   }
//   if (NEI_UNLIKELY(error))      { error_handle(); }
// ---------------------------------------------------------------------------
#if defined(__GNUC__) || defined(__clang__)
#define NEI_LIKELY(x)   __builtin_expect(!!(x), 1)
#define NEI_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define NEI_LIKELY(x)   (x)
#define NEI_UNLIKELY(x) (x)
#endif

// ---------------------------------------------------------------------------
// UNREACHABLE — tell the compiler a code path is unreachable (C and C++)
// ---------------------------------------------------------------------------
// Example:
//   enum Color { Red, Green, Blue };
//   const char *Name(Color c) {
//     switch (c) {
//       case Red:   return "Red";
//       case Green: return "Green";
//       case Blue:  return "Blue";
//     }
//     NEI_UNREACHABLE();   // all enum values handled above
//   }
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#define NEI_UNREACHABLE() __assume(0)
#elif defined(__GNUC__) || defined(__clang__)
#define NEI_UNREACHABLE() __builtin_unreachable()
#else
#define NEI_UNREACHABLE() ((void)0)
#endif

#endif // NEI_MACROS_COMPILER_SPECIFIC_H
