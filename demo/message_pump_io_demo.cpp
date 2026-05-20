#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <neixx/command_line/command_line.h>
#include <neixx/task/message_loop/message_pump_io.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <errno.h>
#include <unistd.h>
#endif

namespace {

void PrintUsageAndCommandLineSnapshot(const nei::CommandLine& command_line) {
  std::cout << "Usage:" << std::endl;
  std::cout << "  message_pump_io_demo [--help]" << std::endl;
  std::cout << "  message_pump_io_demo [--burst] [--receivers=<N>] [--per-receiver=<N>]"
            << std::endl;
  std::cout << std::endl;
  std::cout << "Options:" << std::endl;
  std::cout << "  --help, --h, --?       Show this help and exit." << std::endl;
  std::cout << "  --burst                Enable burst benchmark-style mode (Windows path)."
            << std::endl;
  std::cout << "  --receivers=<N>        Number of concurrent receive sockets (default: 4)."
            << std::endl;
  std::cout << "  --per-receiver=<N>     Packets per receiver (default: 250)."
            << std::endl;
  std::cout << std::endl;
  std::cout << "CommandLine snapshot:" << std::endl;
  std::cout << "  full: " << command_line.GetCommandLineString() << std::endl;
  std::cout << "  switches:" << std::endl;
  const auto& switches = command_line.GetSwitches();
  if (switches.empty()) {
    std::cout << "    (none)" << std::endl;
  } else {
    for (const auto& kv : switches) {
      std::string key(kv.first.begin(), kv.first.end());
      std::string value(kv.second.begin(), kv.second.end());
      std::cout << "    --" << key;
      if (!value.empty()) {
        std::cout << "=" << value;
      }
      std::cout << std::endl;
    }
  }

  const std::vector<std::string> args = command_line.GetArgs();
  std::cout << "  positional args (" << args.size() << "):" << std::endl;
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::cout << "    [" << i << "] " << args[i] << std::endl;
  }
}

class DemoDelegate final : public nei::MessagePump::Delegate {
 public:
  explicit DemoDelegate(nei::MessagePumpForIO* pump) : pump_(pump) {}

  bool DoWork() override {
    if (should_quit_.load(std::memory_order_acquire) && pump_ != nullptr) {
      std::cout << "[DoWork] Quit requested, stopping run loop." << std::endl;
      pump_->Quit();
      return true;
    }

    if (has_run_work_) {
      return false;
    }
    has_run_work_ = true;
    std::cout << "[DoWork] Cross-thread wakeup received." << std::endl;
    return true;
  }

  bool DoDelayedWork(NextWorkInfo* next_work_info) override {
    if (next_work_info != nullptr) {
      next_work_info->next_run_time = NextWorkInfo::kNoScheduledRunTime;
      next_work_info->recent_now = nei::TimeTicks();
    }
    return false;
  }

  bool DoIdleWork() override {
    return false;
  }

  void RequestQuit() {
    should_quit_.store(true, std::memory_order_release);
  }

 private:
  nei::MessagePumpForIO* pump_ = nullptr;
  bool has_run_work_ = false;
  std::atomic<bool> should_quit_{false};
};

#if defined(_WIN32)
class UdpIocpWatcher final : public nei::MessagePumpForIO::Watcher {
 public:
  UdpIocpWatcher(nei::MessagePumpForIO* pump,
                 SOCKET recv_socket,
                 OVERLAPPED* overlapped,
                 char* recv_buf,
                 std::atomic<bool>* completion_seen)
      : pump_(pump),
        recv_socket_(recv_socket),
        overlapped_(overlapped),
        recv_buf_(recv_buf),
        completion_seen_(completion_seen) {}

