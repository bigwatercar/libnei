// winsock2.h must precede any header that may pull in windows.h
#if defined(_WIN32)
#include <winsock2.h>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>

#include <iostream>

#include <neixx/common/time.h>
#include <neixx/functional/bind.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/udp_socket.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/run_loop.h>
#include <neixx/task/sequence_manager.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace nei::net {
namespace {

// =============================================================================
// UdpSocketTest — IO thread + main-thread RunLoop support
// =============================================================================
class UdpSocketTest : public testing::Test {
protected:
  void SetUp() override {
    // Main-thread SequenceManager so RunLoop::Run() works on the test thread.
    main_mgr_ = std::make_unique<SequenceManager>();

    Thread::Options io_opts;
    io_opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(io_thread_.StartWithOptions(io_opts));
    io_runner_ = io_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);
  }

  void TearDown() override {
    io_thread_.Stop();
    main_mgr_->Shutdown();
  }

  // Bind a socket to 127.0.0.1:0; return assigned endpoint.  IO-thread only.
  static IPEndPoint BindToLoopback(UDPSocket *sock, scoped_refptr<TaskRunner> runner) {
    IPEndPoint local(IPAddress::FromIPv4(127, 0, 0, 1), 0);
    EXPECT_TRUE(sock->Bind(local, std::move(runner)));
    IPEndPoint bound;
    EXPECT_TRUE(sock->GetLocalAddress(&bound));
    return bound;
  }

  std::unique_ptr<SequenceManager> main_mgr_;
  Thread io_thread_{"udp-io"};
  scoped_refptr<TaskRunner> io_runner_;
};

// =============================================================================
// Target 1: Zero-Byte Datagram
// =============================================================================
TEST_F(UdpSocketTest, ZeroByteDatagram) {
  std::atomic<bool> send_ok{false};
  std::atomic<int> send_bytes{-1};
  std::atomic<bool> recv_ok{false};
  std::atomic<int> recv_bytes{-1};
  std::atomic<int> completions{0};

  RunLoop loop;
  auto quit_ptr = std::make_shared<OnceClosure>(loop.QuitClosure());

  io_runner_->PostTask(FROM_HERE, [&, quit_ptr]() {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    // sock is captured by value in both callbacks so it outlives this lambda.
    auto sock = std::make_shared<UDPSocket>();
    IPEndPoint bound = BindToLoopback(sock.get(), io_runner_);

    // RecvFrom first so it pends.
    auto recv_buf = MakeRefCounted<IOBufferWithSize>(2048);
    sock->RecvFrom(recv_buf, 2048, [&, quit_ptr, sock](bool ok, int n, const IPEndPoint & /*peer*/) {
      EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
      recv_ok.store(ok);
      recv_bytes.store(n);
      if (completions.fetch_add(1) + 1 == 2)
        std::move(*quit_ptr).Run();
    });

    // Send a zero-length datagram to ourselves.
    auto send_buf = MakeRefCounted<IOBufferWithSize>(0);
    sock->SendTo(send_buf, 0, bound, [&, quit_ptr, sock](bool ok, int n) {
      EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
      send_ok.store(ok);
      send_bytes.store(n);
      if (completions.fetch_add(1) + 1 == 2)
        std::move(*quit_ptr).Run();
    });
  });

  loop.Run();

  EXPECT_TRUE(send_ok.load());
  EXPECT_EQ(send_bytes.load(), 0);
  EXPECT_TRUE(recv_ok.load());
  EXPECT_EQ(recv_bytes.load(), 0);
}

