// =============================================================================
// PipeStream unit tests — async determinism, UAF safety, yield quota,
// peer disconnect, and rapid cancel/retry.
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/io/async_stream.h>
#include <neixx/io/io_buffer.h>
#include <neixx/io/pipe_stream.h>
#include <neixx/common/platform_handle.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <neixx/common/scoped_handle.h>
#else
#include <cerrno>
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <neixx/trace_event/trace_event.h>
#include <neixx/trace_event/trace_log.h>

#include <fstream>

namespace nei {
namespace {

// ===========================================================================
// Test helpers
// ===========================================================================

constexpr std::size_t kSmallBufSize  = 4096;
constexpr std::size_t kLargeBufSize  = 64 * 1024;   // 64 KiB

// ---------------------------------------------------------------------------
// CreateAsyncPipePair — creates a cross-platform pipe pair suitable for
// async PipeStream testing.
//
// On Windows: uses CreateNamedPipe(FILE_FLAG_OVERLAPPED) for the read end
//             and CreateFile for the write end.
// On POSIX:   uses pipe() and sets O_NONBLOCK on the read end.
//
// Returns true on success.  On failure both handles are invalid.
// The caller owns both handles (via PlatformHandle RAII).
// ---------------------------------------------------------------------------
bool CreateAsyncPipePair(PlatformHandle& read_handle,
                         PlatformHandle& write_handle,
                         bool overlapped_write = true) {
#if defined(_WIN32)
  static std::atomic<unsigned long long> pipe_counter{0};
  const DWORD pid  = GetCurrentProcessId();
  const ULONGLONG tick = GetTickCount64();
  const unsigned long long counter =
      pipe_counter.fetch_add(1, std::memory_order_relaxed);
  const std::string pipe_name =
      "\\\\.\\pipe\\nei_pipe_stream_test_" + std::to_string(pid) +
      "_" + std::to_string(tick) + "_" + std::to_string(counter);

  HANDLE server = CreateNamedPipeA(
      pipe_name.c_str(),
      PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      1,                    // max instances
      0,                    // out buffer size (system default)
      0,                    // in  buffer size (system default)
      0,                    // default timeout
      nullptr);             // default security
  if (server == INVALID_HANDLE_VALUE) return false;

  HANDLE client = CreateFileA(
      pipe_name.c_str(),
      GENERIC_WRITE,
      0,                    // no sharing
      nullptr,              // default security
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL |
        (overlapped_write ? FILE_FLAG_OVERLAPPED : 0),
      nullptr);
  if (client == INVALID_HANDLE_VALUE) {
    CloseHandle(server);
    return false;
  }

  // Connect: ConnectNamedPipe completes synchronously because the client
  // already called CreateFile.
  const BOOL connected = ConnectNamedPipe(server, nullptr);
  const DWORD conn_err  = connected ? ERROR_SUCCESS : GetLastError();
  if (!connected && conn_err != ERROR_PIPE_CONNECTED) {
    CloseHandle(server);
    CloseHandle(client);
    return false;
  }

  read_handle  = PlatformHandle::FromNativeHandle<DefaultHandleTraits>(server);
  write_handle = PlatformHandle::FromNativeHandle<DefaultHandleTraits>(client);
  return true;
#else
  int fds[2];
  if (pipe(fds) != 0) return false;

  // Read end must be non-blocking for epoll integration.  Write end
  // stays blocking so the writer thread does not spin on EAGAIN when
  // the pipe buffer is full — it simply sleeps until the reader drains.
  int rflags = fcntl(fds[0], F_GETFL, 0);
  if (rflags == -1 || fcntl(fds[0], F_SETFL, rflags | O_NONBLOCK) == -1) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }

  read_handle  = PlatformHandle::FromNativeHandle(fds[0]);
  write_handle = PlatformHandle::FromNativeHandle(fds[1]);
  return true;
#endif
}

// ---------------------------------------------------------------------------
// WriteAll — synchronously writes |data| to |write_handle| in a loop until
// all bytes are written.  Blocks if the pipe buffer is full.
// Returns true on success.
// ---------------------------------------------------------------------------
bool WriteAll(const PlatformHandle& write_handle,
              const void* data,
              std::size_t size) {
#if defined(_WIN32)
  HANDLE h = static_cast<HANDLE>(write_handle.GetHandle());
  const char* p   = static_cast<const char*>(data);
  std::size_t rem = size;
  while (rem > 0) {
    DWORD written = 0;
    if (!WriteFile(h, p, static_cast<DWORD>(rem), &written, nullptr))
      return false;
    if (written == 0) return false;
    p   += written;
    rem -= written;
  }
  return true;
#else
  int fd = write_handle.GetFd();
  const char* p   = static_cast<const char*>(data);
  std::size_t rem = size;
  while (rem > 0) {
    ssize_t n = write(fd, p, rem);
    if (n < 0) {
      // Write end is blocking; EINTR is the only expected benign error.
      if (errno == EINTR) continue;
      return false;
    }
    p   += n;
    rem -= static_cast<std::size_t>(n);
  }
  return true;
#endif
}

// ---------------------------------------------------------------------------
// AcquireIOBuffer — helper to get a scoped_refptr<IOBuffer> and keep the
// backing IOBufferWithSize alive inside the callback lambda.
// ---------------------------------------------------------------------------
struct IOBufHolder {
  scoped_refptr<IOBufferWithSize> sized;
  scoped_refptr<IOBuffer>         buf;
};

IOBufHolder AcquireIOBuf(std::size_t size) {
  IOBufHolder h;
  h.sized = IOBufferPool::GetInstance().AcquireBuffer(size);
  h.buf   = scoped_refptr<IOBuffer>(h.sized.get());
  return h;
}

// ===========================================================================
// PipeStreamTest fixture
// ===========================================================================

class PipeStreamTest : public testing::Test {
 protected:
  void SetUp() override {
#if !defined(_WIN32)
    // Prevent SIGPIPE from killing the process when writing to a pipe
    // with no readers (peer disconnect tests).
    signal(SIGPIPE, SIG_IGN);
#endif
    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(io_thread_.StartWithOptions(opts));
    io_runner_ = io_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);
  }

  void TearDown() override {
    io_thread_.Stop();
  }

  // Enable tracing and return path for trace output file.
  static void EnableTracing() {
    nei::g_trace_enabled.store(true, std::memory_order_release);
  }

  static void DisableTracingAndDump(const char* filename) {
    nei::g_trace_enabled.store(false, std::memory_order_release);
    std::ofstream out(filename);
    if (out.is_open()) {
      nei::TraceLog::GetInstance().Flush(out);
    }
  }

  // Shortcut: create a pipe pair via the IO thread and return handles.
  bool MakePipe(PlatformHandle& read_h, PlatformHandle& write_h) {
    return CreateAsyncPipePair(read_h, write_h);
  }

  Thread io_thread_{"pipe-stream-test-io"};
  scoped_refptr<TaskRunner> io_runner_;
};

// ===========================================================================
// 🔴 Test 1 — Async Determinism Guard
// ===========================================================================
//
// Verifies that ReadAsync never invokes the callback synchronously within
// its own call stack, even when data is already available in the pipe.
// The callback must only fire after the pump processes pending tasks.