  void OnFileCanReadWithoutBlocking(nei::NativeIOHandle /*handle*/) override {
    DWORD transferred = 0;
    DWORD flags = 0;
    const BOOL ok = WSAGetOverlappedResult(recv_socket_, overlapped_,
                                           &transferred, FALSE, &flags);
    if (ok) {
      completion_seen_->store(true, std::memory_order_release);
      std::cout << "[IOCP] Overlapped completion received, bytes="
                << transferred;
      if (transferred > 0) {
        std::cout << ", first_byte="
                  << static_cast<int>(static_cast<unsigned char>(recv_buf_[0]));
      }
      std::cout << std::endl;
      if (pump_ != nullptr) {
        pump_->Quit();
      }
      return;
    }

    const int err = WSAGetLastError();
    std::cout << "[IOCP] Callback arrived but WSAGetOverlappedResult failed, err="
              << err << std::endl;
    if (pump_ != nullptr) {
      pump_->Quit();
    }
  }

  void OnFileCanWriteWithoutBlocking(nei::NativeIOHandle /*handle*/) override {
    std::cout << "[IOCP] Writable callback received (unused in this demo)."
              << std::endl;
  }

 private:
  nei::MessagePumpForIO* pump_ = nullptr;
  SOCKET recv_socket_ = INVALID_SOCKET;
  OVERLAPPED* overlapped_ = nullptr;
  char* recv_buf_ = nullptr;
  std::atomic<bool>* completion_seen_ = nullptr;
};

struct BurstStats {
  std::atomic<int> total_completed{0};
  std::atomic<int> total_bytes{0};
  std::atomic<int> completion_errors{0};
  std::atomic<bool> has_started{false};
  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point end_time;
};

class BurstReceiverWatcher final : public nei::MessagePumpForIO::Watcher {
 public:
  BurstReceiverWatcher(nei::MessagePumpForIO* pump,
                       SOCKET recv_socket,
                       int target_count,
                       int total_expected,
                       BurstStats* stats)
      : pump_(pump),
        recv_socket_(recv_socket),
        target_count_(target_count),
        total_expected_(total_expected),
        stats_(stats) {
    wsa_buf_.buf = recv_buf_;
    wsa_buf_.len = static_cast<ULONG>(sizeof(recv_buf_));
  }

  bool ArmReceive() {
    std::memset(&overlapped_, 0, sizeof(overlapped_));
    DWORD flags = 0;
    DWORD bytes_received = 0;
    from_len_ = static_cast<int>(sizeof(from_addr_));
    const int recv_ret = WSARecvFrom(
        recv_socket_, &wsa_buf_, 1, &bytes_received, &flags,
        reinterpret_cast<sockaddr*>(&from_addr_), &from_len_, &overlapped_, nullptr);
    return recv_ret == 0 || (recv_ret == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING);
  }

  void OnFileCanReadWithoutBlocking(nei::NativeIOHandle /*handle*/) override {
    DWORD transferred = 0;
    DWORD flags = 0;
    const BOOL ok = WSAGetOverlappedResult(recv_socket_, &overlapped_,
                                           &transferred, FALSE, &flags);
    if (!ok) {
      stats_->completion_errors.fetch_add(1, std::memory_order_acq_rel);
      if (pump_ != nullptr) {
        pump_->Quit();
      }
      return;
    }

    if (!stats_->has_started.exchange(true, std::memory_order_acq_rel)) {
      stats_->start_time = std::chrono::steady_clock::now();
    }

    ++completed_count_;
    stats_->total_bytes.fetch_add(static_cast<int>(transferred), std::memory_order_acq_rel);
    const int total = stats_->total_completed.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (completed_count_ < target_count_) {
      if (!ArmReceive()) {
        stats_->completion_errors.fetch_add(1, std::memory_order_acq_rel);
        if (pump_ != nullptr) {
          pump_->Quit();
        }
      }
      return;
    }

    if (total >= total_expected_ && pump_ != nullptr) {
      stats_->end_time = std::chrono::steady_clock::now();
      pump_->Quit();
    }
  }

  void OnFileCanWriteWithoutBlocking(nei::NativeIOHandle /*handle*/) override {}

 private:
  nei::MessagePumpForIO* pump_ = nullptr;
  SOCKET recv_socket_ = INVALID_SOCKET;
  int target_count_ = 0;
  int total_expected_ = 0;
  int completed_count_ = 0;
  BurstStats* stats_ = nullptr;