// =============================================================================
// Target 2: Bind-Close Race Leak Defense
// =============================================================================
TEST_F(UdpSocketTest, BindCloseRace) {
  RunLoop loop;
  auto quit_ptr = std::make_shared<OnceClosure>(loop.QuitClosure());

  io_runner_->PostTask(FROM_HERE, [this, quit_ptr]() {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    constexpr int kIterations = 100;
    for (int i = 0; i < kIterations; ++i) {
      auto sock = std::make_unique<UDPSocket>();
      IPEndPoint local(IPAddress::FromIPv4(127, 0, 0, 1), 0);
      EXPECT_TRUE(sock->Bind(local, io_runner_));
      sock->Close();
    }

    // Verify we can still bind after many create/destroy cycles.
    auto final_sock = std::make_unique<UDPSocket>();
    IPEndPoint local(IPAddress::FromIPv4(127, 0, 0, 1), 0);
    EXPECT_TRUE(final_sock->Bind(local, io_runner_));
    IPEndPoint bound;
    EXPECT_TRUE(final_sock->GetLocalAddress(&bound));
    EXPECT_GT(bound.port(), 0u);
    final_sock->Close();

    std::move(*quit_ptr).Run();
  });

  loop.Run();
}

// =============================================================================
// Target 3: Orphaned UAF Defense
// =============================================================================
TEST_F(UdpSocketTest, OrphanedWhileRecvPending) {
  RunLoop loop;
  auto quit_ptr = std::make_shared<OnceClosure>(loop.QuitClosure());

  io_runner_->PostTask(FROM_HERE, [this, quit_ptr]() {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    auto sock = std::make_shared<UDPSocket>();
    BindToLoopback(sock.get(), io_runner_); // bind side-effect only

    // Issue RecvFrom — it will pend waiting for data.
    auto recv_buf = MakeRefCounted<IOBufferWithSize>(2048);
    sock->RecvFrom(recv_buf, 2048, [](bool /*ok*/, int /*n*/, const IPEndPoint & /*peer*/) {
      // May fire on cancel; the point is we don't crash.
    });

    // Destroy the socket in a subsequent task while I/O is in-flight.
    io_runner_->PostTask(FROM_HERE, [sock, quit_ptr]() {
      sock->Close();
      // sock goes out of scope here — triggers Orphan path.
      std::move(*quit_ptr).Run();
    });
  });

  loop.Run();
  SUCCEED();
}

// =============================================================================
// Target 4: Lock-Free Dispatch Deadlock Immunity
// =============================================================================
TEST_F(UdpSocketTest, ReentrantCloseInCallback) {
  RunLoop loop;
  auto quit_ptr = std::make_shared<OnceClosure>(loop.QuitClosure());

  io_runner_->PostTask(FROM_HERE, [this, quit_ptr]() {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    auto sock = std::make_shared<UDPSocket>();
    BindToLoopback(sock.get(), io_runner_); // bind side-effect only

    // Issue RecvFrom with a callback that re-enters Close().
    auto recv_buf = MakeRefCounted<IOBufferWithSize>(2048);
    sock->RecvFrom(recv_buf, 2048, [sock, this, quit_ptr](bool /*ok*/, int /*n*/, const IPEndPoint & /*peer*/) {
      EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
      // Re-enter Close from inside the callback — must not deadlock.
      sock->Close();
      std::move(*quit_ptr).Run();
    });

    // Close from a separate task to trigger the failure callback path.
    io_runner_->PostTask(FROM_HERE, [sock]() { sock->Close(); });
  });

  loop.Run();
  SUCCEED();
}

