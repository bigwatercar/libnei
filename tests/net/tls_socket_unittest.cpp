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
#include <neixx/net/ssl_context.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tcp_server_socket.h>
#include <neixx/net/tls_client_socket.h>
#include <neixx/net/tls_server_socket.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
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
  ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random,
                            &drbg, 2048, 65537);
  if (ret != 0) {
    ADD_FAILURE() << "rsa_gen_key: " << ret;
    return {};
  }

  // Build self-signed cert.
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_mpi_lset(&serial, 1);

  const char* subject = "CN=libnei-test,O=NEI,C=CN";
  mbedtls_x509write_crt_set_subject_name(&crt, subject);
  mbedtls_x509write_crt_set_issuer_name(&crt, subject);
  mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20350101000000");
  mbedtls_x509write_crt_set_subject_key(&crt, &key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key);
  mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);

  unsigned char der_buf[4096];
  ret = mbedtls_x509write_crt_der(&crt, der_buf, sizeof(der_buf),
                                  mbedtls_ctr_drbg_random, &drbg);
  if (ret <= 0) {
    ADD_FAILURE() << "x509write_crt_der: " << ret;
    return {};
  }

  std::string cert_pem, key_pem;
  if (ret > 0) {
    unsigned char pem[8192]; size_t olen = 0;
    mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n",
                             "-----END CERTIFICATE-----\n",
                             der_buf + sizeof(der_buf) - ret, static_cast<size_t>(ret),
                             pem, sizeof(pem), &olen);
    cert_pem.assign(reinterpret_cast<char*>(pem), olen);
  }

  unsigned char key_der[4096];
  int key_len = mbedtls_pk_write_key_der(&key, key_der, sizeof(key_der));
  EXPECT_GT(key_len, 0) << "pk_write_key_der failed";
  if (key_len > 0) {
    unsigned char pem[8192]; size_t olen = 0;
    mbedtls_pem_write_buffer("-----BEGIN RSA PRIVATE KEY-----\n",
                             "-----END RSA PRIVATE KEY-----\n",
                             key_der + sizeof(key_der) - key_len, static_cast<size_t>(key_len),
                             pem, sizeof(pem), &olen);
    key_pem.assign(reinterpret_cast<char*>(pem), olen);
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

    // Server context.
    server_ctx_ = std::make_unique<SSLContext>(SSLContext::Mode::Server);
    ASSERT_TRUE(server_ctx_->SetCertificate(cert_pem_, key_pem_));

    // Client context — trusts our self-signed CA.
    client_ctx_ = std::make_unique<SSLContext>(SSLContext::Mode::Client);
    client_ctx_->SetPeerVerify(nei::net::PeerVerify::kOptional);
    ASSERT_TRUE(client_ctx_->SetCAChain(cert_pem_));

    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(io_thread_.StartWithOptions(opts));
    io_runner_ = io_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);
    ASSERT_TRUE(srv_thread_.StartWithOptions(opts));
    srv_runner_ = srv_thread_.GetTaskRunner();
    ASSERT_TRUE(srv_runner_);
  }

  void TearDown() override { srv_thread_.Stop(); io_thread_.Stop(); }

  static uint16_t FindFreePort() {
#if defined(_WIN32)
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(s, (struct sockaddr*)&addr, sizeof(addr));
    int len = sizeof(addr);
    ::getsockname(s, (struct sockaddr*)&addr, &len);
    ::closesocket(s);
    return ntohs(addr.sin_port);
#else
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, (struct sockaddr*)&addr, &len);
    ::close(fd);
    return ntohs(addr.sin_port);
#endif
  }

  Thread io_thread_{"tls-test-io"};
  scoped_refptr<TaskRunner> io_runner_;
  Thread srv_thread_{"tls-test-srv"};
  scoped_refptr<TaskRunner> srv_runner_;
  std::string cert_pem_;
  std::string key_pem_;
  std::unique_ptr<SSLContext> server_ctx_;
  std::unique_ptr<SSLContext> client_ctx_;
};

// =============================================================================
// Test 0 — Cert generation smoke test
// =============================================================================

TEST_F(TlsSocketTest, CertGeneration) {
  ASSERT_FALSE(cert_pem_.empty());
  ASSERT_FALSE(key_pem_.empty());

  SSLContext srv(SSLContext::Mode::Server);
  EXPECT_TRUE(srv.SetCertificate(cert_pem_, key_pem_))
      << "SetCertificate failed";

  SSLContext cli(SSLContext::Mode::Client);
  EXPECT_TRUE(cli.SetCAChain(cert_pem_))
      << "SetCAChain failed";

  // Verify endpoint configuration via mbedtls public getter.
  EXPECT_EQ(mbedtls_ssl_conf_get_endpoint(srv.config()), MBEDTLS_SSL_IS_SERVER)
      << "Server endpoint is not MBEDTLS_SSL_IS_SERVER";
  EXPECT_EQ(mbedtls_ssl_conf_get_endpoint(cli.config()), MBEDTLS_SSL_IS_CLIENT)
      << "Client endpoint is not MBEDTLS_SSL_IS_CLIENT";
}

