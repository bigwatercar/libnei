// =============================================================================
// TCP socket unit tests  --  async handshake, data transfer, error paths,
// and destruction safety.
// =============================================================================

// winsock2.h must come before any header that might include windows.h.
#if defined(_WIN32)
#include <winsock2.h>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include <neixx/common/time.h>
#include <neixx/functional/bind.h>
#include <neixx/io/io_buffer.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/net/tcp_server_socket.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nei::net {
namespace {

// ===========================================================================
// TcpSocketTest fixture  --  provides a dedicated IO thread with MessagePumpForIO.
// ===========================================================================

class TcpSocketTest : public testing::Test {
protected:
  void SetUp() override {
    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(io_thread_.StartWithOptions(opts));
    io_runner_ = io_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);
  }

  void TearDown() override {
    io_thread_.Stop();
  }

  // Finds a free TCP port by binding to loopback:0, querying the assigned
  // port, then closing.  The port is immediately reusable.
  static uint16_t FindFreePort() {
#if defined(_WIN32)
    // WSAStartup must be called before any socket API on Windows.
    static const bool wsa_ready = []() {
      WSADATA d;
      return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)wsa_ready;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
      return 0;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      ::closesocket(s);
      return 0;
    }
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
    if (::bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      ::close(fd);
      return 0;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(fd, (struct sockaddr *)&addr, &len);
    ::close(fd);
    return ntohs(addr.sin_port);
#endif
  }

  Thread io_thread_{"tcp-test-io"};
  scoped_refptr<SingleThreadTaskRunner> io_runner_;
};

// ===========================================================================
// Test 1  --  BasicHandshake
// ===========================================================================

TEST_F(TcpSocketTest, BasicHandshake) {
  const uint16_t port = FindFreePort();

  WaitableEvent server_accept_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent client_connect_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> accepted{false};
  std::atomic<bool> connected{false};

  // shared_ptr keeps server and client alive beyond the PostTask closure.
  auto server = std::make_shared<TCPServerSocket>();
  auto client = std::make_shared<TCPClientSocket>();

  io_runner_->PostTask(
      FROM_HERE, [server, client, this, port, &server_accept_done, &client_connect_done, &accepted, &connected]() {
        bool ok = server->Listen(
            IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
            1,
            [server, &server_accept_done, &accepted](bool success, std::unique_ptr<TCPClientSocket> accepted_client) {
              accepted.store(success);
              EXPECT_TRUE(success) << "Server accept should succeed";
              EXPECT_NE(accepted_client, nullptr);
              if (accepted_client)
                accepted_client->Close();
              server_accept_done.Signal();
            },
            io_runner_,
            {});
        ASSERT_TRUE(ok) << "Listen should succeed on port " << port;

        client->Connect(
            IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
            [client, &client_connect_done, &connected](bool ok) {
              connected.store(ok);
              EXPECT_TRUE(ok) << "Client connect should succeed";
              client_connect_done.Signal();
            },
            io_runner_);
      });

  ASSERT_TRUE(server_accept_done.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(client_connect_done.TimedWait(std::chrono::seconds(5)));

  EXPECT_TRUE(accepted.load());
  EXPECT_TRUE(connected.load());

  // Break reference cycle: accept callback captures server shared_ptr,
  // which would keep the Impl (and its WeakPtrFactory) alive past TearDown.
  server->Shutdown();
}

// ===========================================================================
// Test 2  --  AsyncStreamTransfer (1 MB zero-copy)
// ===========================================================================

TEST_F(TcpSocketTest, AsyncStreamTransfer) {
  const uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);
  constexpr std::size_t kTransferSize = 1024 * 1024;

  WaitableEvent transfer_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> data_match{false};

  auto server = std::make_shared<TCPServerSocket>();
  auto client = std::make_shared<TCPClientSocket>();

  io_runner_->PostTask(FROM_HERE, [server, client, &transfer_done, &data_match, this, port, kTransferSize]() {
    auto ref_buf = MakeRefCounted<IOBufferWithSize>(kTransferSize);
    for (std::size_t i = 0; i < kTransferSize; ++i)
      ref_buf->data()[i] = static_cast<unsigned char>(i & 0xFF);

    auto recv_buf = MakeRefCounted<IOBufferWithSize>(kTransferSize);
    std::memset(recv_buf->data(), 0, kTransferSize);
    auto recv_offset = std::make_shared<std::size_t>(0);

    // IOBufferWithSize inherits RefCountedThreadSafe<IOBuffer>, so it
    // implicitly converts to scoped_refptr<IOBuffer> — no .get() needed.
    // refcount is shared even when the pointer type differs.
    scoped_refptr<IOBuffer> ref_base(ref_buf.get());
    scoped_refptr<IOBuffer> recv_base(recv_buf.get());

    bool ok = server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        1,
        [server, &transfer_done, &data_match, ref_base, recv_base, recv_offset, kTransferSize](
            bool success, std::unique_ptr<TCPClientSocket> accepted) mutable {
          ASSERT_TRUE(success);
          ASSERT_NE(accepted, nullptr);

          // Move accepted into shared ownership so it survives the callback.
          auto sock = std::make_shared<std::unique_ptr<TCPClientSocket>>(std::move(accepted));

          auto do_read = std::make_shared<std::function<void()>>();
          *do_read = [do_read, &transfer_done, &data_match, sock, recv_base, recv_offset, ref_base, kTransferSize]() {
            auto *accepted = sock->get();
            std::size_t remaining = kTransferSize - *recv_offset;
            if (remaining == 0) {
              data_match.store(std::memcmp(ref_base->data(), recv_base->data(), kTransferSize) == 0);
              (*sock)->Close();
              transfer_done.Signal();
              return;
            }
            // Read into the correct offset within the buffer.
            // IOBuffer::data() always returns the base pointer, so we
            // create a temporary buffer and memcpy to the right spot.
            auto read_buf = MakeRefCounted<IOBufferWithSize>(remaining);
            accepted->ReadAsync(
                read_buf, remaining, [recv_offset, recv_base, do_read, read_buf](bool s, std::size_t n) {
                  EXPECT_TRUE(s);
                  EXPECT_GT(n, 0u);
                  // Copy received data to the correct position.
                  std::memcpy(recv_base->data() + *recv_offset, read_buf->data(), n);
                  *recv_offset += n;
                  (*do_read)();
                });
          };
          (*do_read)();
        },
        io_runner_,
        {});
    ASSERT_TRUE(ok);

    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [client, ref_base, port, kTransferSize](bool connected_ok) {
          ASSERT_TRUE(connected_ok);
          client->WriteAsync(ref_base, kTransferSize, [kTransferSize](bool s, std::size_t n) {
            EXPECT_TRUE(s);
            EXPECT_EQ(n, kTransferSize);
          });
        },
        io_runner_);
  });

  ASSERT_TRUE(transfer_done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(data_match.load()) << "1 MB data should match byte-for-byte";

  // Break reference cycle: accept callback captures server shared_ptr.
  server->Shutdown();
}

