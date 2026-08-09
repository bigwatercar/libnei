// =============================================================================
// TLS socket unit tests  --  async handshake, data transfer, error paths
// =============================================================================

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pem.h>
#include <mbedtls/ssl.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include <neixx/common/time.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tcp_server_socket.h>
#include <neixx/net/tls_client_socket.h>
#include <neixx/net/tls_server_socket.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/run_loop.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace nei::net {
namespace {

// =============================================================================
// Generate self-signed certificate for testing
// =============================================================================

struct TestCert {
  std::string cert_pem;
  std::string key_pem;
};

TestCert GenerateSelfSignedCert() {
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

  // Generate RSA 2048 key.
  int ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
  if (ret != 0) {
    ADD_FAILURE() << "pk_setup: " << ret;
    return {};
  }
  ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random, &drbg, 2048, 65537);
  if (ret != 0) {
    ADD_FAILURE() << "rsa_gen_key: " << ret;
    return {};
  }

  // Build self-signed cert.
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_mpi_lset(&serial, 1);

  const char *subject = "CN=libnei-test,O=NEI,C=CN";
  mbedtls_x509write_crt_set_subject_name(&crt, subject);
  mbedtls_x509write_crt_set_issuer_name(&crt, subject);
  mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20350101000000");
  mbedtls_x509write_crt_set_subject_key(&crt, &key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key);
  mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);

  unsigned char der_buf[4096];
  ret = mbedtls_x509write_crt_der(&crt, der_buf, sizeof(der_buf), mbedtls_ctr_drbg_random, &drbg);
  if (ret <= 0) {
    ADD_FAILURE() << "x509write_crt_der: " << ret;
    return {};
  }

  std::string cert_pem, key_pem;
  if (ret > 0) {
    unsigned char pem[8192];
    size_t olen = 0;
    mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n",
                             "-----END CERTIFICATE-----\n",
                             der_buf + sizeof(der_buf) - ret,
                             static_cast<size_t>(ret),
                             pem,
                             sizeof(pem),
                             &olen);
    cert_pem.assign(reinterpret_cast<char *>(pem), olen);
  }

  unsigned char key_der[4096];
  int key_len = mbedtls_pk_write_key_der(&key, key_der, sizeof(key_der));
  EXPECT_GT(key_len, 0) << "pk_write_key_der failed";
  if (key_len > 0) {
    unsigned char pem[8192];
    size_t olen = 0;
    mbedtls_pem_write_buffer("-----BEGIN RSA PRIVATE KEY-----\n",
                             "-----END RSA PRIVATE KEY-----\n",
                             key_der + sizeof(key_der) - key_len,
                             static_cast<size_t>(key_len),
                             pem,
                             sizeof(pem),
                             &olen);
    key_pem.assign(reinterpret_cast<char *>(pem), olen);
  }

  mbedtls_pk_free(&key);
  mbedtls_x509write_crt_free(&crt);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  mbedtls_mpi_free(&serial);

  return {cert_pem, key_pem};
}

// =============================================================================
// TlsSocketTest fixture
// =============================================================================

class TlsSocketTest : public testing::Test {
protected:
  void SetUp() override {
    // Generate cert once per test suite.
    static TestCert cert = GenerateSelfSignedCert();
    cert_pem_ = cert.cert_pem;
    key_pem_ = cert.key_pem;
    ASSERT_FALSE(cert_pem_.empty()) << "Cert generation produced empty PEM";
    ASSERT_FALSE(key_pem_.empty()) << "Key generation produced empty PEM";

    // Server context (fixture member — alive for the whole test).
    ASSERT_TRUE(server_ctx_.SetCertificate(cert_pem_, key_pem_));

    // Client context — trusts our self-signed CA.
    client_ctx_.SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client_ctx_.SetCAChain(cert_pem_));

    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(io_thread_.StartWithOptions(opts));
    io_runner_ = io_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);
    ASSERT_TRUE(srv_thread_.StartWithOptions(opts));
    srv_runner_ = srv_thread_.GetTaskRunner();
    ASSERT_TRUE(srv_runner_);
  }

  void TearDown() override {
    srv_thread_.Stop();
    io_thread_.Stop();
  }

  static uint16_t FindFreePort() {
#if defined(_WIN32)
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
      return 0;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(s, (struct sockaddr *)&addr, sizeof(addr));
    int len = sizeof(addr);
    ::getsockname(s, (struct sockaddr *)&addr, &len);
    ::closesocket(s);
    return ntohs(addr.sin_port);
#else
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      return 0;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, (struct sockaddr *)&addr, &len);
    ::close(fd);
    return ntohs(addr.sin_port);