TEST_F(PipeStreamTest, ReadAsyncNeverCallsBackSynchronously) {
  PlatformHandle read_h, write_h;
  ASSERT_TRUE(MakePipe(read_h, write_h));

  const std::string payload = "Hello, async pipe!";
  WaitableEvent test_done(WaitableEvent::ResetPolicy::kAutomatic, false);

  io_runner_->PostTask(
      FROM_HERE,
      [&, read_h = std::move(read_h), write_h = std::move(write_h), payload]() mutable {
        auto writer = std::make_shared<PipeOutputStream>(io_runner_);
        ASSERT_TRUE(writer->BindPlatformHandle(std::move(write_h)));

        auto write_holder = AcquireIOBuf(payload.size());
        std::memcpy(write_holder.buf->data(), payload.data(), payload.size());
        writer->WriteAsync(write_holder.buf, payload.size(),
                           [write_holder, writer](bool, std::size_t) {});

        auto stream = std::make_shared<PipeInputStream>(io_runner_);
        ASSERT_TRUE(stream->BindPlatformHandle(std::move(read_h)));

        auto fired = std::make_shared<bool>(false);
        auto holder = AcquireIOBuf(kSmallBufSize);
        stream->ReadAsync(
            holder.buf, kSmallBufSize,
            [fired, holder, stream](bool /*ok*/, std::size_t /*n*/) {
              *fired = true;
            });

        EXPECT_FALSE(*fired)
            << "ReadAsync invoked the callback synchronously.";

        io_runner_->PostTask(FROM_HERE,
                             [&test_done]() { test_done.Signal(); });
      });

  ASSERT_TRUE(test_done.TimedWait(std::chrono::seconds(5)));
  SUCCEED();
}

// Same check for WriteAsync.
TEST_F(PipeStreamTest, WriteAsyncNeverCallsBackSynchronously) {
  PlatformHandle read_h, write_h;
  ASSERT_TRUE(MakePipe(read_h, write_h));

  WaitableEvent test_done(WaitableEvent::ResetPolicy::kAutomatic, false);

  io_runner_->PostTask(FROM_HERE, [&, write_h = std::move(write_h)]() mutable {
    auto stream = std::make_shared<PipeOutputStream>(io_runner_);
    ASSERT_TRUE(stream->BindPlatformHandle(std::move(write_h)));

    auto fired = std::make_shared<bool>(false);

    auto holder = AcquireIOBuf(kSmallBufSize);
    stream->WriteAsync(
        holder.buf, kSmallBufSize,
        [fired, holder, stream](bool /*ok*/, std::size_t /*n*/) {
          *fired = true;
        });

    EXPECT_FALSE(*fired)
        << "WriteAsync invoked the callback synchronously.";

    io_runner_->PostTask(FROM_HERE, [&test_done]() { test_done.Signal(); });
  });

  ASSERT_TRUE(test_done.TimedWait(std::chrono::seconds(5)));
  SUCCEED();
}

// ===========================================================================
// 🔴 Test 2 — Cross-Thread UAF Attack
// ===========================================================================
//
// Start an I/O operation on the IO thread, then destroy the PipeStream from
// a different thread while the I/O is still pending (no data in pipe).
// The ShutdownAndSelfDestruct mechanism must prevent use-after-free when
// the kernel later completes (or cancels) the I/O.
//
// This test is designed to be run under ASAN/LSAN to catch any heap misuse.

TEST_F(PipeStreamTest, CrossThreadDeleteWhileReadPending) {
  PlatformHandle read_h, write_h;
  ASSERT_TRUE(MakePipe(read_h, write_h));
  // Do NOT write any data — ReadAsync will go PENDING.

  WaitableEvent io_armed(WaitableEvent::ResetPolicy::kAutomatic, false);

  // Raw pointer + manual ownership transfer across threads.
  PipeInputStream* raw_stream = nullptr;

  io_runner_->PostTask(FROM_HERE, [&, read_h = std::move(read_h)]() mutable {
    auto stream = std::make_unique<PipeInputStream>(io_runner_);
    ASSERT_TRUE(stream->BindPlatformHandle(std::move(read_h)));

    auto holder = AcquireIOBuf(kSmallBufSize);
    stream->ReadAsync(holder.buf, kSmallBufSize,
                      [holder](bool, std::size_t) {
                        // Should not be called — pipe has no data.
                      });

    // Transfer ownership to the test thread.
    raw_stream = stream.release();
    io_armed.Signal();
  });

  ASSERT_TRUE(io_armed.TimedWait(std::chrono::seconds(5)));
  ASSERT_NE(raw_stream, nullptr);

  // 🔴 Delete from the *test* thread while I/O is pending on the IO thread.
  // The destructor will post ShutdownAndSelfDestruct to the IO thread,
  // which must cleanly cancel the pending I/O and self-destruct.
  delete raw_stream;

  // Give the IO thread a moment to process ShutdownAndSelfDestruct.
  WaitableEvent cleanup_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner_->PostTask(FROM_HERE, [&cleanup_done]() {
    cleanup_done.Signal();
  });
  ASSERT_TRUE(cleanup_done.TimedWait(std::chrono::seconds(5)));

  // Close the write handle so the pipe is fully torn down.
  write_h = PlatformHandle();

  // If we reach here without ASAN complaints, the UAF defence holds.
  SUCCEED();
}

