// libnei TCP Cross-System Benchmark  --  WSL ↔ Windows
//
// Measures TCP connection throughput across the WSL2 / Windows boundary.
// The WSL2 virtual NIC (Hyper-V switch) introduces a real kernel boundary
// unlike pure localhost, providing a realistic cross-host test.
//
// Usage:
//   Server:  tcp_cross_bench --server  --port 9000
//   Client:  tcp_cross_bench --client  --host 127.0.0.1  --port 9000  --conn 10000
//
//   WSL server + Windows client:
//     WSL:   ./tcp_cross_bench --server --port 9000
//     Win:   tcp_cross_bench.exe --client --host 127.0.0.1 --port 9000 --conn 10000
//            (WSL2 auto-forwards localhost to the VM)
//
//   Windows server + WSL client:
//     Win:   tcp_cross_bench.exe --server --port 9000
//     WSL:   ./tcp_cross_bench --client --host <Windows IP> --port 9000 --conn 10000
//            (Windows IP = $(grep nameserver /etc/resolv.conf | awk '{print $2}'))

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
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
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

// ---------------------------------------------------------------------------
// IO worker threads
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

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
void RunServer(uint16_t port, bool hold) {
  const int kWorkers = 4;
  std::vector<std::unique_ptr<IoThread>> workers;
  for (int i = 0; i < kWorkers; ++i)
    workers.push_back(std::make_unique<IoThread>("xsvr-wkr-" + std::to_string(i)));

  IoThread acc_thread("xsvr-acceptor");

  auto server = std::make_shared<nei::net::TCPServerSocket>();
  std::atomic<int64_t> accepted{0};
  std::atomic<int64_t> failed{0};
  nei::WaitableEvent ready(nei::WaitableEvent::ResetPolicy::kAutomatic, false);

  // When --hold is set, store accepted connections instead of closing them.
  // This exhausts the process fd limit and forces the reserve-fd EMFILE
  // recovery path to activate in the accept loop.
  std::mutex held_mutex;
  std::vector<std::unique_ptr<nei::net::TCPClientSocket>> held_socks;

  auto t_start = Clock::now();

  // Round-robin worker selector
  auto selector = [&, next = 0]() mutable -> nei::scoped_refptr<nei::TaskRunner> {
    return workers[next++ % kWorkers]->runner();
  };

  acc_thread.runner()->PostTask(FROM_HERE, [&]() {
    bool ok = server->Listen(
        nei::net::IPEndPoint(nei::net::IPAddress::FromIPv4(0, 0, 0, 0), port),
        65535,
        [&](bool success, std::unique_ptr<nei::net::TCPClientSocket> sock) {
          if (success) {
            accepted.fetch_add(1);
            if (hold) {
              std::lock_guard<std::mutex> lock(held_mutex);
              held_socks.push_back(std::move(sock));
            } else {
              sock->Close();
            }
          } else {
            failed.fetch_add(1);
          }
        },
        acc_thread.runner(),
        std::move(selector));
    ready.Signal();
    if (!ok) std::cerr << "FATAL: server Listen() failed" << std::endl;
  });

  ready.Wait();
  std::cout << "[server] listening on 0.0.0.0:" << port
            << "  (workers=" << kWorkers << ", hold=" << (hold ? "yes" : "no")
            << ")" << std::endl;

  if (hold) {
    std::cout << "[server] HOLD mode: accepted connections are NOT closed.\n"
              << "[server] The fd limit will be exhausted, triggering EMFILE.\n"
              << "[server] If the reserve-fd trick works, accepting will slow\n"
              << "[server] but NOT stop — one drain per EMFILE cycle.\n";
  }

  // Print progress every second until SIGINT / client finishes.
  int64_t prev = 0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    int64_t cur = accepted.load();
    int64_t f   = failed.load();
    size_t held = 0;
    {
      std::lock_guard<std::mutex> lock(held_mutex);
      held = held_socks.size();
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       Clock::now() - t_start).count();
    std::cout << "[server] accepted=" << cur
              << "  rate=" << (cur * 1000 / std::max<int64_t>(elapsed, 1))
              << "/s  failed=" << f
              << "  held=" << held;
    if (prev == cur && elapsed > 5000 && cur > 0)
      std::cout << "  (drained)";
    std::cout << std::endl;
    prev = cur;
  }
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
void RunClient(const std::string& host, uint16_t port, int total_conn) {
  nei::net::IPAddress addr = nei::net::IPAddress::FromString(host);
  if (addr.IsUnspecified()) {
    std::cerr << "ERROR: invalid host '" << host << "'" << std::endl;
    return;
  }

  IoThread cli_thread("xcli-io");
  auto runner = cli_thread.runner();

  const int kBatch = 500;
  std::atomic<int> done{0};
  std::atomic<int> fail{0};

  std::cout << "[client] connecting to " << host << ":" << port
            << "  total=" << total_conn << "  batch=" << kBatch << std::endl;

  auto t0 = Clock::now();

  int launched = 0;
  while (launched < total_conn) {
    int n = std::min(kBatch, total_conn - launched);
    auto batch_rem = std::make_shared<std::atomic<int>>(n);
    nei::WaitableEvent batch_done(
        nei::WaitableEvent::ResetPolicy::kAutomatic, false);

    for (int i = 0; i < n; ++i) {
      auto client = std::make_shared<nei::net::TCPClientSocket>();
      client->Connect(
          nei::net::IPEndPoint(addr, port),
          [client, &done, &fail, total_conn, batch_rem, &batch_done](bool ok) {
            if (!ok) fail.fetch_add(1);
            if (done.fetch_add(1) + 1 >= total_conn) {
              // last connection — no extra signal needed beyond batch_done
            }
            if (batch_rem->fetch_sub(1) == 1)
              batch_done.Signal();
          },
          runner);
    }

    batch_done.Wait();
    launched += n;
  }

  auto t1 = Clock::now();
  double elapsed = std::chrono::duration<double>(t1 - t0).count();
  int d = done.load();
  int f = fail.load();

  std::cout << "\n=== Cross-System TCP Benchmark ===\n"
            << "  Direction : client → " << host << ":" << port << "\n"
            << "  Connections: " << total_conn << "\n"
            << "  Successful : " << (d - f) << "\n"
            << "  Failed     : " << f << "\n"
            << "  Elapsed    : " << elapsed << " s\n"
            << "  Rate       : " << static_cast<int>(d / elapsed) << " conn/s\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
void PrintUsage(const char* prog) {
  std::cerr << "Usage:\n"
            << "  Server:  " << prog << " --server  --port <N>  [--hold]\n"
            << "  Client:  " << prog << " --client  --host <IP>  --port <N>  --conn <N>\n"
            << "\n"
            << "Options:\n"
            << "  --hold   Server holds accepted connections open (fd exhaustion test).\n"
            << "           Without --hold, server closes each connection immediately.\n"
            << "\n"
            << "Examples:\n"
            << "  # WSL server + Windows client (normal throughput)\n"
            << "  WSL>  " << prog << " --server --port 9000\n"
            << "  Win>  " << prog << " --client --host 127.0.0.1 --port 9000 --conn 10000\n"
            << "\n"
            << "  # WSL fd exhaustion test (reserve-fd trick verification)\n"
            << "  WSL>  " << prog << " --server --port 9000 --hold\n"
            << "  Win>  " << prog << " --client --host 127.0.0.1 --port 9000 --conn 2000\n"
            << "\n"
            << "  # Windows server + WSL client\n"
            << "  Win>  " << prog << " --server --port 9000\n"
            << "  WSL>  " << prog << " --client --host <Windows-IP> --port 9000 --conn 10000\n"
            << "       (Windows IP = $(grep nameserver /etc/resolv.conf | awk '{print $2}'))\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  nei::AtExitManager at_exit;

  bool server_mode = false;
  bool client_mode = false;
  bool hold = false;
  std::string host = "127.0.0.1";
  uint16_t port = 9000;
  int conn = 10000;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--server") == 0) {
      server_mode = true;
    } else if (std::strcmp(argv[i], "--client") == 0) {
      client_mode = true;
    } else if (std::strcmp(argv[i], "--hold") == 0) {
      hold = true;
    } else if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      host = argv[++i];
    } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--conn") == 0 && i + 1 < argc) {
      conn = std::atoi(argv[++i]);
    } else {
      PrintUsage(argv[0]);
      return 1;
    }
  }

  if (server_mode == client_mode) {
    PrintUsage(argv[0]);
    return 1;
  }

#if defined(_WIN32)
  // Windows: raise ephemeral port range for C10K client-side
  // TcpTimedWaitDelay and MaxUserPort are registry settings — not
  // programmable from user mode.  The benchmark batches connections
  // to avoid exhausting the default ~16K ephemeral ports.
#endif

  if (server_mode) {
    RunServer(port, hold);
  } else {
    RunClient(host, port, conn);
  }

  return 0;
}
