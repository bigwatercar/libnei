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
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

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
      WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)wsa_ready;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
      ::closesocket(s); return 0;
    }
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
    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      ::close(fd); return 0;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(fd, (struct sockaddr*)&addr, &len);
    ::close(fd);
    return ntohs(addr.sin_port);
#endif
  }

  Thread io_thread_{"tcp-test-io"};
  scoped_refptr<TaskRunner> io_runner_;
};

// ===========================================================================
// Test 1  --  BasicHandshake
// ===========================================================================

TEST_F(TcpSocketTest, BasicHandshake) {
  const uint16_t port = 19321;

  WaitableEvent server_accept_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent client_connect_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> accepted{false};
  std::atomic<bool> connected{false};

  // shared_ptr keeps server and client alive beyond the PostTask closure.
  auto server = std::make_shared<TCPServerSocket>();
  auto client = std::make_shared<TCPClientSocket>();

  io_runner_->PostTask(FROM_HERE, [server, client, this, port,
                                    &server_accept_done, &client_connect_done,
                                    &accepted, &connected]() {
    bool ok = server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), 1,
        [server, &server_accept_done, &accepted](
            bool success, std::unique_ptr<TCPClientSocket> accepted_client) {
          accepted.store(success);
          EXPECT_TRUE(success) << "Server accept should succeed";
          EXPECT_NE(accepted_client, nullptr);
          if (accepted_client) accepted_client->Close();
          server_accept_done.Signal();
        },
        io_runner_, {});
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

  server_accept_done.Wait();
  client_connect_done.Wait();

  EXPECT_TRUE(accepted.load());
  EXPECT_TRUE(connected.load());
}

// ===========================================================================
// Test 2  --  AsyncStreamTransfer (1 MB zero-copy)
// ===========================================================================

TEST_F(TcpSocketTest, AsyncStreamTransfer) {
  const uint16_t port = 19322;
  ASSERT_NE(port, 0);
  constexpr std::size_t kTransferSize = 1024 * 1024;

  WaitableEvent transfer_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> data_match{false};

  auto server = std::make_shared<TCPServerSocket>();
  auto client = std::make_shared<TCPClientSocket>();

  io_runner_->PostTask(FROM_HERE, [server, client, &transfer_done, &data_match,
                                    this, port, kTransferSize]() {
    auto ref_buf = MakeRefCounted<IOBufferWithSize>(kTransferSize);
    for (std::size_t i = 0; i < kTransferSize; ++i)
      ref_buf->data()[i] = static_cast<char>(i & 0xFF);

    auto recv_buf = MakeRefCounted<IOBufferWithSize>(kTransferSize);
    std::memset(recv_buf->data(), 0, kTransferSize);
    auto recv_offset = std::make_shared<std::size_t>(0);

    // Pass as scoped_refptr<IOBuffer>  --  IOBufferWithSize inherits from
    // IOBuffer with a single RefCountedThreadSafe<IOBuffer> base, so the
    // refcount is shared even when the pointer type differs.
    scoped_refptr<IOBuffer> ref_base(ref_buf.get());
    scoped_refptr<IOBuffer> recv_base(recv_buf.get());

    bool ok = server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), 1,
        [server, &transfer_done, &data_match,
         ref_base, recv_base, recv_offset, kTransferSize](
            bool success, std::unique_ptr<TCPClientSocket> accepted) mutable {
          ASSERT_TRUE(success);
          ASSERT_NE(accepted, nullptr);

          // Move accepted into shared ownership so it survives the callback.
          auto sock = std::make_shared<std::unique_ptr<TCPClientSocket>>(
              std::move(accepted));

          auto do_read = std::make_shared<std::function<void()>>();
          *do_read = [do_read, &transfer_done, &data_match,
                       sock,
                       recv_base, recv_offset,
                       ref_base, kTransferSize]() {
            auto* accepted = sock->get();
            std::size_t remaining = kTransferSize - *recv_offset;
            if (remaining == 0) {
              data_match.store(
                  std::memcmp(ref_base->data(), recv_base->data(),
                              kTransferSize) == 0);
              (*sock)->Close();
              transfer_done.Signal();
              return;
            }
            // Read into the correct offset within the buffer.
            // IOBuffer::data() always returns the base pointer, so we
            // create a temporary buffer and memcpy to the right spot.
            auto read_buf = MakeRefCounted<IOBufferWithSize>(remaining);
            accepted->ReadAsync(
                read_buf, remaining,
                [recv_offset, recv_base, do_read, read_buf](bool s, std::size_t n) {
                  EXPECT_TRUE(s);
                  EXPECT_GT(n, 0u);
                  // Copy received data to the correct position.
                  std::memcpy(recv_base->data() + *recv_offset,
                              read_buf->data(), n);
                  *recv_offset += n;
                  (*do_read)();
                });
          };
          (*do_read)();
        },
        io_runner_, {});
    ASSERT_TRUE(ok);

    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [client, ref_base, port, kTransferSize](bool connected_ok) {
          ASSERT_TRUE(connected_ok);
          client->WriteAsync(ref_base, kTransferSize,
                             [kTransferSize](bool s, std::size_t n) {
                               EXPECT_TRUE(s);
                               EXPECT_EQ(n, kTransferSize);
                             });
        },
        io_runner_);
  });

  transfer_done.Wait();
  EXPECT_TRUE(data_match.load()) << "1 MB data should match byte-for-byte";
}