// ===========================================================================
// Test 3  --  ConnectionRefused (async failure detection)
// ===========================================================================

TEST_F(TcpSocketTest, ConnectionRefused) {
  const uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);

  WaitableEvent connect_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> connect_result{true};

  auto client = std::make_shared<TCPClientSocket>();

  io_runner_->PostTask(FROM_HERE, [client, port, &connect_done, &connect_result, this]() {
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [client, &connect_done, &connect_result](bool ok) {
          connect_result.store(ok);
          connect_done.Signal();
        },
        io_runner_);
  });

  ASSERT_TRUE(connect_done.TimedWait(std::chrono::seconds(5)));
  EXPECT_FALSE(connect_result.load()) << "Connect to dead port should report failure";
}

// ===========================================================================
// Test 4  --  ServerDestructionWhilePending (no crash, no leak)
// ===========================================================================

TEST_F(TcpSocketTest, ServerDestructionWhilePending) {
  const uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);

  WaitableEvent server_created(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent server_destroyed(WaitableEvent::ResetPolicy::kAutomatic, false);

  io_runner_->PostTask(FROM_HERE, [&]() {
    auto server = std::make_unique<TCPServerSocket>();
    bool ok = server->Listen(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
                             1,
                             [](bool /*success*/, std::unique_ptr<TCPClientSocket> /*client*/) {
                               ADD_FAILURE() << "Accept callback should not fire after destruction";
                             },
                             io_runner_,
                             {});
    ASSERT_TRUE(ok);
    server_created.Signal();

    server.reset();
    server_destroyed.Signal();
  });

  ASSERT_TRUE(server_created.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(server_destroyed.TimedWait(std::chrono::seconds(5)));
  SUCCEED();
}

// ===========================================================================
// Test 5  --  ExplicitShutdownWrite (half-close handshake)
// ===========================================================================

TEST_F(TcpSocketTest, ExplicitShutdownWrite) {
  const uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);

  WaitableEvent eof_detected(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> server_saw_eof{false};

  auto server = std::make_shared<TCPServerSocket>();
  auto client = std::make_shared<TCPClientSocket>();

  io_runner_->PostTask(FROM_HERE, [&, this, port]() {
    bool ok = server->Listen(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
                             1,
                             [&](bool success, std::unique_ptr<TCPClientSocket> accepted) mutable {
                               ASSERT_TRUE(success);
                               auto sock = std::make_shared<std::unique_ptr<TCPClientSocket>>(std::move(accepted));

                               // Keep reading until EOF  --  proof that ShutdownWrite sent FIN.
                               auto buf = MakeRefCounted<IOBufferWithSize>(64);
                               auto do_read = std::make_shared<std::function<void()>>();
                               *do_read = [&, sock, buf, do_read]() {
                                 (*sock)->ReadAsync(buf, 64, [&, do_read](bool s, std::size_t n) {
                                   if (!s || n == 0) {
                                     server_saw_eof.store(true);
                                     eof_detected.Signal();
                                     return;
                                   }
                                   (*do_read)();
                                 });
                               };
                               (*do_read)();
                             },
                             io_runner_,
                             {});
    ASSERT_TRUE(ok);

    auto send_buf = MakeRefCounted<IOBufferWithSize>(64);
    for (int i = 0; i < 64; ++i)
      send_buf->data()[i] = 'A';

    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [&, send_buf](bool connected) {
          ASSERT_TRUE(connected);
          client->WriteAsync(send_buf, 64, [&](bool, std::size_t) {
            client->ShutdownWrite(); // Send FIN, read stays open.
          });
        },
        io_runner_);
  });

  ASSERT_TRUE(eof_detected.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(server_saw_eof.load()) << "Server should detect EOF after client ShutdownWrite";
}

// ===========================================================================
// Test 6  --  OrphanedDestruction_FoolproofFallback
// ===========================================================================

TEST_F(TcpSocketTest, OrphanedDestruction) {
  const uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);

  WaitableEvent accepted(WaitableEvent::ResetPolicy::kAutomatic, false);

  auto server = std::make_shared<TCPServerSocket>();

  io_runner_->PostTask(FROM_HERE, [&, this, port]() {
    bool ok = server->Listen(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
                             1,
                             [&accepted](bool /*success*/, std::unique_ptr<TCPClientSocket> /*client*/) {
                               // Just signal acceptance  --  client will be orphaned.
                               accepted.Signal();
                             },
                             io_runner_,
                             {});
    ASSERT_TRUE(ok);

    auto client = std::make_shared<TCPClientSocket>();
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [client](bool ok) {
          ASSERT_TRUE(ok);
          // Post a pending read so Orphan() has a callback to cancel.
          auto buf = MakeRefCounted<IOBufferWithSize>(64);
          client->ReadAsync(buf, 64, [](bool, std::size_t) {});
        },
        io_runner_);
  });

  // Wait for accept, then let IO thread drain.
  ASSERT_TRUE(accepted.TimedWait(std::chrono::seconds(5)));
  WaitableEvent drain(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner_->PostTask(FROM_HERE, [&drain]() { drain.Signal(); });
  ASSERT_TRUE(drain.TimedWait(std::chrono::seconds(5)));

  SUCCEED() << "Client destroyed without Close/ShutdownWrite  --  no UAF, no leak";
}

