/**
 * \file threading_alt.h
 *
 * \brief Alternate threading implementation type for Mbed TLS
 *        (MBEDTLS_THREADING_ALT).
 *
 * Mbed TLS is a C library; the mutex type must be defined in a C-compatible
 * header. Here the mutex is an opaque pointer to a C++ std::mutex that is
 * allocated by the callbacks registered via mbedtls_threading_set_alt() (see
 * libnei net/ssl_context.cpp). Keeping the field opaque keeps this header
 * compilable by both the Mbed TLS C sources and libnei's C++ code, and it
 * guarantees a consistent mbedtls_threading_mutex_t definition across every
 * translation unit.
 */
#ifndef MBEDTLS_THREADING_ALT_H
#define MBEDTLS_THREADING_ALT_H

typedef struct mbedtls_threading_mutex_t {
    /* Opaque pointer to a std::mutex (owned by the callback layer). */
    void *mutex;
} mbedtls_threading_mutex_t;

#endif /* MBEDTLS_THREADING_ALT_H */
