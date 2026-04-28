#pragma once
#ifndef NEI_DEBUG_CHECK_H
#define NEI_DEBUG_CHECK_H

#include <nei/log/log.h>
#include <stdlib.h>

#if !defined(NEI_CHROMIUM_LIKE_CHECK)
#define NEI_CHROMIUM_LIKE_CHECK 1
#endif

#if !defined(NEI_DCHECK_IS_ON)
#if defined(NDEBUG)
#define NEI_DCHECK_IS_ON 0
#else
#define NEI_DCHECK_IS_ON 1
#endif
#endif

#if NEI_CHROMIUM_LIKE_CHECK
#define NEI_INTERNAL_CHECK_FAIL(kind, expr_text)                                                                       \
  do {                                                                                                                 \
    NEI_LOG_FATAL("%s failed: (%s)", kind, expr_text);                                                               \
    nei_log_flush();                                                                                                   \
    abort();                                                                                                           \
  } while (0)

#define CHECK(condition)                                                                                                \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      NEI_INTERNAL_CHECK_FAIL("CHECK", #condition);                                                                  \
    }                                                                                                                  \
  } while (0)

#define CHECK_OP(kind, op, lhs, rhs)                                                                                   \
  do {                                                                                                                 \
    if (!((lhs) op (rhs))) {                                                                                           \
      NEI_INTERNAL_CHECK_FAIL(kind, #lhs " " #op " " #rhs);                                                        \
    }                                                                                                                  \
  } while (0)

#define CHECK_EQ(lhs, rhs) CHECK_OP("CHECK_EQ", ==, lhs, rhs)
#define CHECK_NE(lhs, rhs) CHECK_OP("CHECK_NE", !=, lhs, rhs)
#define CHECK_LT(lhs, rhs) CHECK_OP("CHECK_LT", <, lhs, rhs)
#define CHECK_LE(lhs, rhs) CHECK_OP("CHECK_LE", <=, lhs, rhs)
#define CHECK_GT(lhs, rhs) CHECK_OP("CHECK_GT", >, lhs, rhs)
#define CHECK_GE(lhs, rhs) CHECK_OP("CHECK_GE", >=, lhs, rhs)

#if NEI_DCHECK_IS_ON
#define DCHECK(condition) CHECK(condition)
#define DCHECK_EQ(lhs, rhs) CHECK_EQ(lhs, rhs)
#define DCHECK_NE(lhs, rhs) CHECK_NE(lhs, rhs)
#define DCHECK_LT(lhs, rhs) CHECK_LT(lhs, rhs)
#define DCHECK_LE(lhs, rhs) CHECK_LE(lhs, rhs)
#define DCHECK_GT(lhs, rhs) CHECK_GT(lhs, rhs)
#define DCHECK_GE(lhs, rhs) CHECK_GE(lhs, rhs)
#else
#define DCHECK(condition) ((void)0)
#define DCHECK_EQ(lhs, rhs) ((void)0)
#define DCHECK_NE(lhs, rhs) ((void)0)
#define DCHECK_LT(lhs, rhs) ((void)0)
#define DCHECK_LE(lhs, rhs) ((void)0)
#define DCHECK_GT(lhs, rhs) ((void)0)
#define DCHECK_GE(lhs, rhs) ((void)0)
#endif

#else
#define CHECK(condition) ((void)0)
#define CHECK_EQ(lhs, rhs) ((void)0)
#define CHECK_NE(lhs, rhs) ((void)0)
#define CHECK_LT(lhs, rhs) ((void)0)
#define CHECK_LE(lhs, rhs) ((void)0)
#define CHECK_GT(lhs, rhs) ((void)0)
#define CHECK_GE(lhs, rhs) ((void)0)

#define DCHECK(condition) ((void)0)
#define DCHECK_EQ(lhs, rhs) ((void)0)
#define DCHECK_NE(lhs, rhs) ((void)0)
#define DCHECK_LT(lhs, rhs) ((void)0)
#define DCHECK_LE(lhs, rhs) ((void)0)
#define DCHECK_GT(lhs, rhs) ((void)0)
#define DCHECK_GE(lhs, rhs) ((void)0)
#endif

#endif // NEI_DEBUG_CHECK_H
