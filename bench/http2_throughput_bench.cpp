// libnei HTTP/2 loopback throughput benchmark (TLS, h2 ALPN)
//
// Measures request/response throughput over a single HTTP/2 connection and
// compares sequential vs. concurrent (multiplexed) submission modes.
// Pair with http_throughput_bench (HTTP/1.1 keep-alive) for the 1.1 vs 2
// comparison.
//
// Build: cmake --build build/windows-vs2022-shared --target http2_throughput_bench --config Release
// Run:   .\http2_throughput_bench [requests] [concurrency]
//        default: 10000 requests, concurrency 1 (sequential)

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
#include <neixx/memory/ref_counted.h>
#include <neixx/net/http/http2_client_session.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/wsa_init.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

// This binary links its own copy of Mbed TLS (nei.dll does not export the
// symbols) — register threading callbacks for that copy before any direct
// Mbed TLS call (rsa_gen_key etc. fail without them).
#include "mbedtls_threading.h"

using namespace nei;
using namespace nei::net;
using namespace nei::net::http;

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint16_t kServerPort = 18093;
constexpr int kDefaultRequests = 10000;
constexpr int kDefaultConcurrency = 1;

// ---------------------------------------------------------------------------
// Self-signed cert generator (in-memory, no file I/O) — same approach as
// tls_throughput_bench.
// ---------------------------------------------------------------------------
struct TestCert {
  std::string cert_pem;
  std::string key_pem;
};