// =============================================================================
// Target 5: High-Concurrency Drain — UDP congestion resilience
// =============================================================================
//
// This test verifies that high-volume UDP send/recv on loopback does not
// cause crashes, UAF, deadlocks, or resource leaks.
//
// UDP is inherently unreliable — datagrams may be silently dropped by the
// kernel under congestion (ENOBUFS / WSAENOBUFS).  The test uses a safety
// timeout to guarantee termination and does NOT require 100% delivery.
// Send failures are surfaced to the callback (not internally queued) so
// that the caller can implement application-level pacing or retry.
//
TEST_F(UdpSocketTest, HighConcurrencyDrain) {
  const int kDatagramCount = 500;
  const int kPayloadSize = 64;
  const int kTimeoutMs = 5000; // Safety timeout — prevents hang on loss

  RunLoop loop;
  auto quit = loop.QuitClosure();
  auto quit_ptr = std::make_shared<OnceClosure>(std::move(quit));

  struct DrainState {
    std::atomic<int> sent{0};
    std::atomic<int> send_failures{0};
    std::atomic<int> received{0};
    std::atomic<bool> done{false};
    std::function<void()> start_recv;
  };

  auto state = std::make_shared<DrainState>();

  io_runner_->PostTask(FROM_HERE, [this, quit_ptr, state, kDatagramCount, kPayloadSize, kTimeoutMs]() mutable {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    auto sock = std::make_shared<UDPSocket>();
    IPEndPoint bound = BindToLoopback(sock.get(), io_runner_);

    // Safety timeout: quit the loop after kTimeoutMs regardless of
    // delivery progress.  Prevents the test from hanging forever when
    // the kernel silently drops UDP datagrams under congestion.
    io_runner_->PostDelayedTask(FROM_HERE,
                                BindOnce([quit_ptr, state]() {
                                  bool expected = false;
                                  if (state->done.compare_exchange_strong(expected, true)) {
                                    std::move(*quit_ptr).Run();
                                  }
                                }),
                                TimeDelta::FromMilliseconds(kTimeoutMs));

    auto recv_buf = MakeRefCounted<IOBufferWithSize>(2048);

    state->start_recv = [&, quit_ptr, state, sock, recv_buf, kDatagramCount, kPayloadSize]() {
      if (state->done.load(std::memory_order_acquire))
        return;
      sock->RecvFrom(
          recv_buf,
          2048,
          [this, quit_ptr, state, sock, kDatagramCount, kPayloadSize](bool ok, int n, const IPEndPoint & /*peer*/) {
            EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
            if (state->done.load(std::memory_order_acquire))
              return;
            if (ok && n == kPayloadSize) {
              int prev = state->received.fetch_add(1, std::memory_order_acq_rel);
              if (prev + 1 >= kDatagramCount) {
                bool expected = false;
                if (state->done.compare_exchange_strong(expected, true)) {
                  std::move(*quit_ptr).Run();
                }
                return;
              }
            }
            state->start_recv();
          });
    };

    state->start_recv();

    // Send datagrams.  Individual sends may fail with ENOBUFS /
    // WSAENOBUFS under congestion — this is valid UDP behavior.
    // The lower layer surfaces the failure via the callback rather
    // than silently queuing internally.
    for (int i = 0; i < kDatagramCount; ++i) {
      auto send_buf = MakeRefCounted<IOBufferWithSize>(kPayloadSize);
      std::memset(send_buf->data(), static_cast<unsigned char>(i & 0xFF), kPayloadSize);
      sock->SendTo(send_buf, kPayloadSize, bound, [state](bool ok, int /*n*/) {
        state->sent.fetch_add(1, std::memory_order_relaxed);
        if (!ok)
          state->send_failures.fetch_add(1, std::memory_order_relaxed);
        // NOTE: ok may be false under congestion.
        // This is valid UDP behavior — do not EXPECT_TRUE here.
      });
    }
  });

  loop.Run();

  int recv = state->received.load();
  int sent = state->sent.load();
  int failures = state->send_failures.load();

  // Sanity: at least some datagrams must have been delivered.
  EXPECT_GT(recv, 0) << "No datagrams received — basic I/O path is broken";

  // Log delivery statistics for manual inspection.
  std::cout << "[HighConcurrencyDrain] sent=" << sent << " failures=" << failures << " received=" << recv
            << " delivery_ratio=" << (sent > 0 ? (100.0 * recv / sent) : 0.0) << "%" << std::endl;

  SUCCEED();
}

