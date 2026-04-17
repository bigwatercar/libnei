#ifndef NEI_MACROS_COMPILER_SPECIFIC_H
#define NEI_MACROS_COMPILER_SPECIFIC_H

#undef NEI_INLINE
#if defined(_MSC_VER)
#define NEI_INLINE __inline
#define NEI_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
#define NEI_INLINE __inline__
#define NEI_FORCE_INLINE inline __attribute__((always_inline))
#endif

#endif // NEI_MACROS_COMPILER_SPECIFIC_H