TestCert GenerateSelfSignedCert() {
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

  int ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
  if (ret != 0) {
    std::cerr << "pk_setup failed" << std::endl;
    return {};
  }
  ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random, &drbg, 2048, 65537);
  if (ret != 0) {
    std::cerr << "rsa_gen_key failed" << std::endl;
    return {};
  }

  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_mpi_lset(&serial, 1);
  const char *subj = "CN=http2-bench,O=NEI,C=CN";
  mbedtls_x509write_crt_set_subject_name(&crt, subj);
  mbedtls_x509write_crt_set_issuer_name(&crt, subj);
  mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20350101000000");
  mbedtls_x509write_crt_set_subject_key(&crt, &key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key);
  mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);

  unsigned char der[4096];
  ret = mbedtls_x509write_crt_der(&crt, der, sizeof(der), mbedtls_ctr_drbg_random, &drbg);
  std::string cert_pem, key_pem;
  if (ret > 0) {
    unsigned char pem[8192];
    size_t olen = 0;
    mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n",
                             "-----END CERTIFICATE-----\n",
                             der + sizeof(der) - ret,
                             static_cast<size_t>(ret),
                             pem,
                             sizeof(pem),
                             &olen);
    cert_pem.assign(reinterpret_cast<char *>(pem), olen);
  }
  unsigned char kder[4096];
  int klen = mbedtls_pk_write_key_der(&key, kder, sizeof(kder));
  if (klen > 0) {
    unsigned char pem[8192];
    size_t olen = 0;
    mbedtls_pem_write_buffer("-----BEGIN RSA PRIVATE KEY-----\n",
                             "-----END RSA PRIVATE KEY-----\n",
                             kder + sizeof(kder) - klen,
                             static_cast<size_t>(klen),
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

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
struct Stats {
  std::atomic<int64_t> completed{0};
  std::atomic<int64_t> total_ns{0};
  std::atomic<int64_t> min_ns{INT64_MAX};
  std::atomic<int64_t> max_ns{0};
  std::atomic<int64_t> bad_body{0};
};

struct RequestState {
  Clock::time_point start;
  std::string body;
};

// ---------------------------------------------------------------------------
// Benchmark runner
// ---------------------------------------------------------------------------
void RunBenchmark(int total_requests, int concurrency) {
  AtExitManager at_exit;
  net::EnsureWsa();

  TestCert cert = GenerateSelfSignedCert();
  if (cert.cert_pem.empty() || cert.key_pem.empty()) {
    std::cerr << "cert generation failed" << std::endl;
    return;
  }

  net::SSLContext server_ctx(net::SSLContext::Mode::Server);
  if (!server_ctx.SetCertificate(cert.cert_pem, cert.key_pem)) {
    std::cerr << "SetCertificate failed" << std::endl;
    return;
  }
  server_ctx.SetAlpnProtocols({"h2"});

  net::SSLContext client_ctx(net::SSLContext::Mode::Client);
  client_ctx.SetPeerVerify(net::PeerVerify::kOptional);
  if (!client_ctx.SetCAChain(cert.cert_pem)) {
    std::cerr << "SetCAChain failed" << std::endl;
    return;
  }
  client_ctx.SetAlpnProtocols({"h2"});

  // ---- Server on dedicated IO thread ----
  Thread server_thread;
  Thread::Options server_opts;
  server_opts.message_pump_type = MessagePumpType::IO;
  if (!server_thread.StartWithOptions(server_opts)) {
    std::cerr << "server thread start failed" << std::endl;
    return;
  }
  auto server_runner = server_thread.GetTaskRunner();

  auto server = std::make_shared<HttpServer>();
  auto server_ready = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto server_error = std::make_shared<std::atomic<bool>>(false);

  server_runner->PostTask(FROM_HERE, [server, server_ready, server_error, server_runner, &server_ctx]() {
    server->AddRoute(HttpMethod::kGet, "/ping", [](const HttpRequest &) {
      HttpResponse resp;
      resp.SetStatus(HttpStatusCode::kOk);
      resp.body = "pong";
      resp.headers.push_back({"Content-Type", "text/plain"});
      return resp;
    });
    IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), kServerPort);
    if (!server->Listen(addr, &server_ctx, server_runner))
      server_error->store(true);
    server_ready->Signal();
  });
  server_ready->Wait();
  if (server_error->load()) {
    std::cerr << "server listen failed" << std::endl;
    return;
  }

  // ---- Client on separate IO thread ----
  Thread client_thread;
  Thread::Options client_opts;
  client_opts.message_pump_type = MessagePumpType::IO;
  if (!client_thread.StartWithOptions(client_opts)) {
    std::cerr << "client thread start failed" << std::endl;
    return;
  }
  auto client_runner = client_thread.GetTaskRunner();

  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto session_closed = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto stats = std::make_shared<Stats>();
  auto send_error = std::make_shared<std::atomic<bool>>(false);
  auto session_holder = std::make_shared<scoped_refptr<Http2ClientSession>>();

  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), kServerPort);

  auto bench_start = std::make_shared<Clock::time_point>();

  client_runner->PostTask(FROM_HERE, [=, &client_ctx]() {
    auto session = scoped_refptr<Http2ClientSession>(new Http2ClientSession());
    *session_holder = session;
    session->SetSessionCloseCallback([session_closed](std::string reason) {
      std::cerr << "session closed: " << reason << std::endl;
      session_closed->Signal();
    });
    auto remaining = std::make_shared<std::atomic<int>>(total_requests);
    auto inflight = std::make_shared<std::atomic<int>>(0);

    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.url = Url("https://127.0.0.1/ping");

    auto submit_next = std::make_shared<std::function<void()>>();
    *submit_next = [=]() {
      while (inflight->load() < concurrency && remaining->load() > 0) {
        remaining->fetch_sub(1);
        inflight->fetch_add(1);
        auto state = std::make_shared<RequestState>();
        state->start = Clock::now();
        int32_t id = session->SubmitRequest(
            req,
            [](int32_t, HttpStatus, const HttpHeaders &) {},
            [state](int32_t, const char *data, std::size_t len, bool) {
              if (len > 0)
                state->body.append(data, len);
            },
            [stats, state, inflight, remaining, done, submit_next, send_error](int32_t, bool clean) {
              if (!clean)
                send_error->store(true);
              if (state->body != "pong")
                stats->bad_body.fetch_add(1);
              int64_t rtt = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - state->start).count();
              stats->completed.fetch_add(1);
              stats->total_ns.fetch_add(rtt);
              int64_t cur = stats->min_ns.load();
              while (rtt < cur && !stats->min_ns.compare_exchange_weak(cur, rtt)) {
              }
              cur = stats->max_ns.load();
              while (rtt > cur && !stats->max_ns.compare_exchange_weak(cur, rtt)) {
              }
              inflight->fetch_sub(1);
              if (remaining->load() == 0 && inflight->load() == 0) {
                done->Signal();
                return;
              }
              (*submit_next)();
            });
        if (id < 0)
          send_error->store(true);
      }
    };

    // Start the benchmark inside the connect callback — waiting here would
    // deadlock (the callback is posted back to this same runner).
    session->Connect(
        addr, &client_ctx, client_runner, [bench_start, submit_next, done, send_error](bool ok, std::string error) {
          if (!ok) {
            std::cerr << "connect failed: " << error << std::endl;
            send_error->store(true);
            done->Signal();
            return;
          }
          *bench_start = Clock::now();
          (*submit_next)();
        });
  });

  done->Wait();

  auto bench_end = Clock::now();
  double elapsed = std::chrono::duration_cast<std::chrono::microseconds>(bench_end - *bench_start).count() / 1e6;

  // Graceful close: GOAWAY drain, then session_closed fires.
  if (*session_holder)
    client_runner->PostTask(FROM_HERE, [session_holder]() { (*session_holder)->Close(); });
  session_closed->Wait();

  int64_t completed = stats->completed.load();
  std::cout << "=== HTTP/2 loopback throughput (TLS, h2) ===\n";
  std::cout << std::left << std::setw(16) << "requests" << total_requests << "\n";
  std::cout << std::left << std::setw(16) << "concurrency" << concurrency << "\n";
  std::cout << std::left << std::setw(16) << "completed" << completed << "\n";
  std::cout << std::left << std::setw(16) << "elapsed_s" << std::fixed << std::setprecision(3) << elapsed << "\n";
  std::cout << std::left << std::setw(16) << "req/s" << std::fixed << std::setprecision(1)
            << (elapsed > 0 ? completed / elapsed : 0) << "\n";
  if (completed > 0) {
    double mean_us = stats->total_ns.load() / 1e3 / completed;
    std::cout << std::left << std::setw(16) << "mean_rtt_us" << std::fixed << std::setprecision(1) << mean_us << "\n";
    std::cout << std::left << std::setw(16) << "min_rtt_us" << std::fixed << std::setprecision(1)
              << stats->min_ns.load() / 1e3 << "\n";
    std::cout << std::left << std::setw(16) << "max_rtt_us" << std::fixed << std::setprecision(1)
              << stats->max_ns.load() / 1e3 << "\n";
  }
  if (stats->bad_body.load() > 0 || send_error->load())
    std::cout << "errors: bad_body=" << stats->bad_body.load() << " send_error=" << send_error->load() << "\n";

  server->Shutdown();
  server_thread.Stop();
  client_thread.Stop();
}

} // namespace

int main(int argc, char **argv) {
  int requests = kDefaultRequests;
  int concurrency = kDefaultConcurrency;
  if (argc > 1)
    requests = std::atoi(argv[1]);
  if (argc > 2)
    concurrency = std::atoi(argv[2]);
  if (requests <= 0 || concurrency <= 0) {
    std::cerr << "usage: http2_throughput_bench [requests] [concurrency]\n";
    return 1;
  }
  RunBenchmark(requests, concurrency);
  return 0;
}