#endif
  }

  Thread io_thread_{"tls-test-io"};
  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  Thread srv_thread_{"tls-test-srv"};
  scoped_refptr<SingleThreadTaskRunner> srv_runner_;
  std::string cert_pem_;
  std::string key_pem_;
  SSLContext server_ctx_{SSLContext::Mode::Server};
  SSLContext client_ctx_{SSLContext::Mode::Client};
};

// =============================================================================
// Test 1 — Cert generation smoke test
// =============================================================================

TEST_F(TlsSocketTest, CertGeneration) {
  ASSERT_FALSE(cert_pem_.empty());
  ASSERT_FALSE(key_pem_.empty());

  SSLContext srv(SSLContext::Mode::Server);
  EXPECT_TRUE(srv.SetCertificate(cert_pem_, key_pem_));

  SSLContext cli(SSLContext::Mode::Client);
  EXPECT_TRUE(cli.SetCAChain(cert_pem_));

  EXPECT_EQ(mbedtls_ssl_conf_get_endpoint(srv.config()), MBEDTLS_SSL_IS_SERVER);
  EXPECT_EQ(mbedtls_ssl_conf_get_endpoint(cli.config()), MBEDTLS_SSL_IS_CLIENT);
}

// =============================================================================
// Test 2 — HandshakeEofDeathSpiral (防 EOF 死亡螺旋)
// =============================================================================
//
// Raw TCP server accepts then immediately closes (sends FIN).  The TLS
// client must detect the EOF and report failure instead of entering an
// infinite WANT_READ → ReadAsync → EOF → retry loop.

TEST_F(TlsSocketTest, HandshakeEofDeathSpiral) {
  uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);

  // ---- Raw TCP server: accept once, then close immediately ----
  WaitableEvent server_ready(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> server_done{false};

  std::thread raw_server([&]() {
#if defined(_WIN32)
    SOCKET listen_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(listen_fd, INVALID_SOCKET);
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    ASSERT_EQ(::bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
    ASSERT_EQ(::listen(listen_fd, 1), 0);
    server_ready.Signal();

    SOCKET client_fd = ::accept(listen_fd, nullptr, nullptr);
    if (client_fd != INVALID_SOCKET) {
      // Immediately close — sends RST, not graceful FIN.  On Windows,
      // closesocket with SO_LINGER set to 0 ensures immediate RST.
      struct linger lg = {1, 0};
      setsockopt(client_fd, SOL_SOCKET, SO_LINGER, (char *)&lg, sizeof(lg));
      ::closesocket(client_fd);
    }
    ::closesocket(listen_fd);
#else
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    ASSERT_EQ(::bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
    ASSERT_EQ(::listen(listen_fd, 1), 0);
    server_ready.Signal();

    int client_fd = ::accept(listen_fd, nullptr, nullptr);
    if (client_fd >= 0) {
      ::close(client_fd); // Sends FIN → peer sees EOF
    }
    ::close(listen_fd);
#endif
    server_done.store(true);
  });

  server_ready.Wait();

  // ---- TLS client: connect → should fail on EOF during handshake ----
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> connect_ok{true};

  auto client = std::make_shared<TLSClientSocket>(std::make_unique<TCPClientSocket>(), &client_ctx_);
  io_runner_->PostTask(FROM_HERE, [&]() {
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [&](bool ok) {
          connect_ok.store(ok);
          done.Signal();
        },
        io_runner_);
  });

  // Must complete within 5 seconds — if EOF death spiral exists, this hangs.
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5))) << "Test hung — EOF death spiral likely!";
  EXPECT_FALSE(connect_ok.load()) << "Handshake should fail when raw server sends RST/FIN mid-handshake";

  raw_server.join();
}