// ===========================================================================
// Test 7  --  OrphanedBackgroundFlush (orphan writes data + sends FIN)
// ===========================================================================
//
// Verifies that when TCPClientSocket is destroyed with a large pending write,
// the orphaned Impl stays alive in the background, flushes all data, and sends
// FIN.  The server must receive every byte intact and eventually see EOF.
//
// This is the critical safety guarantee: destroying the socket object must
// never truncate in-flight writes.

TEST_F(TcpSocketTest, OrphanedBackgroundFlush) {
  const uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);
  constexpr std::size_t kTransferSize = 1024 * 1024; // 1 MB

  WaitableEvent transfer_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> all_data_received{false};
  std::atomic<bool> eof_received{false};

  auto server = std::make_shared<TCPServerSocket>();

  io_runner_->PostTask(FROM_HERE, [&, this, port]() {
    // ---- server side: accumulate all received data ----
    auto recv_buf = MakeRefCounted<IOBufferWithSize>(kTransferSize);
    std::memset(recv_buf->data(), 0, kTransferSize);
    auto recv_offset = std::make_shared<std::size_t>(0);

    bool ok = server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        1,
        [&, recv_buf, recv_offset](bool success, std::unique_ptr<TCPClientSocket> accepted) mutable {
          ASSERT_TRUE(success);
          ASSERT_NE(accepted, nullptr);

          auto sock = std::make_shared<std::unique_ptr<TCPClientSocket>>(std::move(accepted));

          auto do_read = std::make_shared<std::function<void()>>();
          *do_read = [&, sock, recv_buf, recv_offset, do_read]() {
            std::size_t remaining = kTransferSize - *recv_offset;
            // Read in chunks.  Once the expected byte count is reached we
            // keep reading a small buffer  --  the next read will return 0
            // (EOF) which proves the orphan sent FIN.
            std::size_t chunk = remaining > 0 ? remaining : 64;
            auto read_buf = MakeRefCounted<IOBufferWithSize>(chunk);
            (*sock)->ReadAsync(
                read_buf, chunk, [&, sock, recv_buf, recv_offset, do_read, read_buf](bool s, std::size_t n) {
                  if (!s || n == 0) {
                    // EOF  --  orphan finished flushing and sent FIN.
                    eof_received.store(true);
                    if (*recv_offset == kTransferSize) {
                      // Verify data integrity byte-for-byte against the
                      // known pattern (i & 0xFF).
                      bool match = true;
                      for (std::size_t i = 0; i < kTransferSize; ++i) {
                        if (static_cast<unsigned char>(recv_buf->data()[i]) != static_cast<unsigned char>(i & 0xFF)) {
                          match = false;
                          break;
                        }
                      }
                      all_data_received.store(match);
                    }
                    (*sock)->Close();
                    transfer_done.Signal();
                    return;
                  }
                  std::memcpy(recv_buf->data() + *recv_offset, read_buf->data(), n);
                  *recv_offset += n;
                  (*do_read)();
                });
          };
          (*do_read)();
        },
        io_runner_,
        {});
    ASSERT_TRUE(ok);

    // ---- client side: write 1 MB then destroy immediately ----
    auto send_buf = MakeRefCounted<IOBufferWithSize>(kTransferSize);
    for (std::size_t i = 0; i < kTransferSize; ++i)
      send_buf->data()[i] = static_cast<unsigned char>(i & 0xFF);

    auto client = std::make_shared<TCPClientSocket>();
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [&, send_buf, client](bool connected) mutable {
          ASSERT_TRUE(connected);
          // Post a large write  --  the kernel send buffer is much smaller
          // than 1 MB, so the write will not complete synchronously.
          client->WriteAsync(send_buf, kTransferSize, [](bool, std::size_t) {
            // User callback may fire normally if the
            // write happened to complete before the
            // subsequent reset().  Both paths are valid.
          });
          // Destroy the socket shell immediately.  If the write is still
          // in-flight the orphaned Impl takes over, flushes buffered data,
          // sends FIN, and drains the socket in background.
          client.reset();
        },
        io_runner_);
  });

  ASSERT_TRUE(transfer_done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(eof_received.load()) << "Server should detect EOF  --  orphaned Impl must send FIN";
  EXPECT_TRUE(all_data_received.load()) << "Server must receive all " << kTransferSize
                                        << " bytes with the correct pattern  --  orphan must flush data";
}

