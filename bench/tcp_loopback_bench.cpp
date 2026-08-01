// libnei TCP single-connection loopback throughput benchmark
//
// Server and client run on separate IO threads to maximize throughput.
// Measures MB/s for buffer sizes from 4 KB to 1 MB.
//
// Build: cmake --build build/linux-gcc-release-shared --target tcp_loopback_bench
// Run:   ./build/linux-gcc-release-shared/bench/tcp_loopback_bench [total_MB]

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
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
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
  explicit IoThread(const std::string &name) {
    nei::Thread::Options opts;
    opts.message_pump_type = nei::MessagePumpType::IO;
    thread_ = std::make_unique<nei::Thread>(name);
    thread_->StartWithOptions(opts);
    runner_ = thread_->GetTaskRunner();
  }

  ~IoThread() {
    thread_->Stop();
  }

  nei::scoped_refptr<nei::SingleThreadTaskRunner> runner() const {
    return runner_;
  }

private:
  std::unique_ptr<nei::Thread> thread_;
  nei::scoped_refptr<nei::SingleThreadTaskRunner> runner_;
};

static uint16_t FindFreePort() {
#if defined(_WIN32)
  nei::net::EnsureWsa();
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

void RunBenchmark(size_t buffer_size, size_t total_mb) {
  const size_t total_bytes = total_mb * 1024ULL * 1024ULL;
  const uint16_t port = FindFreePort();
  if (port == 0) {
    std::cerr << "ERROR: no free port" << std::endl;
    return;
  }

  IoThread srv_thread("bench-srv");
  IoThread cli_thread("bench-cli");

  // All shared state is heap-allocated via shared_ptr so that IO thread
  // callbacks never reference stack variables (avoids use-after-free on
  // early return / timeout).
  auto bench_done = std::make_shared<nei::WaitableEvent>(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  auto server_bytes = std::make_shared<std::atomic<size_t>>(0);

  auto server = std::make_shared<nei::net::TCPServerSocket>();
  auto client = std::make_shared<nei::net::TCPClientSocket>();

  // ---- Server: accept + drain ----
  srv_thread.runner()->PostTask(
      FROM_HERE, [server, port, buffer_size, bench_done, server_bytes, srv_runner = srv_thread.runner()]() {
        bool ok = server->Listen(
            nei::net::IPEndPoint(nei::net::IPAddress::FromIPv4(127, 0, 0, 1), port),
            1,
            [bench_done, server_bytes, buffer_size, srv_runner](bool success,
                                                                std::unique_ptr<nei::net::TCPClientSocket> acc) {
              if (!success) {
                bench_done->Signal();
                return;
              }
              // Transfer ownership from unique_ptr to shared_ptr.
              auto sock = std::shared_ptr<nei::net::TCPClientSocket>(std::move(acc));
              auto buf = nei::MakeRefCounted<nei::IOBufferWithSize>(buffer_size);
              auto total = std::make_shared<std::atomic<size_t>>(0);
              auto do_read = std::make_shared<std::function<void()>>();
              // The do_read lambda captures a weak_ptr to itself (breaks cycle),
              // but the inner ReadAsync callback captures a strong shared_ptr to
              // keep do_read alive while a read is pending.
              std::weak_ptr<std::function<void()>> do_read_weak = do_read;
              *do_read = [bench_done, server_bytes, sock, buf, total, do_read_weak, buffer_size]() {
                sock->ReadAsync(buf,
                                buffer_size,
                                [bench_done, server_bytes, total, do_read = do_read_weak.lock()](bool s, size_t n) {
                                  // If do_read is gone, we are shutting down — bail out.
                                  if (!do_read)
                                    return;
                                  if (!s || n == 0) {
                                    server_bytes->store(total->load());
                                    bench_done->Signal();
                                    return;
                                  }
                                  total->fetch_add(n);
                                  (*do_read)();
                                });
              };
              (*do_read)();
            },
            srv_runner);
        if (!ok) {
          std::cerr << "server Listen failed" << std::endl;
          bench_done->Signal();
        }
      });

  // ---- Client: connect + write until done ----
  auto write_buf = nei::MakeRefCounted<nei::IOBufferWithSize>(buffer_size);
  std::memset(write_buf->data(), 0, buffer_size);

  cli_thread.runner()->PostTask(
      FROM_HERE,
      [client,
       port,
       buffer_size,
       total_bytes,
       bench_done,
       server_bytes,
       write_buf,
       cli_runner = cli_thread.runner()]() {
        client->Connect(
            nei::net::IPEndPoint(nei::net::IPAddress::FromIPv4(127, 0, 0, 1), port),
            [bench_done, server_bytes, client, write_buf, total_bytes, buffer_size](bool ok) {
              if (!ok) {
                std::cerr << "connect failed" << std::endl;
                bench_done->Signal();
                return;
              }
              auto rem = std::make_shared<std::atomic<size_t>>(total_bytes);
              auto dw = std::make_shared<std::function<void()>>();
              // Same pattern: weak self-reference in the outer lambda,
              // strong reference in the inner WriteAsync callback.
              std::weak_ptr<std::function<void()>> dw_weak = dw;
              *dw = [bench_done, client, write_buf, rem, dw_weak, buffer_size]() {
                size_t chunk = std::min(buffer_size, rem->load());
                if (chunk == 0) {
                  client->Close();
                  return;
                }
                client->WriteAsync(write_buf, chunk, [bench_done, rem, dw = dw_weak.lock()](bool s, size_t n) {
                  if (!dw)
                    return;
                  if (!s) {
                    bench_done->Signal();
                    return;
                  }
                  rem->fetch_sub(n);
                  (*dw)();
                });
              };
              (*dw)();
            },
            cli_runner);
      });

  // Timeout scales with data size.  The 4KB buffer is the slowest path;
  // at ~80 MB/s (Debug ASAN), 1 GB takes ~12 s.  Use a conservative
  // minimum of 120 s and scale at 30 s/GB (roughly 3× margin for Debug).
  auto timeout_s = std::max(120, static_cast<int>(total_mb * 30 / 1024));
  auto t0 = Clock::now();
  if (!bench_done->TimedWait(std::chrono::seconds(timeout_s))) {
    std::cerr << "  ERROR: benchmark timed out (" << timeout_s << "s)" << std::endl;
    return;
  }
  auto t1 = Clock::now();

  double elapsed = std::chrono::duration<double>(t1 - t0).count();
  double mb = static_cast<double>(server_bytes->load()) / (1024.0 * 1024.0);
  double mbps = (elapsed > 0.001) ? mb / elapsed : 0.0;

  std::cout << "  " << std::setw(7) << (buffer_size / 1024) << " KB  |  " << std::setw(6) << static_cast<int>(mb)
            << " MB  |  " << std::setw(7) << std::fixed << std::setprecision(3) << elapsed << " s |  " << std::setw(9)
            << std::fixed << std::setprecision(1) << mbps << " MB/s" << std::endl;
}

} // namespace

int main(int argc, char *argv[]) {
#if defined(_WIN32)
  nei::net::EnsureWsa();
#endif
  nei::AtExitManager at_exit;

  // ThreadPoolInstance is not used by this benchmark (it creates its own
  // IoThreads).  Keeping a pool alive leaks its internal TaskQueues and
  // WeakPtrFactory bookkeeping (~3 MB across repeated runs).
  // nei::ThreadPoolInstance::CreateAndStart(
  //     nei::ThreadPoolInstance::InitParams{});

  size_t total_mb = 1024;
  if (argc > 1)
    total_mb = static_cast<size_t>(std::atoi(argv[1]));
  if (total_mb == 0)
    total_mb = 1024;

  std::cout << "=== TCP Loopback Throughput ===" << std::endl;
  std::cout << "Total per run: " << total_mb << " MB" << std::endl;
  std::cout << std::endl;
  std::cout << "  Buffer  |  Data   |  Elapsed  |  Throughput" << std::endl;
  std::cout << "----------|---------|-----------|------------" << std::endl;

  std::vector<size_t> sizes = {4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576};
  for (auto sz : sizes)
    RunBenchmark(sz, total_mb);

  std::cout << std::endl;
  // nei::ThreadPoolInstance::Shutdown();
  return 0;
}
