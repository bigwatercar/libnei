// libnei TCP connection stress benchmark — concurrent connect storm (C10K)
//
// Server accepts and immediately closes; all clients connect concurrently.
// Measures connection setup/teardown stability under concurrent load.
//
// Build: cmake --build build/linux-gcc-release-shared --target tcp_conn_stress_bench
// Run:   ./build/linux-gcc-release-shared/bench/tcp_conn_stress_bench [total_connections]

// winsock2.h must come before any header that might include windows.h.
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
#include <thread>
#include <vector>

#include <neixx/common/at_exit.h>
#include <neixx/common/location.h>
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

void RunStressTest(int total_connections) {
  const uint16_t port = FindFreePort();
  if (port == 0) { std::cerr << "ERROR: no free port" << std::endl; return; }

  // Use multiple IO worker threads via Multi-Reactor so the accept
  // load is spread across workers and the listen backlog doesn't
  // become the bottleneck.
  const int kWorkers = 4;
  std::vector<std::unique_ptr<IoThread>> workers;
  for (int i = 0; i < kWorkers; ++i) {
    workers.push_back(std::make_unique<IoThread>("stress-wkr-" + std::to_string(i)));
  }

  IoThread acceptor_thread("stress-acceptor");

  nei::WaitableEvent server_ready(
      nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> server_accepted{0};
  std::atomic<int> server_failed{0};

  auto server = std::make_shared<nei::net::TCPServerSocket>();

  // Round-robin worker selector for Multi-Reactor dispatch.
  auto worker_selector = [&workers, next = 0]() mutable -> nei::scoped_refptr<nei::TaskRunner> {
    int idx = next++ % static_cast<int>(workers.size());
    return workers[idx]->runner();
  };

  // ---- Server: accept all connections (Multi-Reactor) ----
  acceptor_thread.runner()->PostTask(FROM_HERE,
      [&, acc_runner = acceptor_thread.runner(), ws = std::move(worker_selector)]() mutable {
    bool ok = server->Listen(
        nei::net::IPEndPoint(
            nei::net::IPAddress::FromIPv4(127, 0, 0, 1), port),
        total_connections + 100,
        [&](bool success,
            std::unique_ptr<nei::net::TCPClientSocket> accepted) {
          if (!success) {
            server_failed.fetch_add(1);
            return;
          }
          server_accepted.fetch_add(1);
          accepted->Close();
        },
        acc_runner,
        std::move(ws));
    server_ready.Signal();
    if (!ok) {
      std::cerr << "server Listen failed" << std::endl;
    }
  });

  server_ready.Wait();

  // ---- Client: fire connections in concurrent batches ----
  // Firing all N connections at once overflows the TCP listen backlog.
  // Instead, keep up to |batch_size| connections in-flight at a time
  // so that the kernel queue + AcceptEx pipeline absorb the load.
  const int batch_size = 500;
  std::atomic<int> client_done{0};
  std::atomic<int> client_fail{0};
  nei::WaitableEvent all_done(
      nei::WaitableEvent::ResetPolicy::kManual, false);

  // Use the acceptor thread's runner for client-side Connect I/O.
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

      client->Connect(
          nei::net::IPEndPoint(
              nei::net::IPAddress::FromIPv4(127, 0, 0, 1), port),
          [client, &client_done, &client_fail, &all_done, total_connections,
           batch_remaining, batch_event](bool ok) {
            if (!ok) {
              client_fail.fetch_add(1);
            }
            if (client_done.fetch_add(1) + 1 == total_connections) {
              all_done.Signal();
            }
            if (batch_remaining->fetch_sub(1) == 1) {
              batch_event->Signal();
            }
          },
          cli_runner);
    }

    batch_event->Wait();
    batch_clients.clear();
    launched += batch_count;
  }

  // Wait for all clients to finish.
  all_done.Wait();

  auto t1 = Clock::now();
  double elapsed = std::chrono::duration<double>(t1 - t0).count();

  // Clean up.
  server->Close();
  server.reset();


  // Drain remaining I/O tasks.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Stop workers before logging (clean shutdown order).
  workers.clear();

  int accepted = server_accepted.load();
  int srv_fail = server_failed.load();
  int cli_done = client_done.load();
  int cli_fail = client_fail.load();

  std::cout << "  Connections   : " << total_connections << std::endl;
  std::cout << "  Workers       : " << kWorkers << std::endl;
  std::cout << "  Server accepts: " << accepted
            << "  (failures: " << srv_fail << ")" << std::endl;
  std::cout << "  Client done   : " << cli_done
            << "  (failures: " << cli_fail << ")" << std::endl;
  std::cout << "  Elapsed       : "
            << std::fixed << std::setprecision(3) << elapsed << " s"
            << std::endl;
  if (elapsed > 0.001) {
    std::cout << "  Rate          : "
              << std::fixed << std::setprecision(1)
              << (total_connections / elapsed) << " conn/s"
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

  std::cout << "=== TCP Connection Stress ===" << std::endl;
  RunStressTest(total);

  std::cout << std::endl;
  nei::ThreadPoolInstance::Shutdown();
  return 0;
}