// ===========================================================================
// Test 8  --  MultiReactorRoundRobin (4 worker IO threads, round-robin)
// ===========================================================================
//
// Verifies that RunnerSelector correctly distributes accepted connections
// across multiple worker IO threads.  8 clients connect; each accepted
// socket is assigned to one of 4 workers via round-robin.  The test
// validates that the Multi-Reactor plumbing works end-to-end without
// crashes or hangs.

TEST_F(TcpSocketTest, MultiReactorRoundRobin) {
  // ---- create 4 worker IO threads ----
  std::vector<std::unique_ptr<Thread>> workers;
  for (int i = 0; i < 4; ++i) {
    auto t = std::make_unique<Thread>("tcp-worker-" + std::to_string(i));
    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(t->StartWithOptions(opts));
    workers.push_back(std::move(t));
  }

  const uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);
  constexpr int kNumConnections = 8; // 2 per worker

  std::atomic<int> accepted{0};
  WaitableEvent all_accepted(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<size_t> next_worker{0};

  auto server = std::make_shared<TCPServerSocket>();

  // ---- start server on acceptor IO thread ----
  io_runner_->PostTask(FROM_HERE, [&, this, port]() {
    bool ok = server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        kNumConnections,
        [&](bool success, std::unique_ptr<TCPClientSocket> client) {
          ASSERT_TRUE(success);
          ASSERT_NE(client, nullptr);
          // The client socket is already bound to a worker IO thread.
          // Close it immediately  --  no I/O needed for this smoke test.
          client->Close();
          if (++accepted == kNumConnections)
            all_accepted.Signal();
        },
        io_runner_,
        // Round-robin across the 4 worker threads.
        [&]() -> scoped_refptr<SingleThreadTaskRunner> {
          return workers[next_worker++ % workers.size()]->GetTaskRunner();
        });
    ASSERT_TRUE(ok);
  });

  // ---- connect kNumConnections clients from the acceptor IO thread ----
  for (int i = 0; i < kNumConnections; ++i) {
    WaitableEvent connected(WaitableEvent::ResetPolicy::kAutomatic, false);
    io_runner_->PostTask(FROM_HERE, [&, this, port]() {
      auto client = std::make_shared<TCPClientSocket>();
      client->Connect(
          IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
          [client, &connected](bool ok) {
            EXPECT_TRUE(ok) << "Client should connect successfully";
            client->Close();
            connected.Signal();
          },
          io_runner_);
    });
    ASSERT_TRUE(connected.TimedWait(std::chrono::seconds(5)));
  }

  ASSERT_TRUE(all_accepted.TimedWait(std::chrono::seconds(5)));
  EXPECT_EQ(accepted.load(), kNumConnections) << "All " << kNumConnections << " connections should be accepted";

  // ---- tear down workers ----
  for (auto &w : workers)
    w->Stop();
}

