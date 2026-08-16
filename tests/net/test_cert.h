#pragma once

#ifndef NEI_TESTS_NET_TEST_CERT_H_
#define NEI_TESTS_NET_TEST_CERT_H_

// =============================================================================
// Shared test certificate helper — generates a self-signed RSA-2048 cert +
// private key (PEM) for TLS tests.  Header-only; kept in an anonymous helper
// namespace at each use site via macro-style inclusion is NOT done — callers
// include this header and use nei::test_cert::Generate().
// =============================================================================

#include <string>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include "mbedtls_threading.h"

namespace nei {
namespace test_cert {

struct Cert {
  std::string cert_pem;
  std::string key_pem;
};

// Generate a fresh self-signed certificate.  Returns empty strings on
// failure.  Slow (RSA keygen) — call once per test suite.
inline Cert Generate() {
  // The test binary links its own copy of Mbed TLS (the symbols are not
  // exported from libnei.so / nei.dll), so register the threading callbacks
  // for that copy before any direct Mbed TLS call here.
  nei::net::internal::EnsureMbedtlsThreading();

  mbedtls_pk_context key;
  mbedtls_x509write_cert crt;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_context entropy;
  mbedtls_mpi serial;

  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&crt);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_entropy_init(&entropy);
  mbedtls_mpi_init(&serial);

  mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);

  const char *subject = "CN=libnei-test,O=NEI,C=CN";

  Cert out;
  int ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
  if (ret != 0)
    goto done;
  ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random, &drbg, 2048, 65537);
  if (ret != 0)
    goto done;

  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_mpi_lset(&serial, 1);

  mbedtls_x509write_crt_set_subject_name(&crt, subject);
  mbedtls_x509write_crt_set_issuer_name(&crt, subject);
  mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20350101000000");
  mbedtls_x509write_crt_set_subject_key(&crt, &key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key);
  mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);

  unsigned char der_buf[4096];
  ret = mbedtls_x509write_crt_der(&crt, der_buf, sizeof(der_buf), mbedtls_ctr_drbg_random, &drbg);
  if (ret <= 0)
    goto done;

  {
    unsigned char pem[8192];
    size_t olen = 0;
    mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n",
                             "-----END CERTIFICATE-----\n",
                             der_buf + sizeof(der_buf) - ret,
                             static_cast<size_t>(ret),
                             pem,
                             sizeof(pem),
                             &olen);
    out.cert_pem.assign(reinterpret_cast<char *>(pem), olen);
  }

  {
    unsigned char key_der[4096];
    int key_len = mbedtls_pk_write_key_der(&key, key_der, sizeof(key_der));
    if (key_len <= 0)
      goto done;
    unsigned char pem[8192];
    size_t olen = 0;
    mbedtls_pem_write_buffer("-----BEGIN RSA PRIVATE KEY-----\n",
                             "-----END RSA PRIVATE KEY-----\n",
                             key_der + sizeof(key_der) - key_len,
                             static_cast<size_t>(key_len),
                             pem,
                             sizeof(pem),
                             &olen);
    out.key_pem.assign(reinterpret_cast<char *>(pem), olen);
  }

done:
  mbedtls_pk_free(&key);
  mbedtls_x509write_crt_free(&crt);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  mbedtls_mpi_free(&serial);
  return out;
}

} // namespace test_cert
} // namespace nei

#endif // NEI_TESTS_NET_TEST_CERT_H_