// ===========================================================================
// Test 3  --  ConnectionRefused (async failure detection)
// ===========================================================================

TEST_F(TcpSocketTest, ConnectionRefused) {
  const uint16_t port = 19323;
  ASSERT_NE(port, 0);

  WaitableEvent connect_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> connect_result{true};

  auto client = std::make_shared<TCPClientSocket>();

  io_runner_->PostTask(FROM_HERE, [client, port, &connect_done,
                                    &connect_result, this]() {
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [client, &connect_done, &connect_result](bool ok) {
          connect_result.store(ok);
          connect_done.Signal();
        },
        io_runner_);
  });

  connect_done.Wait();
  EXPECT_FALSE(connect_result.load())
      << "Connect to dead port should report failure";
}

// ===========================================================================
// Test 4  --  ServerDestructionWhilePending (no crash, no leak)
// ===========================================================================

TEST_F(TcpSocketTest, ServerDestructionWhilePending) {
  const uint16_t port = 19324;
  ASSERT_NE(port, 0);

  WaitableEvent server_created(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent server_destroyed(WaitableEvent::ResetPolicy::kAutomatic, false);

  io_runner_->PostTask(FROM_HERE, [&]() {
    auto server = std::make_unique<TCPServerSocket>();
    bool ok = server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), 1,
        [](bool /*success*/, std::unique_ptr<TCPClientSocket> /*client*/) {
          ADD_FAILURE() << "Accept callback should not fire after destruction";
        },
        io_runner_, {});
    ASSERT_TRUE(ok);
    server_created.Signal();

    server.reset();
    server_destroyed.Signal();
  });

  server_created.Wait();
  server_destroyed.Wait();
  SUCCEED();
}

// ===========================================================================
// Test 5  --  ExplicitShutdownWrite (half-close handshake)
// ===========================================================================