  OVERLAPPED overlapped_{};
  WSABUF wsa_buf_{};
  char recv_buf_[256] = {};
  sockaddr_in from_addr_{};
  int from_len_ = 0;
};

int ParsePositiveOrDefault(const char* text, int default_value) {
  if (text == nullptr || text[0] == '\0') {
    return default_value;
  }
  const int parsed = std::atoi(text);
  return parsed > 0 ? parsed : default_value;
}

int ReadPositiveSwitchOrDefault(const nei::CommandLine& command_line,
                                const char* switch_name,
                                int default_value) {
  const std::string value = command_line.GetSwitchValueASCII(switch_name);
  if (value.empty()) {
    return default_value;
  }
  return ParsePositiveOrDefault(value.c_str(), default_value);
}
#endif

#if !defined(_WIN32)
class PipeReadWatcher final : public nei::MessagePumpForIO::Watcher {
 public:
  explicit PipeReadWatcher(nei::MessagePumpForIO* pump) : pump_(pump) {}

  void OnFileCanReadWithoutBlocking(nei::NativeIOHandle handle) override {
    std::uint8_t byte = 0;
    const ssize_t n = read(static_cast<int>(handle), &byte, sizeof(byte));
    if (n == static_cast<ssize_t>(sizeof(byte))) {
      std::cout << "[IO] Read event fired, byte=" << static_cast<int>(byte)
                << std::endl;
    } else {
      std::cout << "[IO] Read callback fired but read failed, errno=" << errno
                << std::endl;
    }

    if (pump_ != nullptr) {
      std::cout << "[IO] Quitting run loop after first readable event."
                << std::endl;
      pump_->Quit();
    }
  }

  void OnFileCanWriteWithoutBlocking(nei::NativeIOHandle /*handle*/) override {
    std::cout << "[IO] Writable callback received (unused in this demo)."
              << std::endl;
  }

 private:
  nei::MessagePumpForIO* pump_ = nullptr;
};
#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  nei::CommandLine::Init();
#else
  nei::CommandLine::Init(argc, argv);
#endif
  nei::CommandLine& command_line = nei::CommandLine::ForCurrentProcess();

  if (command_line.HasSwitch("help") || command_line.HasSwitch("h") ||
      command_line.HasSwitch("?")) {
    PrintUsageAndCommandLineSnapshot(command_line);
    return 0;
  }

  nei::MessagePumpForIO pump;
  DemoDelegate delegate(&pump);