// =============================================================================
// Test 1 — Basic TLS handshake
// =============================================================================

TEST_F(TlsSocketTest, BasicHandshake) {
  uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);
  ASSERT_NE(port, 0);

  WaitableEvent accepted(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent connected(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> accept_ok{false};
  std::atomic<bool> connect_ok{false};

  // Server — runs on its own IO thread to avoid deadlock with client.
  WaitableEvent server_ready(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto server = std::make_shared<TLSServerSocket>(server_ctx_.get());
  srv_runner_->PostTask(FROM_HERE, [&]() {
    ASSERT_TRUE(server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), 1,
        [&](bool ok, std::unique_ptr<TLSClientSocket> /*c*/) {
          accept_ok.store(ok);
          accepted.Signal();
        },
        srv_runner_));
    server_ready.Signal();
  });
  server_ready.Wait();

  // Client
  auto client = std::make_shared<TLSClientSocket>(
      std::make_unique<TCPClientSocket>(), client_ctx_.get());
  io_runner_->PostTask(FROM_HERE, [&]() {
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [&](bool ok) { connect_ok.store(ok); connected.Signal(); },
        io_runner_);
  });

  ASSERT_TRUE(accepted.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(connected.TimedWait(std::chrono::seconds(10)));
  EXPECT_TRUE(accept_ok.load());
  EXPECT_TRUE(connect_ok.load());
}

// =============================================================================
// Test 2 — Data transfer 1 MB
// =============================================================================

TEST_F(TlsSocketTest, DataTransfer) {
  uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);
  constexpr size_t kSize = 1024 * 1024;

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> data_ok{false};

  auto server = std::make_shared<TLSServerSocket>(server_ctx_.get());
  srv_runner_->PostTask(FROM_HERE, [&]() {
    ASSERT_TRUE(server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), 1,
        [&](bool ok, std::unique_ptr<TLSClientSocket> tls) {
          ASSERT_TRUE(ok);
          auto buf = MakeRefCounted<IOBufferWithSize>(kSize);
          std::memset(buf->data(), 0, kSize);
          auto offset = std::make_shared<size_t>(0);
          auto do_read = std::make_shared<std::function<void()>>();
          auto tls_shared = std::make_shared<TLSClientSocket>(std::move(*tls));
          *do_read = [&, tls_shared, buf, offset, do_read]() {
            size_t remain = kSize - *offset;
            if (remain == 0) { done.Signal(); return; }
            auto chunk = MakeRefCounted<IOBufferWithSize>(remain);
            tls_shared->ReadAsync(chunk, remain,
                [offset, buf, do_read, chunk](bool s, size_t n) {
                  if (!s || n == 0) return;
                  std::memcpy(buf->data() + *offset, chunk->data(), n);
                  *offset += n;
                  (*do_read)();
                });
          };
          (*do_read)();
        },
        srv_runner_));
  });

  auto client = std::make_shared<TLSClientSocket>(
      std::make_unique<TCPClientSocket>(), client_ctx_.get());
  WaitableEvent connected(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner_->PostTask(FROM_HERE, [&]() {
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [&](bool ok) {
          ASSERT_TRUE(ok);
          auto send = MakeRefCounted<IOBufferWithSize>(kSize);
          for (size_t i = 0; i < kSize; ++i) send->data()[i] = static_cast<char>(i & 0xFF);
          auto offset = std::make_shared<size_t>(0);
          auto do_write = std::make_shared<std::function<void()>>();
          *do_write = [&, send, offset, do_write]() {
            size_t remain = kSize - *offset;
            if (remain == 0) { connected.Signal(); return; }
            client->WriteAsync(send, remain,
                [offset, do_write](bool s, size_t n) {
                  ASSERT_TRUE(s);
                  *offset += n;
                  (*do_write)();
                });
          };
          (*do_write)();
        },
        io_runner_);
  });

  ASSERT_TRUE(connected.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(10)));
  SUCCEED() << "1 MB transferred successfully over TLS";
}

// =============================================================================
// Test 3 — Connection refused (no server)
// =============================================================================

TEST_F(TlsSocketTest, ConnectionRefused) {
  uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> result{true};

  auto client = std::make_shared<TLSClientSocket>(
      std::make_unique<TCPClientSocket>(), client_ctx_.get());
  io_runner_->PostTask(FROM_HERE, [&]() {
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [&](bool ok) { result.store(ok); done.Signal(); },
        io_runner_);
  });

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(10)));
  EXPECT_FALSE(result.load()) << "Connect to dead port should fail";
}

}  // namespace
}  // namespace nei::net