// =============================================================================
// Test 3 — DestructionDuringHandshake (防 UAF / 僵尸状态机)
// =============================================================================
//
// Start a TLS handshake, then destroy the client socket before it completes.
// Orphan() must clean up without crashing.

TEST_F(TlsSocketTest, DestructionDuringHandshake) {
  uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);

  // ---- TLS server: accept, but DON'T call the accept callback ----
  // We just need TCP connect + ClientHello to be sent.  The handshake
  // will stall waiting for ServerHello.
  auto server = std::make_shared<TLSServerSocket>(&server_ctx_);
  WaitableEvent server_ready(WaitableEvent::ResetPolicy::kAutomatic, false);
  srv_runner_->PostTask(FROM_HERE, [&]() {
    ASSERT_TRUE(server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        1,
        [](bool, std::unique_ptr<TLSClientSocket>) {},
        srv_runner_));
    server_ready.Signal();
  });
  server_ready.Wait();

  // ---- TLS client: start handshake, destroy before completion ----
  auto client = std::make_shared<TLSClientSocket>(std::make_unique<TCPClientSocket>(), &client_ctx_);

  io_runner_->PostTask(FROM_HERE, [&]() {
    client->Connect(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), [](bool) {}, io_runner_);
  });

  // Let TCP connect + ClientHello get sent, then blow it away.
  auto drain = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner_->PostTask(FROM_HERE, [drain]() { drain->Signal(); });
  drain->Wait();

  // Destroy client mid-handshake.  This triggers Orphan() → Close().
  client.reset();

  // Drain both IO threads — must not crash.
  io_runner_->PostTask(FROM_HERE, [drain]() { drain->Signal(); });
  drain->Wait();
  srv_runner_->PostTask(FROM_HERE, [drain]() { drain->Signal(); });
  drain->Wait();

  server->Close();
  SUCCEED() << "Destroyed TLS client mid-handshake without crash";
}

// =============================================================================
// Test 4 — LargePayloadBioCompaction (大流量 BIO 水位线防内存损坏)
// =============================================================================
//
// 10 MB encrypted transfer verifies that recv_offset compaction at the
// 64 KB threshold does not drop or corrupt any bytes.