#if defined(_WIN32)
  bool burst_mode = command_line.HasSwitch("burst");
  int burst_receivers = 4;
  int burst_per_receiver = 250;
  burst_receivers = ReadPositiveSwitchOrDefault(command_line, "receivers", burst_receivers);
  burst_per_receiver = ReadPositiveSwitchOrDefault(command_line, "per-receiver", burst_per_receiver);

  std::cout << "MessagePumpForIO demo (Windows path):" << std::endl;
  std::cout << "- Command line: " << command_line.GetCommandLineString()
            << std::endl;
  std::cout << "- Parsed switches: burst=" << (burst_mode ? "true" : "false")
            << ", receivers=" << burst_receivers
            << ", per-receiver=" << burst_per_receiver << std::endl;
  std::cout << "- Switch value format: use --receivers=<N> --per-receiver=<N>."
            << std::endl;
  std::cout << "- Demonstrates cross-thread ScheduleWork wakeup." << std::endl;
  if (burst_mode) {
    std::cout << "- Burst mode: concurrent overlapped UDP recv + throughput stats."
              << std::endl;
  } else {
    std::cout << "- Demonstrates real watched-handle callback via IOCP + overlapped UDP recv."
              << std::endl;
  }

  WSADATA wsa_data{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    std::cerr << "WSAStartup failed" << std::endl;
    return 1;
  }

  if (!burst_mode) {
    SOCKET recv_socket = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP,
                                    nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (recv_socket == INVALID_SOCKET) {
      std::cerr << "WSASocket(recv) failed, err=" << WSAGetLastError() << std::endl;
      WSACleanup();
      return 2;
    }

    sockaddr_in recv_addr{};
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_port = htons(0);
    recv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(recv_socket, reinterpret_cast<const sockaddr*>(&recv_addr),
             static_cast<int>(sizeof(recv_addr))) == SOCKET_ERROR) {
      std::cerr << "bind(recv) failed, err=" << WSAGetLastError() << std::endl;
      closesocket(recv_socket);
      WSACleanup();
      return 3;
    }

    int recv_addr_len = static_cast<int>(sizeof(recv_addr));
    if (getsockname(recv_socket, reinterpret_cast<sockaddr*>(&recv_addr),
                    &recv_addr_len) == SOCKET_ERROR) {
      std::cerr << "getsockname(recv) failed, err=" << WSAGetLastError() << std::endl;
      closesocket(recv_socket);
      WSACleanup();
      return 4;
    }

    SOCKET send_socket = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP,
                                    nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (send_socket == INVALID_SOCKET) {
      std::cerr << "WSASocket(send) failed, err=" << WSAGetLastError() << std::endl;
      closesocket(recv_socket);
      WSACleanup();
      return 5;
    }

    char recv_buf[64] = {};
    WSABUF wsa_buf{};
    wsa_buf.buf = recv_buf;
    wsa_buf.len = static_cast<ULONG>(sizeof(recv_buf));
    OVERLAPPED overlapped{};
    DWORD recv_flags = 0;
    DWORD bytes_received = 0;
    sockaddr_in from_addr{};
    int from_len = static_cast<int>(sizeof(from_addr));

    std::atomic<bool> completion_seen{false};
    nei::MessagePumpForIO::FdWatchController controller;
    UdpIocpWatcher watcher(&pump, recv_socket, &overlapped, recv_buf,
                           &completion_seen);
    if (!controller.StartWatching(
            &pump, reinterpret_cast<nei::NativeIOHandle>(recv_socket),
            nei::MessagePumpForIO::FdWatchController::Mode::READ, &watcher)) {
      std::cerr << "StartWatching(recv_socket) failed" << std::endl;
      closesocket(send_socket);
      closesocket(recv_socket);
      WSACleanup();
      return 6;
    }

    const int recv_ret = WSARecvFrom(
        recv_socket, &wsa_buf, 1, &bytes_received, &recv_flags,
        reinterpret_cast<sockaddr*>(&from_addr), &from_len, &overlapped, nullptr);
    if (recv_ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
      std::cerr << "WSARecvFrom failed, err=" << WSAGetLastError() << std::endl;
      controller.StopWatching();
      closesocket(send_socket);
      closesocket(recv_socket);
      WSACleanup();
      return 7;
    }

    std::thread producer([&pump, &delegate, send_socket, recv_addr, &completion_seen]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::cout << "[Producer] ScheduleWork #1" << std::endl;
      pump.ScheduleWork();

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const char msg = 42;
      const int send_ret = sendto(
          send_socket, &msg, 1, 0,
          reinterpret_cast<const sockaddr*>(&recv_addr),
          static_cast<int>(sizeof(recv_addr)));
      if (send_ret == 1) {
        std::cout << "[Producer] Sent one UDP byte to trigger overlapped completion."
                  << std::endl;
      } else {
        std::cout << "[Producer] sendto failed, err=" << WSAGetLastError()
                  << std::endl;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (!completion_seen.load(std::memory_order_acquire)) {
        std::cout << "[Producer] Completion not observed, force quit via ScheduleWork."
                  << std::endl;
        delegate.RequestQuit();
        pump.ScheduleWork();
      }
    });

    pump.Run(&delegate);
    producer.join();

    controller.StopWatching();
    closesocket(send_socket);
    closesocket(recv_socket);
    WSACleanup();
  } else {
    if (burst_receivers > 32) {
      burst_receivers = 32;
    }
    const int total_expected = burst_receivers * burst_per_receiver;
    std::cout << "[Burst] receivers=" << burst_receivers
              << ", per_receiver=" << burst_per_receiver
              << ", expected_packets=" << total_expected << std::endl;

    std::vector<SOCKET> recv_sockets;
    std::vector<sockaddr_in> recv_addrs;
    std::vector<nei::MessagePumpForIO::FdWatchController> controllers(burst_receivers);
    std::vector<std::unique_ptr<BurstReceiverWatcher>> watchers;
    recv_sockets.reserve(static_cast<std::size_t>(burst_receivers));
    recv_addrs.reserve(static_cast<std::size_t>(burst_receivers));
    watchers.reserve(static_cast<std::size_t>(burst_receivers));

    BurstStats stats;

    SOCKET send_socket = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP,
                                    nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (send_socket == INVALID_SOCKET) {
      std::cerr << "WSASocket(send) failed, err=" << WSAGetLastError() << std::endl;
      WSACleanup();
      return 8;
    }

    for (int i = 0; i < burst_receivers; ++i) {
      SOCKET recv_socket = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP,
                                      nullptr, 0, WSA_FLAG_OVERLAPPED);
      if (recv_socket == INVALID_SOCKET) {
        std::cerr << "WSASocket(recv) failed, err=" << WSAGetLastError() << std::endl;
        closesocket(send_socket);
        for (SOCKET s : recv_sockets) {
          closesocket(s);
        }
        WSACleanup();
        return 9;
      }

      sockaddr_in recv_addr{};
      recv_addr.sin_family = AF_INET;
      recv_addr.sin_port = htons(0);
      recv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (bind(recv_socket, reinterpret_cast<const sockaddr*>(&recv_addr),
               static_cast<int>(sizeof(recv_addr))) == SOCKET_ERROR) {
        std::cerr << "bind(recv) failed, err=" << WSAGetLastError() << std::endl;
        closesocket(recv_socket);
        closesocket(send_socket);
        for (SOCKET s : recv_sockets) {
          closesocket(s);
        }
        WSACleanup();
        return 10;
      }

      int recv_addr_len = static_cast<int>(sizeof(recv_addr));
      if (getsockname(recv_socket, reinterpret_cast<sockaddr*>(&recv_addr),
                      &recv_addr_len) == SOCKET_ERROR) {
        std::cerr << "getsockname(recv) failed, err=" << WSAGetLastError() << std::endl;
        closesocket(recv_socket);
        closesocket(send_socket);
        for (SOCKET s : recv_sockets) {
          closesocket(s);
        }
        WSACleanup();
        return 11;
      }

      recv_sockets.push_back(recv_socket);
      recv_addrs.push_back(recv_addr);
    }

    bool setup_ok = true;
    for (int i = 0; i < burst_receivers; ++i) {
      watchers.push_back(std::make_unique<BurstReceiverWatcher>(
          &pump, recv_sockets[static_cast<std::size_t>(i)], burst_per_receiver,
          total_expected, &stats));

      if (!controllers[static_cast<std::size_t>(i)].StartWatching(
              &pump,
              reinterpret_cast<nei::NativeIOHandle>(
                  recv_sockets[static_cast<std::size_t>(i)]),
              nei::MessagePumpForIO::FdWatchController::Mode::READ,
              watchers[static_cast<std::size_t>(i)].get())) {
        std::cerr << "StartWatching failed at index=" << i << std::endl;
        setup_ok = false;
        break;
      }

      if (!watchers[static_cast<std::size_t>(i)]->ArmReceive()) {
        std::cerr << "ArmReceive failed at index=" << i << std::endl;
        setup_ok = false;
        break;
      }
    }

    if (!setup_ok) {
      for (auto& c : controllers) {
        c.StopWatching();
      }
      closesocket(send_socket);
      for (SOCKET s : recv_sockets) {
        closesocket(s);
      }
      WSACleanup();
      return 12;
    }

    std::thread producer([&pump, &delegate, send_socket, recv_addrs, burst_per_receiver, &stats]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      pump.ScheduleWork();

      const char msg = 7;
      for (int round = 0; round < burst_per_receiver; ++round) {
        for (const auto& addr : recv_addrs) {
          const int send_ret = sendto(
              send_socket, &msg, 1, 0,
              reinterpret_cast<const sockaddr*>(&addr),
              static_cast<int>(sizeof(addr)));
          if (send_ret != 1) {
            std::cout << "[Burst] sendto failed, err=" << WSAGetLastError()
                      << std::endl;
            delegate.RequestQuit();
            pump.ScheduleWork();
            return;
          }
        }
      }

      std::this_thread::sleep_for(std::chrono::seconds(2));
      if (stats.total_completed.load(std::memory_order_acquire) <
          static_cast<int>(recv_addrs.size()) * burst_per_receiver) {
        std::cout << "[Burst] Timeout waiting completions, force quit." << std::endl;
        delegate.RequestQuit();
        pump.ScheduleWork();
      }
    });

    pump.Run(&delegate);
    producer.join();

    for (auto& c : controllers) {
      c.StopWatching();
    }
    closesocket(send_socket);
    for (SOCKET s : recv_sockets) {
      closesocket(s);
    }

    const int completed = stats.total_completed.load(std::memory_order_acquire);
    const int total_bytes = stats.total_bytes.load(std::memory_order_acquire);
    const int errors = stats.completion_errors.load(std::memory_order_acquire);
    double elapsed_sec = 0.0;
    if (stats.has_started.load(std::memory_order_acquire)) {
      const auto end_time = stats.end_time.time_since_epoch().count() == 0
                                ? std::chrono::steady_clock::now()
                                : stats.end_time;
      elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(
                        end_time - stats.start_time)
                        .count();
    }

    std::cout << "[Burst] completed=" << completed
              << "/" << total_expected
              << ", bytes=" << total_bytes
              << ", errors=" << errors << std::endl;
    if (elapsed_sec > 0.0) {
      const double pkt_per_sec = static_cast<double>(completed) / elapsed_sec;
      const double mb_per_sec =
          (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / elapsed_sec;
      std::cout << "[Burst] elapsed_sec=" << elapsed_sec
                << ", throughput_pkt_s=" << pkt_per_sec
                << ", throughput_MB_s=" << mb_per_sec << std::endl;
    }

    WSACleanup();
  }
