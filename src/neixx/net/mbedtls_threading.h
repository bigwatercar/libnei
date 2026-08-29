// Copyright ... libnei
//
// Mbed TLS threading registration (MBEDTLS_THREADING_C + ALT).
//
// Mbed TLS keeps implicit shared global state (the global PSA RNG used by
// TLS 1.3 key exchange, PSA key slots, entropy, ...). That state is only
// guarded when MBEDTLS_THREADING_C is enabled, which the vendored config
// now does (see 3rdparty/mbedtls/include/mbedtls/mbedtls_config.h). The
// application must register mutex callbacks via mbedtls_threading_set_alt()
// on every copy of Mbed TLS before any of its functions run.
//
// Mbed TLS is linked into more than one binary here:
//   * libnei (libnei.so / nei.dll) — used by SSLContext / TLSClientSocket;
//   * the test executable (nei_tests) — used directly by test_cert.h and
//     tls_socket_test.cpp (which call mbedtls_rsa_gen_key() etc. directly).
// These copies are independent (hidden visibility on POSIX, not exported
// from the Windows DLL), so each must register its own callbacks. Because
// this helper is inline, every translation unit that includes it registers
// the callbacks for the Mbed TLS copy that unit is linked against:
//   * ssl_context.cpp calls it from a static registrar (runs at library
//     load, before any Mbed TLS call);
//   * test_cert.h / tls_socket_test.cpp call it before their first direct
//     Mbed TLS use.
#ifndef NEIXX_NET_MBEDTLS_THREADING_H_
#define NEIXX_NET_MBEDTLS_THREADING_H_

#include <mutex>

#include <mbedtls/threading.h>

namespace nei::net {
namespace internal {

// Registers std::mutex based Mbed TLS threading callbacks exactly once per
// binary/DSO copy. The first call performs the registration; subsequent calls
// are cheap no-ops (C++ guarantees thread-safe initialization of the local
// static). Safe to call before any other Mbed TLS function.
inline void EnsureMbedtlsThreading() {
  static const bool kRegistered = [] {
    mbedtls_threading_set_alt(
        // mutex_init: allocate the underlying std::mutex.
        [](mbedtls_threading_mutex_t *mutex) { mutex->mutex = new std::mutex; },
        // mutex_free: destroy it and leave the slot invalid.
        [](mbedtls_threading_mutex_t *mutex) {
          delete static_cast<std::mutex *>(mutex->mutex);
          mutex->mutex = nullptr;
        },
        // mutex_lock.
        [](mbedtls_threading_mutex_t *mutex) {
          static_cast<std::mutex *>(mutex->mutex)->lock();
          return 0;
        },
        // mutex_unlock.
        [](mbedtls_threading_mutex_t *mutex) {
          static_cast<std::mutex *>(mutex->mutex)->unlock();
          return 0;
        });
    return true;
  }();
  (void)kRegistered; // Registration happens in the initializer above.
}

} // namespace internal
} // namespace nei::net

#endif // NEIXX_NET_MBEDTLS_THREADING_H_
