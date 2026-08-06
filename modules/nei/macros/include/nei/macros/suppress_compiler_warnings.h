#pragma once

#if defined(_MSC_VER)
#define NEI_SUPPRESS_MSC_WARNING_BEGIN(code) __pragma(warning(push)) __pragma(warning(disable : code))
#define NEI_SUPPRESS_MSC_WARNING_END() __pragma(warning(pop))
#else
#define NEI_SUPPRESS_MSC_WARNING_BEGIN(code)
#define NEI_SUPPRESS_MSC_WARNING_END()
#endif

#if defined(_MSC_VER) && defined(__cplusplus)
#define NEI_SUPPRESS_MSC_WARNING_4251_BEGIN NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
#define NEI_SUPPRESS_MSC_WARNING_4251_END __pragma(warning(pop))
#else
#define NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
#define NEI_SUPPRESS_MSC_WARNING_4251_END
#endif
