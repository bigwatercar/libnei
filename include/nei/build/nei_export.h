#pragma once
#ifndef NEI_MACROS_NEI_EXPORT_H
#define NEI_MACROS_NEI_EXPORT_H

/*
 * Cross-platform API export/import macros for nei.
 *
 * - Static builds: NEI_API resolves to empty.
 * - Shared library build (inside nei): define NEI_EXPORTS to export symbols.
 * - Shared library consumption: symbols are imported on Windows.
 */
#if defined(__has_attribute)
#if __has_attribute(visibility)
#define NEI_HAS_VISIBILITY 1
#else
#define NEI_HAS_VISIBILITY 0
#endif
#else
#define NEI_HAS_VISIBILITY 0
#endif

#if defined(NEI_STATIC)
#define NEI_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#if defined(NEI_EXPORTS)
#define NEI_API __declspec(dllexport)
#else
#define NEI_API __declspec(dllimport)
#endif
#elif NEI_HAS_VISIBILITY || (defined(__GNUC__) && __GNUC__ >= 4)
#define NEI_API __attribute__((visibility("default")))
#else
#define NEI_API
#endif

#undef NEI_HAS_VISIBILITY

#endif // NEI_MACROS_NEI_EXPORT_H