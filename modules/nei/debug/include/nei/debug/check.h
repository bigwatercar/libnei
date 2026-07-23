#pragma once
#ifndef NEI_DEBUG_CHECK_H
#define NEI_DEBUG_CHECK_H

#include <nei/log/log.h>
#include <stdio.h>
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
    fprintf(stderr, "[FATAL:%s:%d] %s failed: (%s)\n", __FILE__, __LINE__, kind, expr_text);                           \
    NEI_LOG_FATAL("%s failed: (%s)", kind, expr_text);                                                               \
    nei_log_flush();                                                                                                   \
    abort();                                                                                                           \
  } while (0)

#define NEI_INTERNAL_CHECK_FAIL_MSG(kind, expr_text, msg)                                                              \
  do {                                                                                                                 \
    fprintf(stderr, "[FATAL:%s:%d] %s failed: (%s)  --  %s\n", __FILE__, __LINE__, kind, expr_text, msg);                 \
    NEI_LOG_FATAL("%s failed: (%s)  --  %s", kind, expr_text, msg);                                                    \
    nei_log_flush();                                                                                                   \
    abort();                                                                                                           \
  } while (0)

#define CHECK(condition)                                                                                                \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      NEI_INTERNAL_CHECK_FAIL("CHECK", #condition);                                                                  \
    }                                                                                                                  \
  } while (0)

#define CHECK_MSG(condition, msg)                                                                                      \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      NEI_INTERNAL_CHECK_FAIL_MSG("CHECK", #condition, msg);                                                         \
    }                                                                                                                  \
  } while (0)

