// libnei TCP RTT (Round-Trip Time) benchmark under concurrent load
//
// Server echoes back a short payload; each client sends a ping, waits for
// the pong, and records the round-trip time.  All clients in a batch are
// in-flight simultaneously so that the server experiences real concurrency.
//
// Build: cmake --build build/linux-gcc-release-shared --target tcp_rtt_bench
// Run:   ./build/linux-gcc-release-shared/bench/tcp_rtt_bench [total_connections]

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
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
#include <thread>
#include <vector>

#include <neixx/common/at_exit.h>
#include <neixx/common/location.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tcp_server_socket.h>
#include <neixx/net/wsa_init.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_pool_instance.h>
#include <neixx/threading/thread.h>

namespace {

using Clock = std::chrono::high_resolution_clock;

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

// Simple 4-byte ping / pong payloads.
const char kPing[4] = {'P', 'I', 'N', 'G'};
const char kPong[4] = {'P', 'O', 'N', 'G'};

void RunRttBench(int total_connections) {
  const uint16_t port = FindFreePort();
  if (port == 0) { std::cerr << "ERROR: no free port" << std::endl; return; }

  // ---- Multi-Reactor workers ----
  const int kWorkers = 4;
  std::vector<std::unique_ptr<IoThread>> workers;
  for (int i = 0; i < kWorkers; ++i) {
    workers.push_back(std::make_unique<IoThread>("rtt-wkr-" + std::to_string(i)));
  }

  IoThread acceptor_thread("rtt-acceptor");

  nei::WaitableEvent server_ready(
      nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> server_echoes{0};

  auto server = std::make_shared<nei::net::TCPServerSocket>();

  // Round-robin worker selector.
  auto worker_selector = [&workers, next = 0]() mutable -> nei::scoped_refptr<nei::TaskRunner> {
    int idx = next++ % static_cast<int>(workers.size());
    return workers[idx]->runner();
  };

  // ---- Server: accept -> read ping -> write pong -> close ----
  acceptor_thread.runner()->PostTask(FROM_HERE,
      [&, acc_runner = acceptor_thread.runner(), ws = std::move(worker_selector)]() mutable {
    bool ok = server->Listen(
        nei::net::IPEndPoint(
            nei::net::IPAddress::FromIPv4(127, 0, 0, 1), port),
        total_connections + 100,
        [&](bool success,
            std::unique_ptr<nei::net::TCPClientSocket> accepted) {
          if (!success) return;
          auto conn = std::make_shared<std::unique_ptr<nei::net::TCPClientSocket>>(
              std::move(accepted));
          auto ping_buf = nei::MakeRefCounted<nei::IOBufferWithSize>(4);

          // Read 4-byte ping.
          (*conn)->ReadAsync(ping_buf, 4,
              [conn, ping_buf, &server_echoes](bool ok, size_t n) {
                if (!ok || n != 4) return;
                // Echo back 4-byte pong.
                auto pong_buf = nei::MakeRefCounted<nei::IOBufferWithSize>(4);
                std::memcpy(pong_buf->data(), kPong, 4);
                (*conn)->WriteAsync(pong_buf, 4,
                    [conn, pong_buf, &server_echoes](bool ok2, size_t) {
                      if (ok2) server_echoes.fetch_add(1);
                      (*conn)->Close();
                    });
              });
        },
        acc_runner,
        std::move(ws));
    server_ready.Signal();
    if (!ok) {
      std::cerr << "server Listen failed" << std::endl;
    }
  });

  server_ready.Wait();

  // ---- Clients: connect -> write ping -> read pong (measure RTT) -> close ----
  const int batch_size = 500;
  std::atomic<int> client_done{0};
  std::atomic<int> client_fail{0};
  nei::WaitableEvent all_done(
      nei::WaitableEvent::ResetPolicy::kManual, false);

  // Store RTT measurements (in microseconds).
  std::vector<double> rtts_us;
  rtts_us.reserve(total_connections);
  std::mutex rtts_mutex;

  auto cli_runner = acceptor_thread.runner();

  auto t0 = Clock::now();

  int launched = 0;
  while (launched < total_connections) {
    int batch_count = std::min(batch_size, total_connections - launched);
    auto batch_remaining = std::make_shared<std::atomic<int>>(batch_count);
    auto batch_event = std::make_shared<nei::WaitableEvent>(
        nei::WaitableEvent::ResetPolicy::kAutomatic, false);

    std::vector<std::shared_ptr<nei::net::TCPClientSocket>> batch_clients;

    for (int i = 0; i < batch_count; ++i) {
      auto client = std::make_shared<nei::net::TCPClientSocket>();
      batch_clients.push_back(client);

      auto t_start = std::make_shared<Clock::time_point>();

      client->Connect(
          nei::net::IPEndPoint(
              nei::net::IPAddress::FromIPv4(127, 0, 0, 1), port),
          [client, &client_done, &client_fail, &all_done, total_connections,
           batch_remaining, batch_event, &rtts_us, &rtts_mutex,
           t_start](bool ok) mutable {
            if (!ok) {
              client_fail.fetch_add(1);
              if (client_done.fetch_add(1) + 1 == total_connections) all_done.Signal();
              if (batch_remaining->fetch_sub(1) == 1) batch_event->Signal();
              return;
            }

            *t_start = Clock::now();

            auto ping_buf = nei::MakeRefCounted<nei::IOBufferWithSize>(4);
            std::memcpy(ping_buf->data(), kPing, 4);

            client->WriteAsync(ping_buf, 4,
                [client, &client_done, &client_fail, &all_done, total_connections,
                 batch_remaining, batch_event, &rtts_us, &rtts_mutex,
                 t_start, ping_buf](bool ok_w, size_t) {
                  if (!ok_w) {
                    client_fail.fetch_add(1);
                    if (client_done.fetch_add(1) + 1 == total_connections) all_done.Signal();
                    if (batch_remaining->fetch_sub(1) == 1) batch_event->Signal();
                    return;
                  }

                  auto pong_buf = nei::MakeRefCounted<nei::IOBufferWithSize>(4);

                  client->ReadAsync(pong_buf, 4,
                      [client, &client_done, &client_fail, &all_done, total_connections,
                       batch_remaining, batch_event, &rtts_us, &rtts_mutex,
                       t_start, pong_buf](bool ok_r, size_t n) {
                        if (ok_r && n == 4 &&
                            std::memcmp(pong_buf->data(), kPong, 4) == 0) {
                          auto t_end = Clock::now();
                          double us = std::chrono::duration<double, std::micro>(
                              t_end - *t_start).count();
                          {
                            std::lock_guard<std::mutex> lk(rtts_mutex);
                            rtts_us.push_back(us);
                          }
                        } else {
                          client_fail.fetch_add(1);
                        }

                        client->Close();

                        if (client_done.fetch_add(1) + 1 == total_connections)
                          all_done.Signal();
                        if (batch_remaining->fetch_sub(1) == 1)
                          batch_event->Signal();
                      });
                });
          },
          cli_runner);
    }

    batch_event->Wait();
    batch_clients.clear();
    launched += batch_count;
  }

  // Wait for any stragglers.
  all_done.Wait();

  auto t1 = Clock::now();
  double elapsed = std::chrono::duration<double>(t1 - t0).count();

  // Clean up.
  server->Close();
  server.reset();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  workers.clear();

  int done = client_done.load();
  int fail = client_fail.load();
  int echoes = server_echoes.load();

  std::cout << "  Connections     : " << total_connections << std::endl;
  std::cout << "  Workers         : " << kWorkers << std::endl;
  std::cout << "  Server echoes   : " << echoes << std::endl;
  std::cout << "  Client OK       : " << (done - fail)
            << "  (failures: " << fail << ")" << std::endl;
  std::cout << "  Total elapsed   : "
            << std::fixed << std::setprecision(3) << elapsed << " s"
            << std::endl;

  // ---- RTT statistics ----
  if (!rtts_us.empty()) {
    std::sort(rtts_us.begin(), rtts_us.end());
    double min_us = rtts_us.front();
    double max_us = rtts_us.back();
    double sum_us = 0;
    for (auto v : rtts_us) sum_us += v;
    double avg_us = sum_us / rtts_us.size();

    auto pct = [&](double p) -> double {
      size_t idx = static_cast<size_t>(rtts_us.size() * p / 100.0);
      if (idx >= rtts_us.size()) idx = rtts_us.size() - 1;
      return rtts_us[idx];
    };

    std::cout << "  RTT min / avg / max : "
              << std::fixed << std::setprecision(0)
              << min_us << " / " << avg_us << " / " << max_us << " us"
              << std::endl;
    std::cout << "  RTT p50 / p90 / p99 : "
              << std::fixed << std::setprecision(0)
              << pct(50) << " / " << pct(90) << " / " << pct(99) << " us"
              << std::endl;
  }
}

}  // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
  nei::net::EnsureWsa();
#endif
  nei::AtExitManager at_exit;
  nei::ThreadPoolInstance::CreateAndStart(
      nei::ThreadPoolInstance::InitParams{});

  int total = 1000;
  if (argc > 1) total = std::atoi(argv[1]);
  if (total <= 0) total = 1000;

  std::cout << "=== TCP RTT Under Concurrency ===" << std::endl;
  RunRttBench(total);

  std::cout << std::endl;
  nei::ThreadPoolInstance::Shutdown();
  return 0;
}
