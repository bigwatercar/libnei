#include <neixx/net/ssl_context.h>

#include <cstring>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <nei/log/log.h>

namespace nei::net {

// =============================================================================
// SSLContext::Impl
// =============================================================================

class SSLContext::Impl {
public:
  explicit Impl(SSLContext::Mode mode)
      : mode_(mode)
      , peer_verify_(mode == SSLContext::Mode::Client ? PeerVerify::kRequired : PeerVerify::kNone) {
    mbedtls_ssl_config_init(&config_);
    mbedtls_x509_crt_init(&server_cert_);
    mbedtls_pk_init(&private_key_);
    mbedtls_ctr_drbg_init(&drbg_);
    mbedtls_entropy_init(&entropy_);
    mbedtls_x509_crt_init(&ca_certs_);

    const unsigned char *custom = nullptr;
    mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_, custom, 0);

    int endpoint = (mode == SSLContext::Mode::Server) ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT;
    mbedtls_ssl_config_defaults(&config_, endpoint, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    ApplyAuthMode();
    mbedtls_ssl_conf_rng(&config_, mbedtls_ctr_drbg_random, &drbg_);
  }

  ~Impl() {
    mbedtls_ssl_config_free(&config_);
    mbedtls_x509_crt_free(&server_cert_);
    mbedtls_pk_free(&private_key_);
    mbedtls_ctr_drbg_free(&drbg_);
    mbedtls_entropy_free(&entropy_);
    mbedtls_x509_crt_free(&ca_certs_);
  }

  // ---- Certificate management ----

  bool SetCertificate(const std::string &cert_pem, const std::string &key_pem) {
    int ret = mbedtls_x509_crt_parse(
        &server_cert_, reinterpret_cast<const unsigned char *>(cert_pem.data()), cert_pem.size());
    if (ret != 0) {
      char buf[128];
      mbedtls_strerror(ret, buf, sizeof(buf));
      NEI_LOG(NEI_L_ERROR, "[SSLContext] cert parse: %s", buf);
      return false;
    }

    const unsigned char *pwd = nullptr;
    ret = mbedtls_pk_parse_key(&private_key_,
                               reinterpret_cast<const unsigned char *>(key_pem.data()),
                               key_pem.size(),
                               pwd,
                               0,
                               mbedtls_ctr_drbg_random,
                               &drbg_);
    if (ret != 0) {
      char buf[128];
      mbedtls_strerror(ret, buf, sizeof(buf));
      NEI_LOG(NEI_L_ERROR, "[SSLContext] key parse: %s", buf);
      return false;
    }

    ret = mbedtls_ssl_conf_own_cert(&config_, &server_cert_, &private_key_);
    if (ret != 0) {
      char buf[128];
      mbedtls_strerror(ret, buf, sizeof(buf));
      NEI_LOG(NEI_L_ERROR, "[SSLContext] conf_own_cert: %s", buf);
      return false;
    }
    return true;
  }

  bool SetCAChain(const std::string &ca_pem) {
    int ret = mbedtls_x509_crt_parse(&ca_certs_, reinterpret_cast<const unsigned char *>(ca_pem.data()), ca_pem.size());
    if (ret != 0)
      return false;
    mbedtls_ssl_conf_ca_chain(&config_, &ca_certs_, nullptr);
    return true;
  }

  // ---- Peer verification ----

  void SetPeerVerify(PeerVerify mode) {
    peer_verify_ = mode;
    ApplyAuthMode();
  }

  void SetHostname(const std::string &hostname) {
    hostname_ = hostname;
  }

  void SetAlpnProtocols(const std::vector<std::string> &protocols) {
    alpn_strings_ = protocols;
    alpn_ptrs_.clear();
    for (const auto &p : alpn_strings_)
      alpn_ptrs_.push_back(p.c_str());
    alpn_ptrs_.push_back(nullptr); // sentinel
    if (!alpn_strings_.empty()) {
      mbedtls_ssl_conf_alpn_protocols(&config_, alpn_ptrs_.data());
    }
  }

  // ---- Accessors ----

  mbedtls_ssl_config *config() {
    return &config_;
  }

  const std::string &hostname() const {
    return hostname_;
  }

private:
  void ApplyAuthMode() {
    int authmode = MBEDTLS_SSL_VERIFY_NONE;
    switch (peer_verify_) {
    case PeerVerify::kNone:
      authmode = MBEDTLS_SSL_VERIFY_NONE;
      break;
    case PeerVerify::kOptional:
      authmode = MBEDTLS_SSL_VERIFY_OPTIONAL;
      break;
    case PeerVerify::kRequired:
      authmode = MBEDTLS_SSL_VERIFY_REQUIRED;
      break;
    }
    mbedtls_ssl_conf_authmode(&config_, authmode);
  }

  SSLContext::Mode mode_;
  PeerVerify peer_verify_;
  std::string hostname_;
  std::vector<std::string> alpn_strings_;
  std::vector<const char *> alpn_ptrs_;
  mbedtls_ssl_config config_;
  mbedtls_x509_crt server_cert_;
  mbedtls_pk_context private_key_;
  mbedtls_ctr_drbg_context drbg_;
  mbedtls_entropy_context entropy_;
  mbedtls_x509_crt ca_certs_;
};

// =============================================================================
// Public shell
// =============================================================================

SSLContext::SSLContext(Mode mode)
    : impl_(std::make_unique<Impl>(mode)) {
}

SSLContext::~SSLContext() {
}

bool SSLContext::SetCertificate(const std::string &cert_pem, const std::string &key_pem) {
  return impl_->SetCertificate(cert_pem, key_pem);
}

bool SSLContext::SetCAChain(const std::string &ca_pem) {
  return impl_->SetCAChain(ca_pem);
}

void SSLContext::SetPeerVerify(PeerVerify mode) {
  impl_->SetPeerVerify(mode);
}

void SSLContext::SetHostname(const std::string &hostname) {
  impl_->SetHostname(hostname);
}

void SSLContext::SetAlpnProtocols(const std::vector<std::string> &protocols) {
  impl_->SetAlpnProtocols(protocols);
}

mbedtls_ssl_config *SSLContext::config() {
  return impl_->config();
}

const std::string &SSLContext::hostname() const {
  return impl_->hostname();
}

} // namespace nei::net
