// libnei TLS single-connection loopback throughput benchmark
//
// Measures encrypted throughput (MB/s) over a TLS connection.
// Verifies data integrity via byte-for-byte comparison.
//
// Build: cmake --build build/linux-gcc-release-shared --target tls_throughput_bench
// Run:   ./tls_throughput_bench [total_MB] [buffer_size]
//        default: 10 MB, 64 KB buffer

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include <neixx/common/at_exit.h>
#include <neixx/common/location.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tls_client_socket.h>
#include <neixx/net/tls_server_socket.h>
#include <neixx/net/wsa_init.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace {

using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Self-signed cert generator (in-memory, no file I/O)
// ---------------------------------------------------------------------------
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

  int ret = mbedtls_pk_setup(&key,
      mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
  if (ret != 0) { std::cerr << "pk_setup failed" << std::endl; return {}; }
  ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key),
      mbedtls_ctr_drbg_random, &drbg, 2048, 65537);
  if (ret != 0) { std::cerr << "rsa_gen_key failed" << std::endl; return {}; }

  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_mpi_lset(&serial, 1);
  const char* subj = "CN=tls-bench,O=NEI,C=CN";
  mbedtls_x509write_crt_set_subject_name(&crt, subj);
  mbedtls_x509write_crt_set_issuer_name(&crt, subj);
  mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20350101000000");
  mbedtls_x509write_crt_set_subject_key(&crt, &key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key);
  mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);

  unsigned char der[4096];
  ret = mbedtls_x509write_crt_der(&crt, der, sizeof(der),
      mbedtls_ctr_drbg_random, &drbg);
  std::string cert_pem, key_pem;
  if (ret > 0) {
    unsigned char pem[8192]; size_t olen = 0;
    mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n",
        "-----END CERTIFICATE-----\n",
        der + sizeof(der) - ret, static_cast<size_t>(ret),
        pem, sizeof(pem), &olen);
    cert_pem.assign(reinterpret_cast<char*>(pem), olen);
  }
  unsigned char kder[4096];
  int klen = mbedtls_pk_write_key_der(&key, kder, sizeof(kder));
  if (klen > 0) {
    unsigned char pem[8192]; size_t olen = 0;
    mbedtls_pem_write_buffer("-----BEGIN RSA PRIVATE KEY-----\n",
        "-----END RSA PRIVATE KEY-----\n",
        kder + sizeof(kder) - klen, static_cast<size_t>(klen),
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

// ---------------------------------------------------------------------------
// IO thread helper
// ---------------------------------------------------------------------------
class IoThread {
 public:
  explicit IoThread(const std::string& name) {
    nei::Thread::Options opts;
    opts.message_pump_type = nei::MessagePumpType::IO;
    thread_ = std::make_unique<nei::Thread>(name);
    thread_->StartWithOptions(opts);
    runner_ = thread_->GetTaskRunner();
  }
  ~IoThread() { thread_->Stop(); }
  nei::scoped_refptr<nei::TaskRunner> runner() const { return runner_; }
 private:
  std::unique_ptr<nei::Thread> thread_;
  nei::scoped_refptr<nei::TaskRunner> runner_;
};

static uint16_t FindFreePort() {
#if defined(_WIN32)
  nei::net::EnsureWsa();
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

// ---------------------------------------------------------------------------
// Benchmark
// ---------------------------------------------------------------------------
void RunBenchmark(size_t total_bytes, size_t buffer_size) {
  const uint16_t port = FindFreePort();
  if (port == 0) { std::cerr << "ERROR: no free port" << std::endl; return; }

  // Single IO thread — avoids cross-thread race between client send
  // and server receive that causes truncated reads on WSL.
  IoThread io("tls-bench");

  TestCert cert = GenerateSelfSignedCert();
  if (cert.cert_pem.empty() || cert.key_pem.empty()) {
    std::cerr << "ERROR: cert generation failed" << std::endl;
    return;
  }

  nei::net::SSLContext srv_ctx(nei::net::SSLContext::Mode::Server);
  srv_ctx.SetCertificate(cert.cert_pem, cert.key_pem);
  nei::net::SSLContext cli_ctx(nei::net::SSLContext::Mode::Client);
  cli_ctx.SetPeerVerify(nei::net::PeerVerify::kOptional);
  cli_ctx.SetCAChain(cert.cert_pem);

  auto payload = std::make_shared<std::vector<unsigned char>>(total_bytes);
  for (size_t i = 0; i < total_bytes; ++i)
    (*payload)[i] = static_cast<unsigned char>((i * 37 + 17) & 0xFF);

  auto recv_buf = std::make_shared<std::vector<unsigned char>>();
  recv_buf->reserve(total_bytes);

  auto bench_done = std::make_shared<nei::WaitableEvent>(
      nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  auto server_ready = std::make_shared<nei::WaitableEvent>(
      nei::WaitableEvent::ResetPolicy::kAutomatic, false);

  auto server = std::make_shared<nei::net::TLSServerSocket>(&srv_ctx);
  auto client = std::make_shared<nei::net::TLSClientSocket>(
      std::make_unique<nei::net::TCPClientSocket>(), &cli_ctx);

  auto t_start = Clock::now();

  auto runner = io.runner();
  runner->PostTask(FROM_HERE, [=]() mutable {
    // ---- Server: listen + accept ----
    bool ok = server->Listen(
        nei::net::IPEndPoint(
            nei::net::IPAddress::FromIPv4(127, 0, 0, 1), port), 1,
        [=](bool success, std::unique_ptr<nei::net::TLSClientSocket> tls) {
          if (!success) { bench_done->Signal(); return; }
          // ---- Server: receive until EOF ----
          auto sock = std::make_shared<nei::net::TLSClientSocket>(
              std::move(*tls));
          auto do_read = std::make_shared<std::function<void()>>();
          *do_read = [=]() {
            auto chunk = nei::MakeRefCounted<nei::IOBufferWithSize>(buffer_size);
            sock->ReadAsync(chunk, buffer_size,
                [=](bool s, size_t n) {
                  if (!s) { bench_done->Signal(); return; }
                  if (n == 0) {
                    // EOF — all data received.
                    sock->Close();
                    bench_done->Signal();
                    return;
                  }
                  recv_buf->insert(recv_buf->end(),
                      chunk->data(), chunk->data() + n);
                  (*do_read)();
                });
          };
          (*do_read)();
        },
        runner);
    if (!ok) { bench_done->Signal(); return; }
    server_ready->Signal();

    // ---- Client: connect + send + close ----
    client->Connect(
        nei::net::IPEndPoint(
            nei::net::IPAddress::FromIPv4(127, 0, 0, 1), port),
        [=](bool ok) {
          if (!ok) { bench_done->Signal(); return; }
          auto offset = std::make_shared<size_t>(0);
          auto do_write = std::make_shared<std::function<void()>>();
          *do_write = [=]() {
            size_t remain = total_bytes - *offset;
            if (remain == 0) {
              client->Close();  // Sends close_notify → server sees EOF.
              return;
            }
            auto chunk = nei::MakeRefCounted<nei::IOBufferWithSize>(
                std::min(remain, buffer_size));
            std::memcpy(chunk->data(), payload->data() + *offset,
                        chunk->size());
            client->WriteAsync(chunk, chunk->size(),
                [=](bool s, size_t n) {
                  if (!s) { bench_done->Signal(); return; }
                  *offset += n;
                  (*do_write)();
                });
          };
          (*do_write)();
        },
        runner);
  });

  server_ready->Wait();
  bench_done->Wait();

  auto t_end = Clock::now();
  double elapsed = std::chrono::duration<double>(t_end - t_start).count();
  double rate = total_bytes / elapsed / (1024 * 1024);
  std::cout << "\n=== TLS Throughput Benchmark ===\n"
            << "  Data       : " << (total_bytes >> 20) << " MB\n"
            << "  Buffer     : " << (buffer_size >> 10) << " KB\n"
            << "  Elapsed    : " << std::fixed << std::setprecision(3)
            << elapsed << " s\n"
            << "  Throughput : " << std::fixed << std::setprecision(1)
            << rate << " MB/s\n";

  if (recv_buf->size() != total_bytes) {
    std::cerr << "ERROR: size mismatch  sent=" << total_bytes
              << "  recv=" << recv_buf->size() << std::endl;
  } else if (*recv_buf != *payload) {
    std::cerr << "ERROR: data corruption detected!" << std::endl;
  } else {
    std::cout << "  Integrity   : OK (byte-for-byte match)" << std::endl;
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  nei::AtExitManager at_exit;

  size_t total_mb = 10;
  size_t buffer_kb = 64;

  if (argc > 1) total_mb = static_cast<size_t>(std::atoll(argv[1]));
  if (argc > 2) buffer_kb = static_cast<size_t>(std::atoll(argv[2]));

  if (total_mb == 0 || total_mb > 100) {
    std::cerr << "Usage: " << argv[0] << " [total_MB=10] [buffer_KB=64]\n"
              << "  total_MB: 1..100 (default 10)\n"
              << "  buffer_KB: 4..1024 (default 64)\n";
    return 1;
  }

  RunBenchmark(total_mb * 1024ULL * 1024ULL, buffer_kb * 1024ULL);
  return 0;
}
