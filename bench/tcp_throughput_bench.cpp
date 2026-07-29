// libnei TCP single-connection loopback throughput benchmark
//
// Measures raw TCP throughput (MB/s) without TLS overhead.
// Verifies data integrity via FNV-1a hash comparison.
//
// Build: cmake --build build/linux-gcc-release-shared --target tcp_throughput_bench
// Run:   ./tcp_throughput_bench [total_MB] [buffer_size]
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

#include <neixx/common/at_exit.h>
#include <neixx/common/location.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tcp_server_socket.h>
#include <neixx/net/wsa_init.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace {

using Clock = std::chrono::high_resolution_clock;

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
  IoThread io("tcp-bench");

  // Deterministic pattern: byte i = (i*37+17) & 0xFF.
  // Pre-compute expected FNV-1a hash for integrity verification.
  auto expected_hash = std::make_shared<uint64_t>(0);
  {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < total_bytes; ++i) {
      h ^= static_cast<unsigned char>((i * 37 + 17) & 0xFF);
      h *= 0x100000001b3ULL;
    }
    *expected_hash = h;
  }
  auto recv_hash = std::make_shared<uint64_t>(0xcbf29ce484222325ULL);
  auto recv_bytes = std::make_shared<std::atomic<size_t>>(0);

  auto bench_done = std::make_shared<nei::WaitableEvent>(
      nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  auto server_ready = std::make_shared<nei::WaitableEvent>(
      nei::WaitableEvent::ResetPolicy::kAutomatic, false);

  auto server = std::make_shared<nei::net::TCPServerSocket>();
  auto client = std::make_shared<nei::net::TCPClientSocket>();

  auto t_start = Clock::now();

  auto runner = io.runner();
  runner->PostTask(FROM_HERE, [=]() mutable {
    bool ok = server->Listen(
        nei::net::IPEndPoint(
            nei::net::IPAddress::FromIPv4(127, 0, 0, 1), port), 1,
        [=](bool success, std::unique_ptr<nei::net::TCPClientSocket> conn) {
          if (!success) { bench_done->Signal(); return; }
          auto sock = std::shared_ptr<nei::net::TCPClientSocket>(conn.release());
          auto do_read = std::make_shared<std::function<void()>>();
          // Use weak_ptr in the outer lambda to break the
          // shared_ptr -> function -> lambda -> shared_ptr cycle.
          // The inner ReadAsync callback holds a strong reference
          // to keep do_read alive while a read is pending.
          std::weak_ptr<std::function<void()>> do_read_weak = do_read;
          *do_read = [=]() {
            auto chunk = nei::MakeRefCounted<nei::IOBufferWithSize>(buffer_size);
            sock->ReadAsync(chunk, buffer_size,
                [=, dr = do_read_weak.lock()](bool s, size_t n) {
                  if (!dr) return;
                  if (!s) { bench_done->Signal(); return; }
                  if (n == 0) { sock->Close(); bench_done->Signal(); return; }
                  recv_bytes->fetch_add(n);
                  uint64_t h = *recv_hash;
                  for (size_t i = 0; i < n; ++i) {
                    h ^= static_cast<unsigned char>(chunk->data()[i]);
                    h *= 0x100000001b3ULL;
                  }
                  *recv_hash = h;
                  (*dr)();
                });
          };
          (*do_read)();
        },
        runner);
    if (!ok) { bench_done->Signal(); return; }
    server_ready->Signal();

    client->Connect(
        nei::net::IPEndPoint(
            nei::net::IPAddress::FromIPv4(127, 0, 0, 1), port),
        [=](bool ok) {
          if (!ok) { bench_done->Signal(); return; }
          auto offset = std::make_shared<size_t>(0);
          auto do_write = std::make_shared<std::function<void()>>();
          // Same weak_ptr pattern as do_read above.
          std::weak_ptr<std::function<void()>> do_write_weak = do_write;
          *do_write = [=]() {
            size_t remain = total_bytes - *offset;
            if (remain == 0) {
              client->Close();
              return;
            }
            auto chunk = nei::MakeRefCounted<nei::IOBufferWithSize>(
                std::min(remain, buffer_size));
            for (size_t i = 0; i < chunk->size(); ++i)
              chunk->data()[i] = static_cast<unsigned char>(((*offset + i) * 37 + 17) & 0xFF);
            client->WriteAsync(chunk, chunk->size(),
                [=, dw = do_write_weak.lock()](bool s, size_t n) {
                  if (!dw) return;
                  if (!s) { bench_done->Signal(); return; }
                  *offset += n;
                  (*dw)();
                });
          };
          (*do_write)();
        },
        runner);
  });

  server_ready->Wait();
  if (!bench_done->TimedWait(std::chrono::seconds(600))) {
    std::cerr << "ERROR: benchmark timed out (600s)" << std::endl;
    return;
  }

  auto t_end = Clock::now();
  double elapsed = std::chrono::duration<double>(t_end - t_start).count();
  size_t received = recv_bytes->load();
  double rate = received / elapsed / (1024 * 1024);
  std::cout << "\n=== TCP Throughput Benchmark ===\n"
            << "  Data       : " << (total_bytes >> 20) << " MB\n"
            << "  Buffer     : " << (buffer_size >> 10) << " KB\n"
            << "  Elapsed    : " << std::fixed << std::setprecision(3)
            << elapsed << " s\n"
            << "  Throughput : " << std::fixed << std::setprecision(1)
            << rate << " MB/s\n";

  bool ok = (*recv_hash == *expected_hash) && (received == total_bytes);
  if (!ok) {
    std::cerr << "ERROR: integrity check FAILED"
              << "  sent=" << total_bytes << "  recv=" << received
              << "  expected_hash=0x" << std::hex << *expected_hash
              << "  actual_hash=0x" << *recv_hash << std::dec << std::endl;
  } else {
    std::cout << "  Integrity   : OK (FNV-1a hash match)" << std::endl;
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  nei::AtExitManager at_exit;

  size_t total_mb = 10;
  size_t buffer_kb = 64;

  if (argc > 1) total_mb = static_cast<size_t>(std::atoll(argv[1]));
  if (argc > 2) buffer_kb = static_cast<size_t>(std::atoll(argv[2]));

  if (total_mb == 0 || total_mb > 8192) {
    std::cerr << "Usage: " << argv[0] << " [total_MB=10] [buffer_KB=64]\n"
              << "  total_MB: 1..8192 (default 10)\n"
              << "  buffer_KB: 4..1024 (default 64)\n";
    return 1;
  }

  RunBenchmark(total_mb * 1024ULL * 1024ULL, buffer_kb * 1024ULL);
  return 0;
}
