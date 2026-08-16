# Third-Party Dependencies

| Library                                          | Version | License     | Usage                                        |
| ------------------------------------------------ | ------- | ----------- | -------------------------------------------- |
| [c-ares](https://github.com/c-ares/c-ares)       | 1.34.6  | MIT         | Asynchronous DNS resolution (`neixx/net`)    |
| [llhttp](https://github.com/nodejs/llhttp)       | 9.2.1   | MIT         | HTTP/1.1 message parsing (`neixx/net/http`)  |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls)   | 3.6.3   | Apache-2.0  | TLS/SSL transport (`neixx/net`)              |

## Vendored changes

- **mbedTLS**: `MBEDTLS_DEBUG_C` disabled in
  `include/mbedtls/mbedtls_config.h` — `MBEDTLS_SSL_DEBUG_MSG` inside
  `mbedtls_ssl_free()` dereferences `ssl->conf->f_dbg`; `conf` lives in
  `SSLContext`, whose lifetime cannot be guaranteed once the asynchronous
  `TLSClientSocket` teardown runs on the IO thread (TSan heap-use-after-free).
  libnei never uses the Mbed TLS debug callbacks, so the module is not needed.