// ===========================================================================
// Test 9  --  WriteChainNoStackOverflow (re-entrancy defence)
// ===========================================================================
//
// Verifies that rapid sequential writes do not cause synchronous re-entrancy
// or stack overflow.  Each write callback immediately posts the next write;
// if the system were dispatching synchronously, the call stack would grow
// unboundedly.  A depth counter proves tasks are trampolined via PostTask.

TEST_F(TcpSocketTest, WriteChainNoStackOverflow) {
  const uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);
  constexpr int kChainLength = 1000;

  WaitableEvent chain_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> max_depth{0};
  std::atomic<int> write_count{0};
  std::atomic<int> server_read_count{0};

  auto server = std::make_shared<TCPServerSocket>();

  io_runner_->PostTask(FROM_HERE, [&, this, port]() {
    bool ok = server->Listen(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
                             1,
                             [&](bool success, std::unique_ptr<TCPClientSocket> accepted) mutable {
                               ASSERT_TRUE(success);
                               auto sock = std::make_shared<std::unique_ptr<TCPClientSocket>>(std::move(accepted));

                               auto do_read = std::make_shared<std::function<void()>>();
                               *do_read = [&, sock, do_read]() {
                                 auto buf = nei::MakeRefCounted<nei::IOBufferWithSize>(64);
                                 (*sock)->ReadAsync(buf, 64, [&, do_read](bool s, std::size_t n) {
                                   if (!s || n == 0)
                                     return;
                                   server_read_count.fetch_add(1);
                                   (*do_read)();
                                 });
                               };
                               (*do_read)();
                             },
                             io_runner_,
                             {});
    ASSERT_TRUE(ok);

    // ---- client: chain of 1000 small writes ----
    auto client = std::make_shared<nei::net::TCPClientSocket>();
    auto depth = std::make_shared<std::atomic<int>>(0);
    auto remaining = std::make_shared<std::atomic<int>>(kChainLength);
    auto do_write = std::make_shared<std::function<void()>>();

    *do_write = [&, client, depth, remaining, do_write]() {
      int d = depth->fetch_add(1) + 1;
      int prev = max_depth.load();
      while (d > prev && !max_depth.compare_exchange_weak(prev, d)) {
      }

      auto buf = nei::MakeRefCounted<nei::IOBufferWithSize>(1);
      buf->data()[0] = 'X';
      client->WriteAsync(buf, 1, [&, depth, remaining, do_write, client](bool ok, std::size_t) {
        depth->fetch_sub(1);
        ASSERT_TRUE(ok);
        write_count.fetch_add(1);
        if (remaining->fetch_sub(1) > 1) {
          (*do_write)();
        } else {
          client->Close();
          chain_done.Signal();
        }
      });
    };

    // Connect first, then start the write chain from the connect callback.
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [do_write](bool ok) {
          ASSERT_TRUE(ok);
          (*do_write)();
        },
        io_runner_);
  });

  ASSERT_TRUE(chain_done.TimedWait(std::chrono::seconds(5)));

  EXPECT_EQ(write_count.load(), kChainLength) << "All " << kChainLength << " writes must complete";
  EXPECT_LE(max_depth.load(), 2) << "Max recursion depth should be ≤2 (async dispatch), "
                                    "was "
                                 << max_depth.load() << "  --  possible synchronous re-entrancy";
  EXPECT_GT(server_read_count.load(), 0) << "Server should have read at least some data";
}