TEST_F(TlsSocketTest, LargePayloadBioCompaction) {
  uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);

  constexpr size_t kPayloadSize = 10 * 1024 * 1024; // 10 MB
  auto payload = std::make_shared<std::vector<unsigned char>>(kPayloadSize);
  for (size_t i = 0; i < kPayloadSize; ++i)
    (*payload)[i] = static_cast<unsigned char>((i * 7 + 13) & 0xFF);

  // ---- Server: accept, receive entire payload ----
  auto recv_buf = std::make_shared<std::vector<unsigned char>>();
  recv_buf->reserve(kPayloadSize);
  auto server_done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto server_ok = std::make_shared<std::atomic<bool>>(false);

  auto server = std::make_shared<TLSServerSocket>(&server_ctx_);
  srv_runner_->PostTask(FROM_HERE, [&, server_done]() {
    ASSERT_TRUE(server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        1,
        [&, server_done](bool ok, std::unique_ptr<TLSClientSocket> tls) {
          ASSERT_TRUE(ok);
          auto tls_shared = std::make_shared<TLSClientSocket>(std::move(*tls));
          auto offset = std::make_shared<size_t>(0);
          auto do_read = std::make_shared<std::function<void()>>();
          *do_read = [=]() {
            size_t remain = kPayloadSize - *offset;
            if (remain == 0) {
              server_ok->store(true);
              server_done->Signal();
              return;
            }
            auto chunk = MakeRefCounted<IOBufferWithSize>(remain);
            tls_shared->ReadAsync(chunk, remain, [=](bool s, size_t n) {
              if (!s || n == 0) {
                server_done->Signal();
                return;
              }
              recv_buf->insert(recv_buf->end(), chunk->data(), chunk->data() + n);
              *offset += n;
              (*do_read)();
            });
          };
          (*do_read)();
        },
        srv_runner_));
  });

  // ---- Client: connect, send whole payload ----
  auto client = std::make_shared<TLSClientSocket>(std::make_unique<TCPClientSocket>(), &client_ctx_);
  auto client_done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);

  io_runner_->PostTask(FROM_HERE, [&, client_done]() {
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [&, client_done](bool ok) {
          ASSERT_TRUE(ok);
          auto send_buf = MakeRefCounted<IOBufferWithSize>(kPayloadSize);
          std::memcpy(send_buf->data(), payload->data(), kPayloadSize);
          auto offset = std::make_shared<size_t>(0);
          auto do_write = std::make_shared<std::function<void()>>();
          *do_write = [=]() {
            size_t remain = kPayloadSize - *offset;
            if (remain == 0) {
              client_done->Signal();
              return;
            }
            // Create a sub-buffer view for the remaining data so each
            // WriteAsync call advances the write position.
            auto chunk = MakeRefCounted<IOBufferWithSize>(remain);
            std::memcpy(chunk->data(), send_buf->data() + *offset, remain);
            client->WriteAsync(chunk, remain, [=](bool s, size_t n) {
              ASSERT_TRUE(s);
              *offset += n;
              (*do_write)();
            });
          };
          (*do_write)();
        },
        io_runner_);
  });

  ASSERT_TRUE(client_done->TimedWait(std::chrono::seconds(30)));
  ASSERT_TRUE(server_done->TimedWait(std::chrono::seconds(30)));
  ASSERT_TRUE(server_ok->load());

  // Verify byte-for-byte match.
  ASSERT_EQ(recv_buf->size(), kPayloadSize);
  EXPECT_EQ(*recv_buf, *payload) << "10 MB TLS round-trip: data corruption detected!";
}

// =============================================================================
// Test 5 — StrictPeerVerificationFailure (非法证书致命拦截)
// =============================================================================
//
// Client uses PeerVerify::kRequired with a WRONG CA — handshake must fail.

TEST_F(TlsSocketTest, StrictPeerVerificationFailure) {
  uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);

  // Generate a SECOND self-signed cert that the client will NOT trust.
  TestCert wrong_cert = GenerateSelfSignedCert();
  ASSERT_FALSE(wrong_cert.cert_pem.empty());

  // Server uses our test cert.
  auto server = std::make_shared<TLSServerSocket>(&server_ctx_);
  WaitableEvent server_ready(WaitableEvent::ResetPolicy::kAutomatic, false);
  srv_runner_->PostTask(FROM_HERE, [&]() {
    ASSERT_TRUE(server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        1,
        [](bool, std::unique_ptr<TLSClientSocket>) {},
        srv_runner_));
    server_ready.Signal();
  });
  server_ready.Wait();

  // Client: PeerVerify::kRequired with the WRONG CA.  Declared before the
  // socket so it outlives it (lifetime contract for the mbedtls session).
  SSLContext strict_cli(SSLContext::Mode::Client);
  strict_cli.SetPeerVerify(PeerVerify::kRequired);
  ASSERT_TRUE(strict_cli.SetCAChain(wrong_cert.cert_pem));

  auto client = std::make_shared<TLSClientSocket>(std::make_unique<TCPClientSocket>(), &strict_cli);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> connect_ok{true};

  io_runner_->PostTask(FROM_HERE, [&]() {
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [&](bool ok) {
          connect_ok.store(ok);
          done.Signal();
        },
        io_runner_);
  });

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(10)));
  EXPECT_FALSE(connect_ok.load()) << "TLS handshake must FAIL with untrusted certificate";

  server->Close();
}

} // namespace
} // namespace nei::net