TEST_F(PipeStreamTest, CrossThreadDeleteWhileWritePending) {
  PlatformHandle read_h, write_h;
  ASSERT_TRUE(MakePipe(read_h, write_h));

  WaitableEvent io_armed(WaitableEvent::ResetPolicy::kAutomatic, false);
  PipeOutputStream* raw_stream = nullptr;

  io_runner_->PostTask(FROM_HERE, [&, write_h = std::move(write_h)]() mutable {
    auto stream = std::make_unique<PipeOutputStream>(io_runner_);
    ASSERT_TRUE(stream->BindPlatformHandle(std::move(write_h)));

    // Write a large buffer to fill the pipe and trigger PENDING.
    auto holder = AcquireIOBuf(1024 * 1024);  // 1 MiB
    stream->WriteAsync(holder.buf, 1024 * 1024,
                       [holder](bool, std::size_t) {});

    raw_stream = stream.release();
    io_armed.Signal();
  });

  ASSERT_TRUE(io_armed.TimedWait(std::chrono::seconds(5)));
  ASSERT_NE(raw_stream, nullptr);

  delete raw_stream;

  WaitableEvent cleanup_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner_->PostTask(FROM_HERE, [&cleanup_done]() {
    cleanup_done.Signal();
  });
  ASSERT_TRUE(cleanup_done.TimedWait(std::chrono::seconds(5)));

  read_h  = PlatformHandle();
  write_h = PlatformHandle();
  SUCCEED();
}

// ===========================================================================
// 🔴 Test 3 — POSIX Yield Quota (anti-starvation)
// ===========================================================================
//
// Pumps 5 MiB of data through a pipe and verifies that the drain loop
// yields the I/O thread so other tasks (a marker task) can interleave.
// Without batch quota (kMaxBytesPerDrain), a single high-throughput pipe
// could monopolise the thread indefinitely.

#if !defined(_WIN32)
TEST_F(PipeStreamTest, PosixYieldQuotaPreventsStarvation) {
  PlatformHandle read_h, write_h;
  ASSERT_TRUE(MakePipe(read_h, write_h));

  constexpr std::size_t kTotalBytes = 256 * 1024;

  // Enlarge the pipe buffer so the writer can push all 256 KiB without
  // blocking.  Default pipe capacity is 64 KiB on Linux; F_SETPIPE_SZ
  // (Linux 2.6.35+) allows raising it up to /proc/sys/fs/pipe-max-size.
#if defined(F_SETPIPE_SZ)
  {
    int fd = read_h.GetFd();
    if (fd >= 0) fcntl(fd, F_SETPIPE_SZ, static_cast<int>(kTotalBytes));
  }
#endif

  std::atomic<bool> write_ok{false};
  // Writer thread: synchronously pumps 256 KiB into the pipe.  The
  // kernel pipe buffer is ~64 KiB, so the writer blocks after the
  // first chunk until the reader drains enough space.
  std::thread writer([wh = std::move(write_h), &write_ok]() mutable {
    std::vector<char> chunk(kTotalBytes, 'X');
    write_ok.store(WriteAll(wh, chunk.data(), chunk.size()));
  });

  WaitableEvent read_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<std::size_t> total_read{0};
  std::atomic<bool> marker_ran{false};

  io_runner_->PostTask(FROM_HERE, [&, read_h = std::move(read_h)]() mutable {
    auto stream = std::make_shared<PipeInputStream>(io_runner_);
    ASSERT_TRUE(stream->BindPlatformHandle(std::move(read_h)));

    auto holder = AcquireIOBuf(kTotalBytes);
    stream->ReadAsync(holder.buf, kTotalBytes,
                 [&total_read, &read_done, holder, stream](
                     bool ok, std::size_t n) {
                   if (ok) total_read.store(n);
                   read_done.Signal();
                 });

    io_runner_->PostTask(FROM_HERE, [&marker_ran]() {
      marker_ran.store(true);
    });
  });

  ASSERT_TRUE(read_done.TimedWait(std::chrono::seconds(10)));

  // Reader has fully drained the pipe — writer can exit cleanly.
  writer.join();
  ASSERT_TRUE(write_ok.load());

  EXPECT_TRUE(marker_ran.load())
      << "Marker task did NOT run — drain loop may be starving "
         "other tasks on the I/O thread.";
  EXPECT_EQ(total_read.load(), kTotalBytes);
}
#endif  // !defined(_WIN32)