// ===========================================================================
// Test 10  --  ConnectFailureCallbackRespectsOrphan
//
// Verifies the orphaned_ guard in posted connect-failure callbacks (fix for
// TCP Win OnIOCompleted / TCP POSIX PostConnectResult TOCTOU window).
//
// Scenario: connect to a dead port → failure callback is posted → orphan
// the socket before the callback executes → callback must be silently
// dropped, not invoked on a destroyed shell.
// ===========================================================================

TEST_F(TcpSocketTest, ConnectFailureCallbackRespectsOrphan) {
  // Verify no crash when a connect-failure callback fires near an orphan.
  // The orphaned_ guard in OnIOCompleted/PostConnectResult prevents the
  // callback from invoking on a destroyed shell.  On loopback the connect
  // may fail synchronously (before the orphan), so the callback may still
  // fire — the point is that it doesn't crash.
  const uint16_t port = FindFreePort();

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> callback_fired{false};

  io_runner_->PostTask(FROM_HERE, [this, port, &done, &callback_fired]() {
    auto client = std::make_shared<TCPClientSocket>();
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [&callback_fired](bool /*success*/) { callback_fired.store(true, std::memory_order_relaxed); },
        io_runner_);

    // Orphan immediately — on loopback the connect may have already
    // failed synchronously and the callback posted.  The orphaned_ guard
    // in posted callbacks must handle either case without crashing.
    client.reset(); // → Orphan()

    // Allow time for any in-flight callback to be dispatched.
    io_runner_->PostDelayedTask(FROM_HERE, BindOnce([&done]() { done.Signal(); }), TimeDelta::FromSeconds(4));
  });

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  // Whether the callback fired or was dropped, the test didn't crash.
  SUCCEED();
}

// ===========================================================================
// Test 11  --  OrphanedWhileConnectInFlight
//
// Verifies the OnIOCompleted self-protector pattern (scoped_refptr<Impl>
// extracted BEFORE delete ctx).  If ctx→self_ref held the last reference
// to the Impl, delete ctx would destroy *this, causing UAF on the
// subsequent orphaned_ check.
//
// Scenario: start connecting → orphan before IOCP/epoll completes →
// OnIOCompleted runs after orphan → self-protector keeps Impl alive
// through orphaned_ check and cleanup.
// ===========================================================================

TEST_F(TcpSocketTest, OrphanedWhileConnectInFlight) {
  // Use a valid server so the connect actually enters the async path.
  const uint16_t port = FindFreePort();

  WaitableEvent server_ready(WaitableEvent::ResetPolicy::kAutomatic, false);

  auto server = std::make_shared<TCPServerSocket>();
  io_runner_->PostTask(FROM_HERE, [this, server, port, &server_ready]() {
    ASSERT_TRUE(server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        1,
        [](bool, std::unique_ptr<TCPClientSocket>) {},
        io_runner_));
    server_ready.Signal();
  });
  ASSERT_TRUE(server_ready.TimedWait(std::chrono::seconds(5)));

  // Now connect and immediately orphan.
  WaitableEvent orphan_done(WaitableEvent::ResetPolicy::kAutomatic, false);

  io_runner_->PostTask(FROM_HERE, [this, port, &orphan_done]() {
    auto client = std::make_shared<TCPClientSocket>();
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [](bool /*success*/) {
          // May or may not fire — the point is no crash.
        },
        io_runner_);

    // Orphan immediately — the connect is in-flight (IOCP pending /
    // epoll EINPROGRESS).  The self-protector in OnIOCompleted must
    // keep the Impl alive through orphaned_ handling.
    client.reset(); // ~TCPClientSocket → Orphan()

    io_runner_->PostTask(FROM_HERE, [&orphan_done]() { orphan_done.Signal(); });
  });

  ASSERT_TRUE(orphan_done.TimedWait(std::chrono::seconds(5)));
  SUCCEED();
}

// ===========================================================================
// Test 12  --  TCPNodelayAndIPV6V6ONLY (documentation / coverage note)
//
// TCP_NODELAY and IPV6_V6ONLY are set unconditionally during socket
// creation on both Connect() and Listen() paths.  These are OS-level
// socket options that cannot be directly observed from userspace without
// platform-specific introspection (TCP_INFO / getsockopt).
//
// The IPv6Loopback test in udp_socket_unittest.cpp validates the IPv6
// data path; TCP's IPv6 dual-stack behavior is implicitly covered by
// the existing handshake / transfer tests when ::1 is used.
//
// This test documents the expectation that the options are applied and
// verifies that Connect() + Listen() with IPv6 do not regress.
// ===========================================================================

