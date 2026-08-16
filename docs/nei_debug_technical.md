# nei/debug 断言检查模块技术设计说明（C99）

## 1. 文档目标与范围

本文档描述 `nei/debug` 中 Chromium 风格 `CHECK` / `DCHECK` 断言宏的设计目标、失败处理、编译期开关与使用建议。

本文档基于：

- `modules/nei/debug/include/nei/debug/check.h`（全部宏定义）
- `modules/nei/log/log.h`（失败时同步写日志）

## 2. 模块定位

| 组件 | 定位 | 对标 Chromium |
|------|------|--------------|
| `CHECK*` | 无条件契约检查（Release 也致命） | `base::CHECK` |
| `DCHECK*` | 开发期断言（Release 编译为空） | `base::DCHECK` |

## 3. 编译期开关

```c
#define NEI_CHROMIUM_LIKE_CHECK 1        // Chromium 风格（默认）
#if !defined(NEI_DCHECK_IS_ON)
#if defined(NDEBUG) → 0   #else → 1     // DCHECK 由 NDEBUG 自动推导
#endif
#endif
```

- `NEI_CHROMIUM_LIKE_CHECK=0` 时全部宏退化为 `((void)0)`（非 Chromium 构建）
- `NEI_DCHECK_IS_ON` 由 `NDEBUG` 自动推导，亦可显式覆盖

## 4. 失败处理链路（CHECK 命中时）

```c
#define NEI_INTERNAL_CHECK_FAIL(kind, expr_text) \
  do { \
    fprintf(stderr, "[FATAL:%s:%d] %s failed: (%s)\n", __FILE__, __LINE__, kind, expr_text); \
    NEI_LOG_FATAL("%s failed: (%s)", kind, expr_text); \
    nei_log_flush();   /* ★ 先冲刷日志再 abort，保证诊断不丢 */ \
    abort(); \
  } while (0)
```

**双通道诊断**：stderr 直接输出 + 经 C 日志系统记录 FATAL——**先 `nei_log_flush()` 再 `abort()`**，避免异步日志缓冲未落盘即进程终止。

## 5. 宏清单

### 5.1 基础

| 宏 | 语义 |
|----|------|
| `CHECK(condition)` | 无条件校验，失败 fatal |
| `CHECK_MSG(condition, msg)` | 带自定义消息 |
| `DCHECK(condition)` / `DCHECK_MSG` | Debug-only（Release `((void)0)`） |

### 5.2 比较宏（OP 展开，失败打印完整表达式）

```c
#define CHECK_OP(kind, op, lhs, rhs) \
  if (!((lhs) op (rhs))) NEI_INTERNAL_CHECK_FAIL(kind, #lhs " " #op " " #rhs)

CHECK_EQ/NE/LT/LE/GT/GE          (+ _MSG 变体)
DCHECK_EQ/NE/LT/LE/GT/GE         (+ _MSG 变体，Release 编译为空)
```

> 注意：`CHECK_EQ(lhs, rhs)` 中 `lhs`/`rhs` **各求值一次**（宏展开为一次比较）；失败时打印表达式原文便于定位。

### 5.3 Release 形态

`NEI_DCHECK_IS_ON=0` 时全部 `DCHECK*` 展开为 `((void)0)`——**零指令、零表达式求值**（参数必须无副作用，与 Chromium 语义一致）。

## 6. 使用建议

- `CHECK*`：不变量/前置条件（必须始终成立，Release 也致命）
- `DCHECK*`：开发期断言/性能敏感路径（Release 零开销）
- 库内典型用法：socket 状态（`DCHECK_MSG(!closed_, ...)`）、队列不变量、线程亲和（`DCHECK_CALLED_ON_VALID_THREAD`）、条件变量返回码（`DCHECK_EQ(rv, 0)`）

## 7. 设计要点

- C 层断言，仅依赖 `stdio/stdlib` + C 日志系统
- `DCHECK` Release 编译为空，参数不求值（勿写有副作用表达式）
- 失败诊断双通道 + 先冲刷后 abort，保证崩溃现场日志完整