#define CHECK_OP(kind, op, lhs, rhs)                                                                                   \
  do {                                                                                                                 \
    if (!((lhs) op (rhs))) {                                                                                           \
      NEI_INTERNAL_CHECK_FAIL(kind, #lhs " " #op " " #rhs);                                                        \
    }                                                                                                                  \
  } while (0)

#define CHECK_OP_MSG(kind, op, lhs, rhs, msg)                                                                          \
  do {                                                                                                                 \
    if (!((lhs) op (rhs))) {                                                                                           \
      NEI_INTERNAL_CHECK_FAIL_MSG(kind, #lhs " " #op " " #rhs, msg);                                               \
    }                                                                                                                  \
  } while (0)

#define CHECK_EQ(lhs, rhs) CHECK_OP("CHECK_EQ", ==, lhs, rhs)
#define CHECK_NE(lhs, rhs) CHECK_OP("CHECK_NE", !=, lhs, rhs)
#define CHECK_LT(lhs, rhs) CHECK_OP("CHECK_LT", <, lhs, rhs)
#define CHECK_LE(lhs, rhs) CHECK_OP("CHECK_LE", <=, lhs, rhs)
#define CHECK_GT(lhs, rhs) CHECK_OP("CHECK_GT", >, lhs, rhs)
#define CHECK_GE(lhs, rhs) CHECK_OP("CHECK_GE", >=, lhs, rhs)

#define CHECK_EQ_MSG(lhs, rhs, msg) CHECK_OP_MSG("CHECK_EQ", ==, lhs, rhs, msg)
#define CHECK_NE_MSG(lhs, rhs, msg) CHECK_OP_MSG("CHECK_NE", !=, lhs, rhs, msg)
#define CHECK_LT_MSG(lhs, rhs, msg) CHECK_OP_MSG("CHECK_LT", <, lhs, rhs, msg)
#define CHECK_LE_MSG(lhs, rhs, msg) CHECK_OP_MSG("CHECK_LE", <=, lhs, rhs, msg)
#define CHECK_GT_MSG(lhs, rhs, msg) CHECK_OP_MSG("CHECK_GT", >, lhs, rhs, msg)
#define CHECK_GE_MSG(lhs, rhs, msg) CHECK_OP_MSG("CHECK_GE", >=, lhs, rhs, msg)

#if NEI_DCHECK_IS_ON
#define DCHECK(condition) CHECK(condition)
#define DCHECK_EQ(lhs, rhs) CHECK_EQ(lhs, rhs)
#define DCHECK_NE(lhs, rhs) CHECK_NE(lhs, rhs)
#define DCHECK_LT(lhs, rhs) CHECK_LT(lhs, rhs)
#define DCHECK_LE(lhs, rhs) CHECK_LE(lhs, rhs)
#define DCHECK_GT(lhs, rhs) CHECK_GT(lhs, rhs)
#define DCHECK_GE(lhs, rhs) CHECK_GE(lhs, rhs)

#define DCHECK_MSG(condition, msg) CHECK_MSG(condition, msg)
#define DCHECK_EQ_MSG(lhs, rhs, msg) CHECK_EQ_MSG(lhs, rhs, msg)
#define DCHECK_NE_MSG(lhs, rhs, msg) CHECK_NE_MSG(lhs, rhs, msg)
#define DCHECK_LT_MSG(lhs, rhs, msg) CHECK_LT_MSG(lhs, rhs, msg)
#define DCHECK_LE_MSG(lhs, rhs, msg) CHECK_LE_MSG(lhs, rhs, msg)
#define DCHECK_GT_MSG(lhs, rhs, msg) CHECK_GT_MSG(lhs, rhs, msg)
#define DCHECK_GE_MSG(lhs, rhs, msg) CHECK_GE_MSG(lhs, rhs, msg)
#else
#define DCHECK(condition) ((void)0)
#define DCHECK_EQ(lhs, rhs) ((void)0)
#define DCHECK_NE(lhs, rhs) ((void)0)
#define DCHECK_LT(lhs, rhs) ((void)0)
#define DCHECK_LE(lhs, rhs) ((void)0)
#define DCHECK_GT(lhs, rhs) ((void)0)
#define DCHECK_GE(lhs, rhs) ((void)0)

#define DCHECK_MSG(condition, msg) ((void)0)
#define DCHECK_EQ_MSG(lhs, rhs, msg) ((void)0)
#define DCHECK_NE_MSG(lhs, rhs, msg) ((void)0)
#define DCHECK_LT_MSG(lhs, rhs, msg) ((void)0)
#define DCHECK_LE_MSG(lhs, rhs, msg) ((void)0)
#define DCHECK_GT_MSG(lhs, rhs, msg) ((void)0)
#define DCHECK_GE_MSG(lhs, rhs, msg) ((void)0)
#endif

#else
#define CHECK(condition) ((void)0)
#define CHECK_EQ(lhs, rhs) ((void)0)
#define CHECK_NE(lhs, rhs) ((void)0)
#define CHECK_LT(lhs, rhs) ((void)0)
#define CHECK_LE(lhs, rhs) ((void)0)
#define CHECK_GT(lhs, rhs) ((void)0)
#define CHECK_GE(lhs, rhs) ((void)0)

#define CHECK_MSG(condition, msg) ((void)0)
#define CHECK_EQ_MSG(lhs, rhs, msg) ((void)0)
#define CHECK_NE_MSG(lhs, rhs, msg) ((void)0)
#define CHECK_LT_MSG(lhs, rhs, msg) ((void)0)
#define CHECK_LE_MSG(lhs, rhs, msg) ((void)0)
#define CHECK_GT_MSG(lhs, rhs, msg) ((void)0)
#define CHECK_GE_MSG(lhs, rhs, msg) ((void)0)

#define DCHECK(condition) ((void)0)
#define DCHECK_EQ(lhs, rhs) ((void)0)
#define DCHECK_NE(lhs, rhs) ((void)0)
#define DCHECK_LT(lhs, rhs) ((void)0)
#define DCHECK_LE(lhs, rhs) ((void)0)
#define DCHECK_GT(lhs, rhs) ((void)0)
#define DCHECK_GE(lhs, rhs) ((void)0)

#define DCHECK_MSG(condition, msg) ((void)0)
#define DCHECK_EQ_MSG(lhs, rhs, msg) ((void)0)
#define DCHECK_NE_MSG(lhs, rhs, msg) ((void)0)
#define DCHECK_LT_MSG(lhs, rhs, msg) ((void)0)
#define DCHECK_LE_MSG(lhs, rhs, msg) ((void)0)
#define DCHECK_GT_MSG(lhs, rhs, msg) ((void)0)
#define DCHECK_GE_MSG(lhs, rhs, msg) ((void)0)
#endif

#define NOTREACHED() CHECK(false)
#define NOTREACHED_MSG(msg) CHECK_MSG(false, msg)

#endif // NEI_DEBUG_CHECK_H