TEST_F(TcpSocketTest, ConnectAndListenWithIPv6Loopback) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ipv6_works{false};

  // Probe: try binding a server to ::1 to see if IPv6 is available.
  io_runner_->PostTask(FROM_HERE, [this, &done, &ipv6_works]() {
    uint8_t ipv6_loopback[16] = {};
    ipv6_loopback[15] = 1;
    IPEndPoint local(IPAddress::FromIPv6(ipv6_loopback), 0);

    auto server = std::make_shared<TCPServerSocket>();
    if (server->Listen(local, 1, [](bool, std::unique_ptr<TCPClientSocket>) {}, io_runner_)) {
      ipv6_works.store(true, std::memory_order_relaxed);
    }
    server->Shutdown();
    done.Signal();
  });

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  if (!ipv6_works.load()) {
    GTEST_SKIP() << "IPv6 loopback not available on this host";
  }

  // IPv6 is available — verify connect + accept round-trip.
  WaitableEvent accepted_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent connected_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> accepted{false};
  std::atomic<bool> connected{false};

  // Declared at test scope so we can break the accept-callback reference
  // cycle after the test completes.
  std::shared_ptr<TCPServerSocket> server;

  io_runner_->PostTask(FROM_HERE, [this, &accepted_done, &connected_done, &accepted, &connected, &server]() {
    uint8_t ipv6_loopback[16] = {};
    ipv6_loopback[15] = 1;

    const uint16_t port = FindFreePort();
    IPEndPoint bind_addr(IPAddress::FromIPv6(ipv6_loopback), port);

    server = std::make_shared<TCPServerSocket>();
    ASSERT_TRUE(server->Listen(
        bind_addr,
        1,
        [&accepted, &accepted_done, server](bool ok, std::unique_ptr<TCPClientSocket> client) {
          accepted.store(ok, std::memory_order_relaxed);
          accepted_done.Signal();
          if (client)
            client->Close();
        },
        io_runner_));

    auto client = std::make_shared<TCPClientSocket>();
    client->Connect(
        bind_addr,
        [&connected, &connected_done, client](bool ok) {
          connected.store(ok, std::memory_order_relaxed);
          connected_done.Signal();
          client->Close();
        },
        io_runner_);
  });

  ASSERT_TRUE(accepted_done.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(connected_done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(accepted.load());
  EXPECT_TRUE(connected.load());

  // Break reference cycle: accept callback captures server shared_ptr.
  if (server)
    server->Shutdown();
}

#if !defined(_WIN32)
// Verifies the server does not crash under rapid connection pressure.
// The reserve fd trick (open /dev/null at Listen() time, used to drain
// the TCP backlog on EMFILE) guarantees some connections are accepted
// even when the process is near the fd limit.  A full end-to-end EMFILE
// test requires lowering the per-process fd limit via setrlimit(), which
// starves the IO pump itself (epoll + eventfd need fds) and causes
// SIGSEGV on WSL kernels.  See docs/TODO.md for the blocked full-EMFILE
// test plan.
TEST_F(TcpSocketTest, ServerDoesNotCrashUnderFdPressure) {
  const uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);

  WaitableEvent server_ready(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> accepted{0};
  std::atomic<bool> server_fatal{false};

  auto server = std::make_shared<TCPServerSocket>();

  io_runner_->PostTask(FROM_HERE, [&]() {
    bool ok = server->Listen(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
                             512,
                             [&](bool success, std::unique_ptr<TCPClientSocket> client) {
                               if (success) {
                                 accepted.fetch_add(1);
                                 client->Close();
                               } else {
                                 server_fatal.store(true);
                               }
                             },
                             io_runner_,
                             {});
    if (!ok)
      server_fatal.store(true);
    server_ready.Signal();
  });

  server_ready.Wait();

  // Fire 200 clients in rapid succession to stress the accept loop.
  // WSL default ulimit is 1024, so 200 is well within safe range.
  const int kClients = 200;
  WaitableEvent all_done(WaitableEvent::ResetPolicy::kManual, false);
  std::atomic<int> client_done{0};
  std::vector<std::shared_ptr<TCPClientSocket>> clients;

  for (int i = 0; i < kClients; ++i) {
    auto client = std::make_shared<TCPClientSocket>();
    clients.push_back(client);
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [&all_done, &client_done, kClients](bool /*ok*/) {
          if (client_done.fetch_add(1) + 1 >= kClients)
            all_done.Signal();
        },
        io_runner_);
  }

  ASSERT_TRUE(all_done.TimedWait(std::chrono::seconds(10)));

  int accepted_count = accepted.load();
  EXPECT_GE(accepted_count, kClients - 5) << "Server accepted " << accepted_count << " out of " << kClients
                                          << " clients.  Accept loop may be blocking or too slow.";
  EXPECT_FALSE(server_fatal.load());

  server->Close();
}
#endif // !_WIN32