// =============================================================================
// Target 6: IPv6 Loopback Basic Send/Recv
// =============================================================================
TEST_F(UdpSocketTest, IPv6Loopback) {
  std::atomic<bool> send_ok{false};
  std::atomic<bool> recv_ok{false};
  std::atomic<int> recv_bytes{-1};
  std::atomic<int> completions{0};

  RunLoop loop;
  auto quit_ptr = std::make_shared<OnceClosure>(loop.QuitClosure());

  io_runner_->PostTask(FROM_HERE, [&, quit_ptr]() {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    auto sock = std::make_shared<UDPSocket>();

    // Bind to IPv6 loopback ::1.
    uint8_t ipv6_loopback[16] = {};
    ipv6_loopback[15] = 1;
    IPAddress addr6 = IPAddress::FromIPv6(ipv6_loopback);
    IPEndPoint local(addr6, 0);
    EXPECT_TRUE(sock->Bind(local, io_runner_));

    IPEndPoint bound;
    EXPECT_TRUE(sock->GetLocalAddress(&bound));
    EXPECT_TRUE(bound.address().IsIPv6());
    EXPECT_GT(bound.port(), 0u);

    // RecvFrom first.
    auto recv_buf = MakeRefCounted<IOBufferWithSize>(2048);
    sock->RecvFrom(recv_buf, 2048, [&, quit_ptr, sock](bool ok, int n, const IPEndPoint &peer) {
      EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
      recv_ok.store(ok);
      recv_bytes.store(n);
      if (ok) {
        EXPECT_TRUE(peer.address().IsIPv6());
      }
      if (completions.fetch_add(1) + 1 == 2)
        std::move(*quit_ptr).Run();
    });

    // Send a datagram to ourselves via IPv6 loopback.
    auto send_buf = MakeRefCounted<IOBufferWithSize>(32);
    std::memset(send_buf->data(), 0xAA, 32);
    sock->SendTo(send_buf, 32, bound, [&, quit_ptr, sock](bool ok, int /*n*/) {
      EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
      send_ok.store(ok);
      if (completions.fetch_add(1) + 1 == 2)
        std::move(*quit_ptr).Run();
    });
  });

  loop.Run();

  EXPECT_TRUE(send_ok.load());
  EXPECT_TRUE(recv_ok.load());
  EXPECT_EQ(recv_bytes.load(), 32);
}

// =============================================================================
// Target 7: SetBroadcast API & Basic Operation
// =============================================================================
TEST_F(UdpSocketTest, SetBroadcast) {
  std::atomic<bool> send_ok{false};
  std::atomic<bool> recv_ok{false};
  std::atomic<int> completions{0};

  RunLoop loop;
  auto quit_ptr = std::make_shared<OnceClosure>(loop.QuitClosure());

  io_runner_->PostTask(FROM_HERE, [&, quit_ptr]() {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    auto sock = std::make_shared<UDPSocket>();
    IPEndPoint local(IPAddress::FromIPv4(0, 0, 0, 0), 0);
    EXPECT_TRUE(sock->Bind(local, io_runner_));

    // Enable broadcast — must succeed on a bound socket.
    EXPECT_TRUE(sock->SetBroadcast(true));

    // Disable broadcast.
    EXPECT_TRUE(sock->SetBroadcast(false));

    // Re-enable and verify normal send/recv still works.
    EXPECT_TRUE(sock->SetBroadcast(true));

    IPEndPoint bound;
    EXPECT_TRUE(sock->GetLocalAddress(&bound));

    auto recv_buf = MakeRefCounted<IOBufferWithSize>(64);
    sock->RecvFrom(recv_buf, 64, [&, quit_ptr, sock](bool ok, int /*n*/, const IPEndPoint &) {
      EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
      recv_ok.store(ok);
      if (completions.fetch_add(1) + 1 == 2)
        std::move(*quit_ptr).Run();
    });

    auto send_buf = MakeRefCounted<IOBufferWithSize>(16);
    std::memset(send_buf->data(), 0xBB, 16);
    sock->SendTo(send_buf,
                 16,
                 IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), bound.port()),
                 [&, quit_ptr, sock](bool ok, int /*n*/) {
                   EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
                   send_ok.store(ok);
                   if (completions.fetch_add(1) + 1 == 2)
                     std::move(*quit_ptr).Run();
                 });
  });

  loop.Run();

  EXPECT_TRUE(send_ok.load());
  EXPECT_TRUE(recv_ok.load());
}