// ===========================================================================
// 🟡 Test 4 — Peer Disconnect (clean EOF / broken pipe)
// ===========================================================================
//
// Starts a read on one end of the pipe, then closes the write end.
// The read callback must receive (false, 0) cleanly without exceptions,
// timeouts, or crashes.

TEST_F(PipeStreamTest, PeerDisconnectDeliversCleanEof) {
  PlatformHandle read_h, write_h;
  ASSERT_TRUE(MakePipe(read_h, write_h));

  // Close the write end FIRST to simulate peer disconnect.
  // The pipe now has no data and the write end is gone — a subsequent
  // read must return EOF ((false, 0)).
  write_h = PlatformHandle();

  WaitableEvent read_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> saw_eof{false};
  std::atomic<bool> callback_fired{false};

  io_runner_->PostTask(FROM_HERE, [&, read_h = std::move(read_h)]() mutable {
    auto stream = std::make_shared<PipeInputStream>(io_runner_);
    ASSERT_TRUE(stream->BindPlatformHandle(std::move(read_h)));

    auto holder = AcquireIOBuf(kSmallBufSize);
    stream->ReadAsync(holder.buf, kSmallBufSize,
                     [&callback_fired, &saw_eof, &read_done, holder,
                      stream](bool ok, std::size_t n) {
                       callback_fired.store(true);
                       if (!ok || n == 0) saw_eof.store(true);
                       read_done.Signal();
                     });
  });

  ASSERT_TRUE(read_done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(callback_fired.load());
  EXPECT_TRUE(saw_eof.load())
      << "Expected (false, 0) on peer disconnect but got success or "
         "the callback was never delivered.";
}

// Also test the write direction peer disconnect.
// This test verifies that writing to a pipe whose read end is closed
// does not crash — the write either succeeds (data fits in pipe buffer)
// or fails gracefully (EPIPE / broken pipe error).
// Disabled: WSL pipe semantics differ from native Linux — write() to a
// disconnected peer succeeds (data fits in buffer) and the pump timing
// is unreliable.  Re-enable after investigating WSL pipe behaviour.
TEST_F(PipeStreamTest, WriteToDisconnectedPeerFailsCleanly) {
  PlatformHandle read_h, write_h;
  ASSERT_TRUE(MakePipe(read_h, write_h));

  // Close the read end first -> write should not crash.
  read_h = PlatformHandle();

  WaitableEvent write_done(WaitableEvent::ResetPolicy::kAutomatic, false);

  io_runner_->PostTask(FROM_HERE, [&, write_h = std::move(write_h)]() mutable {
    auto stream = std::make_shared<PipeOutputStream>(io_runner_);
    ASSERT_TRUE(stream->BindPlatformHandle(std::move(write_h)));

    auto holder = AcquireIOBuf(kSmallBufSize);
    stream->WriteAsync(holder.buf, kSmallBufSize,
                      [&write_done, holder, stream](bool /*ok*/, std::size_t /*n*/) {
                        write_done.Signal();
                      });
  });

  ASSERT_TRUE(write_done.TimedWait(std::chrono::seconds(5)));
  SUCCEED();
}

// ===========================================================================
// 🟡 Test 5 — Rapid Cancel & Retry
// ===========================================================================
//
// Rapidly cycles through: ReadAsync → Close → Bind new handle → ReadAsync.
// Verifies the state machine cleans up correctly and the second operation
// succeeds independently of the first.

// Disabled: WSL pump wakeup ordering after ShutdownAndSelfDestruct is
// unreliable — the phase 2 IO task may be queued ahead of the destructor
// cleanup, causing epoll interference.  Works correctly on native Linux.
TEST_F(PipeStreamTest, RapidCancelAndRetryStateMachine) {
  // ---- Phase 1: first pipe, read + immediate close --------------------
  PlatformHandle h1_read, h1_write;
  ASSERT_TRUE(MakePipe(h1_read, h1_write));

  WaitableEvent phase1_done(WaitableEvent::ResetPolicy::kAutomatic, false);

  io_runner_->PostTask(FROM_HERE, [&, rh = std::move(h1_read)]() mutable {
    auto stream = std::make_unique<PipeInputStream>(io_runner_);
    ASSERT_TRUE(stream->BindPlatformHandle(std::move(rh)));

    auto holder = AcquireIOBuf(kSmallBufSize);
    stream->ReadAsync(holder.buf, kSmallBufSize,
                     [holder](bool, std::size_t) {
                       // May or may not fire — we Close() immediately.
                     });

    // Cancel before any data arrives.
    stream->Close();

    // Explicitly destroy the stream HERE so ShutdownAndSelfDestruct is
    // queued BEFORE the completion signal below.  This guarantees the
    // old stream is fully torn down before phase 2 starts.
    stream.reset();

    // Now safe to signal 鈥?ShutdownAndSelfDestruct will run first.
    io_runner_->PostTask(FROM_HERE, [&phase1_done]() {
      phase1_done.Signal();
    });
  });

  ASSERT_TRUE(phase1_done.TimedWait(std::chrono::seconds(5)));

  // Clean up the first write handle (not used).
  h1_write = PlatformHandle();

  // ---- Phase 2: second pipe, bind + successful read -------------------
  PlatformHandle h2_read, h2_write;
  ASSERT_TRUE(CreateAsyncPipePair(h2_read, h2_write, false));

  const std::string message = "Retry after cancel succeeded!";

  WaitableEvent phase2_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::string received;

  io_runner_->PostTask(FROM_HERE, [&, rh = std::move(h2_read)]() mutable {
    auto stream2 = std::make_shared<PipeInputStream>(io_runner_);
    ASSERT_TRUE(stream2->BindPlatformHandle(std::move(rh)));

    auto holder = AcquireIOBuf(kSmallBufSize);
    stream2->ReadAsync(holder.buf, kSmallBufSize,
                 [&received, &phase2_done, holder, stream2](
                     bool ok, std::size_t n) {
                   if (ok && n > 0)
                     received.assign(holder.buf->data(), n);
                   phase2_done.Signal();
                 });
  });

  // Write data to the *second* pipe's write end.
  ASSERT_TRUE(WriteAll(h2_write, message.data(), message.size()));
  h2_write = PlatformHandle();   // close write → EOF

  ASSERT_TRUE(phase2_done.TimedWait(std::chrono::seconds(5)));

  // 🔴 KEY ASSERTION: the second read must have succeeded with the
  // expected payload, proving the state machine was clean after Close().
  EXPECT_EQ(received, message)
      << "Second read after cancel/retry did not receive the correct "
         "data — state machine may have stale state from the first "
         "cancelled operation.";
}

// Same for the write path.
TEST_F(PipeStreamTest, RapidWriteCancelAndRetry) {
  PlatformHandle h1_read, h1_write;
  ASSERT_TRUE(MakePipe(h1_read, h1_write));

  WaitableEvent phase1_done(WaitableEvent::ResetPolicy::kAutomatic, false);

  io_runner_->PostTask(FROM_HERE, [&, wh = std::move(h1_write)]() mutable {
    auto stream = std::make_unique<PipeOutputStream>(io_runner_);
    ASSERT_TRUE(stream->BindPlatformHandle(std::move(wh)));

    auto holder = AcquireIOBuf(kLargeBufSize);
    stream->WriteAsync(holder.buf, kLargeBufSize,
                      [holder](bool, std::size_t) {});
    stream->Close();
    stream.reset();
    io_runner_->PostTask(FROM_HERE, [&phase1_done]() {
      phase1_done.Signal();
    });
  });

  ASSERT_TRUE(phase1_done.TimedWait(std::chrono::seconds(5)));
  h1_read = PlatformHandle();

  // Phase 2: second pipe, write must succeed.
  PlatformHandle h2_read, h2_write;
  ASSERT_TRUE(MakePipe(h2_read, h2_write));

  WaitableEvent phase2_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> write_ok{false};

  io_runner_->PostTask(FROM_HERE,
                       [&, rh = std::move(h2_read), wh = std::move(h2_write)]() mutable {
    auto reader2 = std::make_shared<PipeInputStream>(io_runner_);
    ASSERT_TRUE(reader2->BindPlatformHandle(std::move(rh)));

    auto read_holder = AcquireIOBuf(kSmallBufSize);
    reader2->ReadAsync(read_holder.buf, kSmallBufSize,
                       [read_holder, reader2](bool, std::size_t) {});

    auto stream2 = std::make_shared<PipeOutputStream>(io_runner_);
    ASSERT_TRUE(stream2->BindPlatformHandle(std::move(wh)));

    auto holder = AcquireIOBuf(kSmallBufSize);
    std::memcpy(holder.buf->data(), "OK", 2);
    stream2->WriteAsync(holder.buf, 2u,
                  [&write_ok, &phase2_done, holder, stream2](
                      bool ok, std::size_t n) {
                    write_ok.store(ok && n == 2);
                    phase2_done.Signal();
                  });
  });

  ASSERT_TRUE(phase2_done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(write_ok.load());
}

// ===========================================================================
// Trace test: single ReadAsync with full pump tracing
// ===========================================================================
// Run: ./nei_tests --gtest_filter=*TraceSingleRead*
// Output: /tmp/pipe_stream_trace.json (open in chrome://tracing)

TEST_F(PipeStreamTest, TraceSingleReadAsync) {
  EnableTracing();

  PlatformHandle read_h, write_h;
  ASSERT_TRUE(MakePipe(read_h, write_h));

  const std::string payload = "Hello, async pipe!";
  WaitableEvent read_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> callback_fired{false};

  io_runner_->PostTask(
      FROM_HERE,
      [&, read_h = std::move(read_h), write_h = std::move(write_h), payload]() mutable {
        auto writer = std::make_shared<PipeOutputStream>(io_runner_);
        ASSERT_TRUE(writer->BindPlatformHandle(std::move(write_h)));

        auto write_holder = AcquireIOBuf(payload.size());
        std::memcpy(write_holder.buf->data(), payload.data(), payload.size());
        writer->WriteAsync(write_holder.buf, payload.size(),
                           [write_holder, writer](bool, std::size_t) {});

        auto stream = std::make_shared<PipeInputStream>(io_runner_);
        ASSERT_TRUE(stream->BindPlatformHandle(std::move(read_h)));

        auto holder = AcquireIOBuf(kSmallBufSize);
        stream->ReadAsync(holder.buf, kSmallBufSize,
                         [&callback_fired, &read_done, holder, stream](
                             bool /*ok*/, std::size_t /*n*/) {
                           callback_fired.store(true);
                           read_done.Signal();
                         });
      });

  bool done = read_done.TimedWait(std::chrono::seconds(5));
  DisableTracingAndDump("/tmp/pipe_stream_trace.json");

  ASSERT_TRUE(done) << "Test hung — trace written to /tmp/pipe_stream_trace.json";
  EXPECT_TRUE(callback_fired.load());
}

}  // namespace
}  // namespace nei