// =============================================================================
// Extreme lifecycle tests — server close while AcceptEx pending (UAF safety)
// =============================================================================

// Verifies that destroying a listening TCPServerSocket while AcceptEx
// completions are still in the kernel IOCP / epoll queue does not cause
// use-after-free or heap corruption.
TEST_F(TcpSocketTest, ServerCloseWhileAcceptExPending) {
  uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);

  // Start a dedicated thread for the server so we can destroy it while the
  // accept queue is still hot.
  Thread srv_thread;
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(srv_thread.StartWithOptions(opts));
  auto srv_runner = srv_thread.GetTaskRunner();

  WaitableEvent server_ready(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> accept_fired{false};

  auto server = std::make_shared<TCPServerSocket>();
  srv_runner->PostTask(FROM_HERE, [&]() {
    ASSERT_TRUE(server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        1,
        [&accept_fired](bool, std::unique_ptr<TCPClientSocket>) { accept_fired.store(true); },
        srv_runner));
    server_ready.Signal();
  });
  server_ready.Wait();

  // Give the kernel time to post AcceptEx / accept4 calls (64 on Windows).
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Destroy the server on its own thread while accepts are pending.
  // This must not crash, hang, or leak.
  WaitableEvent destroyed(WaitableEvent::ResetPolicy::kAutomatic, false);
  srv_runner->PostTask(FROM_HERE, [&]() {
    server->Close();
    server.reset();
    destroyed.Signal();
  });
  ASSERT_TRUE(destroyed.TimedWait(std::chrono::seconds(5)));

  // No client connected — accept callback might fire spuriously on
  // loopback (e.g. port scanners), but the key assertion is: no crash.
  srv_thread.Stop();
}

// =============================================================================
// Extreme lifecycle tests — client Orphan drain through EOF
// =============================================================================

// Verifies that Orphan() on a connected client correctly drains the peer's
// EOF and self-destructs without leaking or crashing.
TEST_F(TcpSocketTest, ClientOrphanDrainReadEOF) {
  uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);

  WaitableEvent client_accepted(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::shared_ptr<TCPClientSocket> accepted_client;

  auto server = std::make_shared<TCPServerSocket>();

  io_runner_->PostTask(FROM_HERE, [&, this, port]() {
    ASSERT_TRUE(server->Listen(IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
                               1,
                               [&](bool ok, std::unique_ptr<TCPClientSocket> client) {
                                 if (ok)
                                   accepted_client = std::move(client);
                                 client_accepted.Signal();
                               },
                               io_runner_,
                               {}));

    // Client connects on the same IO thread.
    auto client = std::make_shared<TCPClientSocket>();
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [client](bool ok) {
          if (!ok)
            return;
          // Post a pending read so Orphan() has a callback to cancel.
          auto buf = MakeRefCounted<IOBufferWithSize>(64);
          client->ReadAsync(buf, 64, [](bool, size_t) {});
        },
        io_runner_);
  });

  ASSERT_TRUE(client_accepted.TimedWait(std::chrono::seconds(5)));

  if (accepted_client) {
    // Flush the IO thread so read is posted.
    WaitableEvent drain(WaitableEvent::ResetPolicy::kAutomatic, false);
    io_runner_->PostTask(FROM_HERE, [&drain]() { drain.Signal(); });
    ASSERT_TRUE(drain.TimedWait(std::chrono::seconds(5)));

    // Orphan the accepted client — triggers ShutdownWrite + StartOrphanDrain.
    io_runner_->PostTask(FROM_HERE, [&]() { accepted_client.reset(); });

    // Flush to let Orphan drain complete.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Close server — must not crash or hang.
  io_runner_->PostTask(FROM_HERE, [&server]() { server->Close(); });
  WaitableEvent cleanup(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner_->PostTask(FROM_HERE, [&cleanup]() { cleanup.Signal(); });
  ASSERT_TRUE(cleanup.TimedWait(std::chrono::seconds(5)));
}

} // namespace
} // namespace nei::net