// =============================================================================
// Target 8: Multicast Join / Leave (IPv4 + IPv6)
// =============================================================================
TEST_F(UdpSocketTest, MulticastJoinLeave) {
  RunLoop loop;
  auto quit_ptr = std::make_shared<OnceClosure>(loop.QuitClosure());

  io_runner_->PostTask(FROM_HERE, [this, quit_ptr]() {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    // ---- IPv4 multicast ----
    {
      auto sock = std::make_unique<UDPSocket>();
      IPEndPoint local(IPAddress::FromIPv4(0, 0, 0, 0), 0);
      EXPECT_TRUE(sock->Bind(local, io_runner_));

      // Join an IPv4 multicast group (mDNS).
      IPAddress mcast_v4 = IPAddress::FromIPv4(224, 0, 0, 251);
      EXPECT_TRUE(sock->JoinGroup(mcast_v4));

      // Leave the group.
      EXPECT_TRUE(sock->LeaveGroup(mcast_v4));

      sock->Close();
    }

    // ---- IPv6 multicast ----
    {
      auto sock = std::make_unique<UDPSocket>();
      uint8_t ipv6_any[16] = {};
      IPAddress addr6_any = IPAddress::FromIPv6(ipv6_any);
      IPEndPoint local(addr6_any, 0);
      // IPv6 multicast requires an IPv6-capable socket; binding to [::]:0
      // may fail on hosts without IPv6.  Treat failure as non-fatal.
      if (sock->Bind(local, io_runner_)) {
        // Join an IPv6 multicast group (mDNS6: ff02::fb).
        uint8_t ff02_fb[16] = {0xff, 0x02};
        ff02_fb[15] = 0xfb;
        IPAddress mcast_v6 = IPAddress::FromIPv6(ff02_fb);

        bool joined = sock->JoinGroup(mcast_v6);
        // JoinGroup may fail if the host lacks a usable IPv6 interface;
        // this is expected in some CI/container environments.
        if (joined) {
          EXPECT_TRUE(sock->LeaveGroup(mcast_v6));
        }
      }

      sock->Close();
    }

    std::move(*quit_ptr).Run();
  });

  loop.Run();
  SUCCEED();
}

// =============================================================================
// Target 9: Multiple Pending RecvFrom — Concurrent Dispatch
// =============================================================================
//
// Verifies that multiple concurrent RecvFrom calls are all serviced
// correctly.  UDP does not guarantee datagram ordering, and IOCP
// completion order is not strict FIFO — this test uses a bitmask to
// track which tagged packets arrived, without assuming order.
//
TEST_F(UdpSocketTest, MultiplePendingRecvFrom) {
  const int kNumPackets = 5;
  std::atomic<int> recv_count{0};
  std::atomic<int> recv_mask{0};
  std::atomic<int> send_count{0};

  RunLoop loop;
  auto quit_ptr = std::make_shared<OnceClosure>(loop.QuitClosure());

  io_runner_->PostTask(FROM_HERE, [&, quit_ptr]() {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    auto sock = std::make_shared<UDPSocket>();
    IPEndPoint bound = BindToLoopback(sock.get(), io_runner_);

    // Issue kNumPackets concurrent RecvFrom calls.
    for (int i = 0; i < kNumPackets; ++i) {
      auto buf = MakeRefCounted<IOBufferWithSize>(128);
      sock->RecvFrom(buf, 128, [&, quit_ptr, sock, buf](bool ok, int n, const IPEndPoint & /*peer*/) {
        EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
        if (ok) {
          EXPECT_EQ(n, 32);
          // Record which tagged packet arrived (order-independent).
          uint8_t tag = static_cast<uint8_t>(buf->data()[0]);
          EXPECT_LT(tag, kNumPackets);
          recv_mask.fetch_or(1 << tag, std::memory_order_relaxed);
        }
        if (recv_count.fetch_add(1) + 1 == kNumPackets)
          std::move(*quit_ptr).Run();
      });
    }

    // Send kNumPackets with index-tagged payloads.
    for (int i = 0; i < kNumPackets; ++i) {
      auto send_buf = MakeRefCounted<IOBufferWithSize>(32);
      std::memset(send_buf->data(), static_cast<unsigned char>(i), 32);
      sock->SendTo(
          send_buf, 32, bound, [&](bool /*ok*/, int /*n*/) { send_count.fetch_add(1, std::memory_order_relaxed); });
    }
  });

  loop.Run();

  // All kNumPackets callback slots should have fired.
  EXPECT_EQ(recv_count.load(), kNumPackets);
  // All kNumPackets distinct tags should have been observed,
  // regardless of arrival order.
  EXPECT_EQ(recv_mask.load(), (1 << kNumPackets) - 1);
  EXPECT_GT(send_count.load(), 0);
}

