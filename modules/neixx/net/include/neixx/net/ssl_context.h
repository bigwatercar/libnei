#pragma once

#ifndef NEIXX_NET_SSL_CONTEXT_H_
#define NEIXX_NET_SSL_CONTEXT_H_

#include <memory>
#include <string>
#include <vector>

#include <nei/build/nei_export.h>

// mbedTLS types hidden via PIMPL.
struct mbedtls_ssl_config;

// MSVC warns about std::unique_ptr<incomplete-type> in DLL interfaces.
// The Impl destructor is defined in ssl_context.cpp where the type is complete.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace nei::net {

// =============================================================================
// PeerVerify — controls peer certificate verification strictness
// =============================================================================
enum class PeerVerify {
  kNone,     // No peer certificate verification.
  kOptional, // Verify the certificate chain but allow the handshake to
             // continue even if verification fails.  Useful for self-signed
             // certificates in testing or internal networks.
  kRequired, // Require a valid certificate; handshake fails if the peer
             // certificate cannot be verified.  Requires a CA chain
             // (SetCAChain) and, for TLS 1.3, a hostname (SetHostname).
};

// =============================================================================
// SSLContext — shared TLS configuration for client and server sockets
// =============================================================================
//
// SSLContext holds the TLS version, cipher suite policy, server certificate
// (cert chain + private key), and optional CA trust store.  It is a
// heavyweight object intended to be created once and shared across many
// TLSClientSocket / TLSServerSocket instances.
//
// The TLS sockets reference the context's internal DRBG / certificates / CA
// chain, so the CALLER must guarantee the context outlives every socket that
// uses it — including every connection accepted by a TLSServerSocket.  This
// is a plain value object (NOT refcounted): keep it on the stack or in a
// member while the sockets are alive.
//
// Usage:
//   // Server:
//   SSLContext ctx(SSLContext::Mode::Server);
//   ctx.SetCertificate(cert_pem, key_pem);
//
//   // Client (verify server cert):
//   SSLContext ctx(SSLContext::Mode::Client);
//   ctx.SetCAChain(ca_pem);
//
// Thread-safety: SSLContext is NOT thread-safe.  The caller must ensure
// exclusive access during configuration.  Once configured, the underlying
// mbedtls_ssl_config may be shared read-only across connections.
//
class NEI_API SSLContext {
public:
  enum class Mode {
    Server, // Authenticates with certificate + private key.
    Client, // Verifies server certificate (requires CA chain).
  };

  explicit SSLContext(Mode mode);
  ~SSLContext();

  SSLContext(const SSLContext &) = delete;
  SSLContext &operator=(const SSLContext &) = delete;

  // ---------------------------------------------------------------------------
  // Certificate & key management
  // ---------------------------------------------------------------------------

  // Loads a PEM-encoded X.509 certificate chain and private key.
  // Required for Mode::Server, optional for Mode::Client (mutual TLS).
  bool SetCertificate(const std::string &cert_pem, const std::string &key_pem);

  // Loads a PEM-encoded CA certificate (or chain) for verifying the
  // peer's certificate.  Required for Mode::Client (to verify the
  // server), optional for Mode::Server (mutual TLS client auth).
  bool SetCAChain(const std::string &ca_pem);

  // ---------------------------------------------------------------------------
  // Peer verification
  // ---------------------------------------------------------------------------

  // Controls how strictly the peer's certificate is verified.
  // Default: kRequired for Client, kNone for Server.
  void SetPeerVerify(PeerVerify mode);

  // Sets the expected hostname for the peer's certificate (SNI + hostname
  // verification).  Required when PeerVerify::kRequired is used with
  // TLS 1.3.  Ignored for Mode::Server.
  void SetHostname(const std::string &hostname);

  // ---------------------------------------------------------------------------
  // ALPN (Application-Layer Protocol Negotiation)
  // ---------------------------------------------------------------------------

  // Sets the list of supported ALPN protocols in client preference order.
  // Each string must be a single protocol ID (e.g. "h2", "http/1.1").
  // The negotiated protocol is available post-handshake via the
  // TLSClientSocket.
  void SetAlpnProtocols(const std::vector<std::string> &protocols);

  // Returns the configured ALPN protocol list (empty if never set).
  const std::vector<std::string> &alpn_protocols() const;

  // ---------------------------------------------------------------------------
  // Internal accessors — for TLSClientSocket / TLSServerSocket Impl
  // ---------------------------------------------------------------------------
  mbedtls_ssl_config *config();
  const std::string &hostname() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nei::net

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // NEIXX_NET_SSL_CONTEXT_H_
