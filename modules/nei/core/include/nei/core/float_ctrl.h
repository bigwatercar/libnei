#pragma once
#ifndef NEI_CORE_FLOAT_CTRL_H
#define NEI_CORE_FLOAT_CTRL_H

#include <nei/build/nei_export.h>

#include <fenv.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque structure to save and restore the full floating-point
 *        environment, including platform-specific control registers.
 *
 * On x86/x64 this additionally saves the SSE MXCSR register.
 * On ARM64 this additionally saves the FPCR register.
 * On other architectures it degrades to a plain fenv_t.
 */
typedef struct nei_float_ctrl_env_st {
  fenv_t fenv;
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  unsigned int mxcsr;
#elif defined(__aarch64__) || defined(_M_ARM64)
  uint64_t fpcr;
#endif
} nei_float_ctrl_env_st;

/**
 * @brief Configure the floating-point environment for scientific computing.
 *
 * This single-call convenience function sets:
 * - Rounding mode to nearest-even (FE_TONEAREST).
 * - Enables FE_DIVBYZERO, FE_OVERFLOW, and FE_INVALID exceptions so that
 *   serious numerical problems are surfaced.
 * - Masks (disables) FE_UNDERFLOW and FE_INEXACT, which are too noisy for
 *   typical iterative scientific workloads.
 * - On x86/x64: sets SSE FTZ+DAZ (flush-to-zero / denormals-are-zero) for
 *   performance, and switches the x87 FPU to extended (64-bit mantissa)
 *   precision to minimise intermediate-rounding artefacts.
 * - On ARM64: sets the FPCR FZ (flush-to-zero) bit for subnormal performance.
 *
 * @note This is a best-effort function; individual failures are not reported.
 *       Use the granular functions below if you need error returns.
 */
NEI_API void nei_float_ctrl_enable_scientific(void);

/**
 * @brief Set the current rounding direction.
 * @param round One of FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO.
 * @return 0 on success, non-zero on failure.
 */
NEI_API int nei_float_ctrl_set_round(int round);

/**
 * @brief Enable (unmask) the given floating-point exception flags.
 * @param excepts Bitwise OR of FE_DIVBYZERO, FE_INVALID, FE_OVERFLOW,
 *                FE_UNDERFLOW, FE_INEXACT.
 * @return 0 on success, -1 on failure.
 */
NEI_API int nei_float_ctrl_enable_exceptions(int excepts);

/**
 * @brief Disable (mask) the given floating-point exception flags.
 * @param excepts Bitwise OR of FE_DIVBYZERO, FE_INVALID, FE_OVERFLOW,
 *                FE_UNDERFLOW, FE_INEXACT.
 * @return 0 on success, -1 on failure.
 */
NEI_API int nei_float_ctrl_disable_exceptions(int excepts);

/**
 * @brief Save the complete floating-point environment.
 *
 * Captures the C99 fenv_t plus, where applicable, the SSE MXCSR (x86) or
 * FPCR (ARM64).  The saved state can later be restored with
 * nei_float_ctrl_restore().
 *
 * @param env Pointer to caller-allocated storage. Must not be NULL.
 */
NEI_API void nei_float_ctrl_save(nei_float_ctrl_env_st *env);

/**
 * @brief Restore a floating-point environment previously saved with
 *        nei_float_ctrl_save().
 * @param env Pointer to a previously-saved environment. Must not be NULL.
 */
NEI_API void nei_float_ctrl_restore(const nei_float_ctrl_env_st *env);

#ifdef __cplusplus
}
#endif

#endif /* NEI_CORE_FLOAT_CTRL_H */