#else
  std::cout << "MessagePumpForIO demo (POSIX path):" << std::endl;
  std::cout << "- Demonstrates cross-thread ScheduleWork wakeup." << std::endl;
  std::cout << "- Demonstrates fd readability watch via pipe." << std::endl;

  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    std::cerr << "pipe() failed, errno=" << errno << std::endl;
    return 1;
  }

  const int read_fd = pipe_fds[0];
  const int write_fd = pipe_fds[1];

  nei::MessagePumpForIO::FdWatchController controller;
  PipeReadWatcher watcher(&pump);
  if (!controller.StartWatching(&pump, read_fd,
                                nei::MessagePumpForIO::FdWatchController::Mode::READ,
                                &watcher)) {
    std::cerr << "StartWatching failed" << std::endl;
    close(read_fd);
    close(write_fd);
    return 2;
  }

  std::thread producer([&pump, write_fd]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "[Producer] ScheduleWork" << std::endl;
    pump.ScheduleWork();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::uint8_t byte = 42;
    const ssize_t n = write(write_fd, &byte, sizeof(byte));
    if (n == static_cast<ssize_t>(sizeof(byte))) {
      std::cout << "[Producer] Wrote one byte to pipe." << std::endl;
    } else {
      std::cout << "[Producer] write failed, errno=" << errno << std::endl;
    }
  });

  pump.Run(&delegate);

  producer.join();
  controller.StopWatching();
  close(read_fd);
  close(write_fd);
#endif

  std::cout << "MessagePumpForIO demo finished." << std::endl;
  return 0;
}