// =============================================================================
// Target 10: Large Datagram — Near-MTU Payload
// =============================================================================
TEST_F(UdpSocketTest, LargeDatagram) {
  // 1400 bytes fits safely within a typical Ethernet MTU (1500 minus
  // IP/UDP headers) and avoids IP fragmentation on loopback.
  const int kPayloadSize = 1400;

  std::atomic<bool> send_ok{false};
  std::atomic<bool> recv_ok{false};
  std::atomic<int> recv_bytes{0};
  std::atomic<int> completions{0};

  RunLoop loop;
  auto quit_ptr = std::make_shared<OnceClosure>(loop.QuitClosure());

  io_runner_->PostTask(FROM_HERE, [&, quit_ptr]() {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    auto sock = std::make_shared<UDPSocket>();
    IPEndPoint bound = BindToLoopback(sock.get(), io_runner_);

    auto recv_buf = MakeRefCounted<IOBufferWithSize>(2048);
    sock->RecvFrom(recv_buf, 2048, [&, quit_ptr, sock](bool ok, int n, const IPEndPoint &) {
      EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
      recv_ok.store(ok);
      recv_bytes.store(n);
      if (completions.fetch_add(1) + 1 == 2)
        std::move(*quit_ptr).Run();
    });

    auto send_buf = MakeRefCounted<IOBufferWithSize>(kPayloadSize);
    std::memset(send_buf->data(), 0x7E, kPayloadSize);
    sock->SendTo(send_buf, kPayloadSize, bound, [&, quit_ptr, sock](bool ok, int n) {
      EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
      send_ok.store(ok);
      EXPECT_EQ(n, kPayloadSize);
      if (completions.fetch_add(1) + 1 == 2)
        std::move(*quit_ptr).Run();
    });
  });

  loop.Run();

  EXPECT_TRUE(send_ok.load());
  EXPECT_TRUE(recv_ok.load());
  EXPECT_EQ(recv_bytes.load(), kPayloadSize);
}

// =============================================================================
// Target 11: SetSendBufferSize / SetReceiveBufferSize Options
// =============================================================================
TEST_F(UdpSocketTest, SetBufferSizes) {
  RunLoop loop;
  auto quit_ptr = std::make_shared<OnceClosure>(loop.QuitClosure());

  io_runner_->PostTask(FROM_HERE, [this, quit_ptr]() {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    auto sock = std::make_unique<UDPSocket>();
    IPEndPoint local(IPAddress::FromIPv4(0, 0, 0, 0), 0);
    EXPECT_TRUE(sock->Bind(local, io_runner_));

    // Set SO_SNDBUF / SO_RCVBUF to reasonable sizes.
    // The OS may silently cap the value; we only verify the call succeeds.
    EXPECT_TRUE(sock->SetSendBufferSize(256 * 1024));
    EXPECT_TRUE(sock->SetReceiveBufferSize(256 * 1024));

    // Also test smaller buffer sizes.
    EXPECT_TRUE(sock->SetSendBufferSize(8192));
    EXPECT_TRUE(sock->SetReceiveBufferSize(8192));

    sock->Close();
    std::move(*quit_ptr).Run();
  });

  loop.Run();
  SUCCEED();
}

} // namespace
} // namespace nei::net
