/* _GNU_SOURCE is required on Linux for feenableexcept / fedisableexcept. */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif

#include "nei/core/float_ctrl.h"

/* ------------------------------------------------------------------ */
/*  Standard / platform headers                                       */
/* ------------------------------------------------------------------ */

#include <fenv.h>

#if defined(_WIN32)
#include <float.h>
#else
#include <fpu_control.h>
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

/* ------------------------------------------------------------------ */
/*  Internal helpers – Windows exception-mask translation              */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)

static unsigned int nei_map_excepts_to_em(int excepts)
{
    unsigned int em = 0;
    if (excepts & FE_INVALID)   em |= _EM_INVALID;
    if (excepts & FE_DIVBYZERO) em |= _EM_ZERODIVIDE;
    if (excepts & FE_OVERFLOW)  em |= _EM_OVERFLOW;
    if (excepts & FE_UNDERFLOW) em |= _EM_UNDERFLOW;
    if (excepts & FE_INEXACT)   em |= _EM_INEXACT;
    return em;
}

static unsigned int nei_map_round_to_rc(int round)
{
    switch (round) {
    case FE_TONEAREST:  return _RC_NEAR;
    case FE_DOWNWARD:   return _RC_DOWN;
    case FE_UPWARD:     return _RC_UP;
    case FE_TOWARDZERO: return _RC_CHOP;
    default:            return (unsigned int)-1;
    }
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/*  nei_float_ctrl_set_round                                          */
/* ------------------------------------------------------------------ */

int nei_float_ctrl_set_round(int round)
{
#if defined(_WIN32)
    unsigned int rc = nei_map_round_to_rc(round);
    if (rc == (unsigned int)-1)
        return -1;

    unsigned int prev = 0;
    errno_t err = _controlfp_s(&prev, rc, _MCW_RC);
    return (err == 0) ? 0 : -1;
#else
    return fesetround(round);
#endif
}

/* ------------------------------------------------------------------ */
/*  nei_float_ctrl_enable_exceptions                                   */
/* ------------------------------------------------------------------ */

int nei_float_ctrl_enable_exceptions(int excepts)
{
#if defined(_WIN32)
    unsigned int em = nei_map_excepts_to_em(excepts);
    unsigned int ctrl = 0;
    errno_t err = _controlfp_s(&ctrl, 0, 0);
    if (err != 0)
        return -1;
    /* _EM_* bits are mask bits: clear them to *enable* the exception. */
    err = _controlfp_s(&ctrl, ctrl & ~em, _MCW_EM);
    return (err == 0) ? 0 : -1;
#else
    if (feenableexcept(excepts) == -1)
        return -1;
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/*  nei_float_ctrl_disable_exceptions                                  */
/* ------------------------------------------------------------------ */

int nei_float_ctrl_disable_exceptions(int excepts)
{
#if defined(_WIN32)
    unsigned int em = nei_map_excepts_to_em(excepts);
    unsigned int ctrl = 0;
    errno_t err = _controlfp_s(&ctrl, 0, 0);
    if (err != 0)
        return -1;
    /* _EM_* bits are mask bits: set them to *disable* the exception.  */
    err = _controlfp_s(&ctrl, ctrl | em, _MCW_EM);
    return (err == 0) ? 0 : -1;
#else
    if (fedisableexcept(excepts) == -1)
        return -1;
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/*  nei_float_ctrl_enable_scientific                                   */
/* ------------------------------------------------------------------ */

void nei_float_ctrl_enable_scientific(void)
{
    /* --- 1. Rounding: nearest-even (all platforms) --- */
    fesetround(FE_TONEAREST);

    /* --- 2. Exception policy --- */
    nei_float_ctrl_enable_exceptions(FE_DIVBYZERO | FE_OVERFLOW | FE_INVALID);
    nei_float_ctrl_disable_exceptions(FE_UNDERFLOW | FE_INEXACT);

    /* --- 3. x86 / x64 specific --- */
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    {
        /* SSE MXCSR: FTZ + DAZ */
        unsigned int csr = _mm_getcsr();
        csr |= 0x8000u;   /* bit 15: Flush to Zero              */
        csr |= 0x0040u;   /* bit  6: Denormals are Zero          */
        _mm_setcsr(csr);

#if defined(_WIN32)
        /* x87 precision: extended (64-bit mantissa) */
        {
            unsigned int cw = 0;
            _controlfp_s(&cw, _PC_64, _MCW_PC);
        }
#else
        /* Linux / POSIX: set x87 FPU precision to extended.     */
        {
            fpu_control_t cw;
            _FPU_GETCW(cw);
            cw &= ~_FPU_EXTENDED;  /* clear precision bits       */
            cw |= _FPU_EXTENDED;   /* extended precision (64-bit) */
            _FPU_SETCW(cw);
        }
#endif
    }
#endif /* x86 / x64 */

    /* --- 4. ARM64 specific --- */
#if defined(__aarch64__) || defined(_M_ARM64)
    {
        uint64_t fpcr;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
        fpcr |= (1ULL << 24);  /* FZ: flush-to-zero (subnormals) */
        __asm__ __volatile__("msr fpcr, %0" :: "r"(fpcr));
    }
#endif /* ARM64 */
}

/* ------------------------------------------------------------------ */
/*  nei_float_ctrl_save                                                */
/* ------------------------------------------------------------------ */

void nei_float_ctrl_save(nei_float_ctrl_env_st *env)
{
    if (env == NULL)
        return;

    fegetenv(&env->fenv);

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    env->mxcsr = _mm_getcsr();
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(env->fpcr));
#endif
}

/* ------------------------------------------------------------------ */
/*  nei_float_ctrl_restore                                             */
/* ------------------------------------------------------------------ */

void nei_float_ctrl_restore(const nei_float_ctrl_env_st *env)
{
    if (env == NULL)
        return;

    fesetenv(&env->fenv);

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    _mm_setcsr(env->mxcsr);
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ __volatile__("msr fpcr, %0" :: "r"(env->fpcr));
#endif
}