TEST_F(TcpSocketTest, ExplicitShutdownWrite) {
  const uint16_t port = 19325;
  ASSERT_NE(port, 0);

  WaitableEvent eof_detected(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> server_saw_eof{false};

  auto server = std::make_shared<TCPServerSocket>();
  auto client = std::make_shared<TCPClientSocket>();

  io_runner_->PostTask(FROM_HERE, [&, this, port]() {
    bool ok = server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), 1,
        [&](bool success,
            std::unique_ptr<TCPClientSocket> accepted) mutable {
          ASSERT_TRUE(success);
          auto sock = std::make_shared<std::unique_ptr<TCPClientSocket>>(
              std::move(accepted));

          // Keep reading until EOF  --  proof that ShutdownWrite sent FIN.
          auto buf = MakeRefCounted<IOBufferWithSize>(64);
          auto do_read = std::make_shared<std::function<void()>>();
          *do_read = [&, sock, buf, do_read]() {
            (*sock)->ReadAsync(
                buf, 64,
                [&, do_read](bool s, std::size_t n) {
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
        io_runner_, {});
    ASSERT_TRUE(ok);

    auto send_buf = MakeRefCounted<IOBufferWithSize>(64);
    for (int i = 0; i < 64; ++i) send_buf->data()[i] = 'A';

    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [&, send_buf](bool connected) {
          ASSERT_TRUE(connected);
          client->WriteAsync(send_buf, 64, [&](bool, std::size_t) {
            client->ShutdownWrite();  // Send FIN, read stays open.
          });
        },
        io_runner_);
  });

  eof_detected.Wait();
  EXPECT_TRUE(server_saw_eof.load())
      << "Server should detect EOF after client ShutdownWrite";
}

// ===========================================================================
// Test 6  --  OrphanedDestruction_FoolproofFallback
// ===========================================================================

TEST_F(TcpSocketTest, OrphanedDestruction) {
  const uint16_t port = 19326;
  ASSERT_NE(port, 0);

  WaitableEvent accepted(WaitableEvent::ResetPolicy::kAutomatic, false);

  auto server = std::make_shared<TCPServerSocket>();

  io_runner_->PostTask(FROM_HERE, [&, this, port]() {
    bool ok = server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), 1,
        [&accepted](bool success,
                     std::unique_ptr<TCPClientSocket> client) {
          // Just signal acceptance  --  client will be orphaned.
          accepted.Signal();
        },
        io_runner_, {});
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
  accepted.Wait();
  WaitableEvent drain(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner_->PostTask(FROM_HERE, [&drain]() { drain.Signal(); });
  drain.Wait();

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
  constexpr std::size_t kTransferSize = 1024 * 1024;  // 1 MB

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
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), 1,
        [&, recv_buf, recv_offset](
            bool success,
            std::unique_ptr<TCPClientSocket> accepted) mutable {
          ASSERT_TRUE(success);
          ASSERT_NE(accepted, nullptr);

          auto sock = std::make_shared<std::unique_ptr<TCPClientSocket>>(
              std::move(accepted));

          auto do_read = std::make_shared<std::function<void()>>();
          *do_read = [&, sock, recv_buf, recv_offset, do_read]() {
            std::size_t remaining = kTransferSize - *recv_offset;
            // Read in chunks.  Once the expected byte count is reached we
            // keep reading a small buffer  --  the next read will return 0
            // (EOF) which proves the orphan sent FIN.
            std::size_t chunk = remaining > 0 ? remaining : 64;
            auto read_buf = MakeRefCounted<IOBufferWithSize>(chunk);
            (*sock)->ReadAsync(
                read_buf, chunk,
                [&, sock, recv_buf, recv_offset, do_read, read_buf](
                    bool s, std::size_t n) {
                  if (!s || n == 0) {
                    // EOF  --  orphan finished flushing and sent FIN.
                    eof_received.store(true);
                    if (*recv_offset == kTransferSize) {
                      // Verify data integrity byte-for-byte against the
                      // known pattern (i & 0xFF).
                      bool match = true;
                      for (std::size_t i = 0; i < kTransferSize; ++i) {
                        if (static_cast<unsigned char>(
                                recv_buf->data()[i]) !=
                            static_cast<unsigned char>(i & 0xFF)) {
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
                  std::memcpy(recv_buf->data() + *recv_offset,
                              read_buf->data(), n);
                  *recv_offset += n;
                  (*do_read)();
                });
          };
          (*do_read)();
        },
        io_runner_, {});
    ASSERT_TRUE(ok);

    // ---- client side: write 1 MB then destroy immediately ----
    auto send_buf = MakeRefCounted<IOBufferWithSize>(kTransferSize);
    for (std::size_t i = 0; i < kTransferSize; ++i)
      send_buf->data()[i] = static_cast<char>(i & 0xFF);

    auto client = std::make_shared<TCPClientSocket>();
    client->Connect(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        [&, send_buf, client](bool connected) mutable {
          ASSERT_TRUE(connected);
          // Post a large write  --  the kernel send buffer is much smaller
          // than 1 MB, so the write will not complete synchronously.
          client->WriteAsync(send_buf, kTransferSize,
                             [](bool, std::size_t) {
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

  transfer_done.Wait();
  EXPECT_TRUE(eof_received.load())
      << "Server should detect EOF  --  orphaned Impl must send FIN";
  EXPECT_TRUE(all_data_received.load())
      << "Server must receive all " << kTransferSize
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
    auto t = std::make_unique<Thread>(
        "tcp-worker-" + std::to_string(i));
    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(t->StartWithOptions(opts));
    workers.push_back(std::move(t));
  }

  const uint16_t port = FindFreePort();
  ASSERT_NE(port, 0);
  constexpr int kNumConnections = 8;  // 2 per worker

  std::atomic<int> accepted{0};
  WaitableEvent all_accepted(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<size_t> next_worker{0};

  auto server = std::make_shared<TCPServerSocket>();

  // ---- start server on acceptor IO thread ----
  io_runner_->PostTask(FROM_HERE, [&, this, port]() {
    bool ok = server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port),
        kNumConnections,
        [&](bool success,
            std::unique_ptr<TCPClientSocket> client) {
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
        [&]() -> scoped_refptr<TaskRunner> {
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
    connected.Wait();
  }

  all_accepted.Wait();
  EXPECT_EQ(accepted.load(), kNumConnections)
      << "All " << kNumConnections << " connections should be accepted";

  // ---- tear down workers ----
  for (auto& w : workers)
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
    bool ok = server->Listen(
        IPEndPoint(IPAddress::FromIPv4(127, 0, 0, 1), port), 1,
        [&](bool success,
            std::unique_ptr<TCPClientSocket> accepted) mutable {
          ASSERT_TRUE(success);
          auto sock = std::make_shared<std::unique_ptr<TCPClientSocket>>(
              std::move(accepted));

          auto do_read = std::make_shared<std::function<void()>>();
          *do_read = [&, sock, do_read]() {
            auto buf = nei::MakeRefCounted<nei::IOBufferWithSize>(64);
            (*sock)->ReadAsync(buf, 64,
                [&, do_read](bool s, std::size_t n) {
                  if (!s || n == 0) return;
                  server_read_count.fetch_add(1);
                  (*do_read)();
                });
          };
          (*do_read)();
        },
        io_runner_, {});
    ASSERT_TRUE(ok);

    // ---- client: chain of 1000 small writes ----
    auto client = std::make_shared<nei::net::TCPClientSocket>();
    auto depth = std::make_shared<std::atomic<int>>(0);
    auto remaining = std::make_shared<std::atomic<int>>(kChainLength);
    auto do_write = std::make_shared<std::function<void()>>();

    *do_write = [&, client, depth, remaining, do_write]() {
      int d = depth->fetch_add(1) + 1;
      int prev = max_depth.load();
      while (d > prev && !max_depth.compare_exchange_weak(prev, d)) {}

      auto buf = nei::MakeRefCounted<nei::IOBufferWithSize>(1);
      buf->data()[0] = 'X';
      client->WriteAsync(buf, 1,
          [&, depth, remaining, do_write, client](bool ok, std::size_t) {
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

  chain_done.Wait();

  EXPECT_EQ(write_count.load(), kChainLength)
      << "All " << kChainLength << " writes must complete";
  EXPECT_LE(max_depth.load(), 2)
      << "Max recursion depth should be ≤2 (async dispatch), "
         "was " << max_depth.load() << "  --  possible synchronous re-entrancy";
  EXPECT_GT(server_read_count.load(), 0)
      << "Server should have read at least some data";
}

}  // namespace
}  // namespace nei::net
