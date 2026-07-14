#include <nei/core/encoding.h>

#ifdef _WIN32

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

/* Stack buffer size for intermediate wchar_t conversions. */
#define ENCODING_STACK_WBUF_SIZE 2048

/* -------------------------------------------------------------------------
 * Internal: WideCharToMultiByte with configurable code page.
 * ------------------------------------------------------------------------- */
static int wstr_to_mb_impl(UINT code_page, const wchar_t *src, int src_len,
                           char *buf, size_t size) {
    int needed = WideCharToMultiByte(code_page, 0, src, src_len,
                                     NULL, 0, NULL, NULL);
    if (needed <= 0) {
        return -1;
    }

    /* WideCharToMultiByte includes the null terminator in 'needed' only when:
     *   - src_len == -1 (null-terminated input), or
     *   - a null character is found within the first src_len characters.
     * Only subtract 1 in those cases. */
    int out_len;
    if (src_len < 0) {
        out_len = needed - 1;
    } else {
        /* Check for embedded null terminator within the explicit length. */
        size_t actual = wcsnlen(src, (size_t)src_len);
        out_len = (actual < (size_t)src_len) ? needed - 1 : needed;
    }

    if (size == 0) {
        return out_len; /* no buffer to write to, report required size */
    }
    if ((size_t)needed >= size) {
        /* Zero the buffer first: when a multi-byte character doesn't fully fit,
         * WideCharToMultiByte stops without writing to the remaining bytes,
         * which may leave buf[...] uninitialized. */
        memset(buf, 0, size);
        WideCharToMultiByte(code_page, 0, src, src_len,
                            buf, (int)(size - 1), NULL, NULL);
        buf[size - 1] = '\0';
        return out_len;
    }
    WideCharToMultiByte(code_page, 0, src, src_len,
                        buf, needed, NULL, NULL);
    buf[needed] = '\0';
    return out_len;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int nei_wstr_to_utf8(const wchar_t *src, int src_len,
                     char *buf, size_t size) {
    return wstr_to_mb_impl(CP_UTF8, src, src_len, buf, size);
}

int nei_utf8_to_wstr(const char *src, wchar_t *buf, int size) {
    int needed = MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
    if (needed <= 0) {
        return -1;
    }
    if (needed > size) {
        return -1; /* buffer too small */
    }
    MultiByteToWideChar(CP_UTF8, 0, src, -1, buf, size);
    return needed - 1; /* exclude null terminator */
}

int nei_mbcs_to_utf8(const char *src, int src_len,
                     char *buf, size_t size) {
    /* Step 1: MBCS -> wchar_t */
    int wlen = MultiByteToWideChar(CP_ACP, 0, src, src_len, NULL, 0);
    if (wlen <= 0) {
        return -1;
    }

    wchar_t wbuf_stack[ENCODING_STACK_WBUF_SIZE];
    wchar_t *wbuf = wbuf_stack;
    if (wlen > ENCODING_STACK_WBUF_SIZE) {
        wbuf = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
        if (wbuf == NULL) {
            return -1;
        }
    }

    MultiByteToWideChar(CP_ACP, 0, src, src_len, wbuf, wlen);

    /* Step 2: wchar_t -> UTF-8 */
    int w_src_len = (src_len < 0) ? -1 : wlen;
    int result = wstr_to_mb_impl(CP_UTF8, wbuf, w_src_len, buf, size);

    if (wbuf != wbuf_stack) {
        free(wbuf);
    }
    return result;
}

int nei_utf8_to_mbcs(const char *src, int src_len,
                     char *buf, size_t size) {
    /* Step 1: UTF-8 -> wchar_t */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, src, src_len, NULL, 0);
    if (wlen <= 0) {
        return -1;
    }

    wchar_t wbuf_stack[ENCODING_STACK_WBUF_SIZE];
    wchar_t *wbuf = wbuf_stack;
    if (wlen > ENCODING_STACK_WBUF_SIZE) {
        wbuf = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
        if (wbuf == NULL) {
            return -1;
        }
    }

    MultiByteToWideChar(CP_UTF8, 0, src, src_len, wbuf, wlen);

    /* Step 2: wchar_t -> MBCS */
    int w_src_len = (src_len < 0) ? -1 : wlen;
    int result = wstr_to_mb_impl(CP_ACP, wbuf, w_src_len, buf, size);

    if (wbuf != wbuf_stack) {
        free(wbuf);
    }
    return result;
}

#endif /* _WIN32 */
