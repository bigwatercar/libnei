// winsock2.h must precede any header that may pull in windows.h
#if defined(_WIN32)
#include <winsock2.h>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>

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
  static IPEndPoint BindToLoopback(UDPSocket* sock,
                                   scoped_refptr<TaskRunner> runner) {
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
  std::atomic<int>  send_bytes{-1};
  std::atomic<bool> recv_ok{false};
  std::atomic<int>  recv_bytes{-1};
  std::atomic<int>  completions{0};

  RunLoop loop;
  auto quit_ptr = std::make_shared<OnceClosure>(loop.QuitClosure());

  io_runner_->PostTask(FROM_HERE, [&, quit_ptr]() {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    // sock is captured by value in both callbacks so it outlives this lambda.
    auto sock = std::make_shared<UDPSocket>();
    IPEndPoint bound = BindToLoopback(sock.get(), io_runner_);

    // RecvFrom first so it pends.
    auto recv_buf = MakeRefCounted<IOBufferWithSize>(2048);
    sock->RecvFrom(recv_buf, 2048,
                   [&, quit_ptr, sock](bool ok, int n, const IPEndPoint& /*peer*/) {
                     EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
                     recv_ok.store(ok);
                     recv_bytes.store(n);
                     if (completions.fetch_add(1) + 1 == 2)
                       std::move(*quit_ptr).Run();
                   });

    // Send a zero-length datagram to ourselves.
    auto send_buf = MakeRefCounted<IOBufferWithSize>(0);
    sock->SendTo(send_buf, 0, bound,
                 [&, quit_ptr, sock](bool ok, int n) {
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
    IPEndPoint bound = BindToLoopback(sock.get(), io_runner_);

    // Issue RecvFrom — it will pend waiting for data.
    auto recv_buf = MakeRefCounted<IOBufferWithSize>(2048);
    sock->RecvFrom(recv_buf, 2048,
                   [](bool /*ok*/, int /*n*/, const IPEndPoint& /*peer*/) {
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
    IPEndPoint bound = BindToLoopback(sock.get(), io_runner_);

    // Issue RecvFrom with a callback that re-enters Close().
    auto recv_buf = MakeRefCounted<IOBufferWithSize>(2048);
    sock->RecvFrom(
        recv_buf, 2048,
        [sock, this, quit_ptr](bool /*ok*/, int /*n*/,
                                const IPEndPoint& /*peer*/) {
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
// Target 5: High-Concurrency EAGAIN Starvation Recovery
// =============================================================================
TEST_F(UdpSocketTest, HighConcurrencyDrain) {
  const int kDatagramCount = 200;
  const int kPayloadSize   = 64;

  RunLoop loop;
  auto quit = loop.QuitClosure();
  auto quit_ptr = std::make_shared<OnceClosure>(std::move(quit));

  // Shared state that outlives recursive lambda invocations.
  struct DrainState {
    std::atomic<int> received{0};
    std::function<void()> start_recv;
  };
  auto state = std::make_shared<DrainState>();

  io_runner_->PostTask(FROM_HERE,
                       [this, quit_ptr, state, kDatagramCount, kPayloadSize]() mutable {
    EXPECT_TRUE(io_runner_->BelongsToCurrentThread());

    // sock captured by value in callbacks so it outlives this lambda.
    auto sock = std::make_shared<UDPSocket>();
    IPEndPoint bound = BindToLoopback(sock.get(), io_runner_);

    auto recv_buf = MakeRefCounted<IOBufferWithSize>(2048);

    state->start_recv = [&, quit_ptr, state, sock, recv_buf,
                         kDatagramCount, kPayloadSize]() {
      sock->RecvFrom(
          recv_buf, 2048,
          [this, quit_ptr, state, sock, kDatagramCount, kPayloadSize](
              bool ok, int n, const IPEndPoint& /*peer*/) {
            EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
            if (ok && n == kPayloadSize) {
              int prev = state->received.fetch_add(1, std::memory_order_acq_rel);
              if (prev + 1 < kDatagramCount) {
                state->start_recv();
              } else {
                std::move(*quit_ptr).Run();
              }
            } else {
              if (state->received.load() < kDatagramCount)
                state->start_recv();
            }
          });
    };

    state->start_recv();

    // Flood sends.
    for (int i = 0; i < kDatagramCount; ++i) {
      auto send_buf = MakeRefCounted<IOBufferWithSize>(kPayloadSize);
      std::memset(send_buf->data(), static_cast<char>(i), kPayloadSize);
      sock->SendTo(send_buf, kPayloadSize, bound,
                   [this, sock, kPayloadSize](bool ok, int n) {
                     EXPECT_TRUE(io_runner_->BelongsToCurrentThread());
                     EXPECT_TRUE(ok);
                     EXPECT_EQ(n, kPayloadSize);
                   });
    }
  });

  loop.Run();
  SUCCEED();
}

}  // namespace
}  // namespace nei::net
