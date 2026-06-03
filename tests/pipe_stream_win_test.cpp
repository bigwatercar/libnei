#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

#include <neixx/common/location.h>
#include <internal/pipe_stream_factory_internal.h>
#include <neixx/io/io_buffer.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/threading/thread.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace nei {
namespace {

std::string BuildUniquePipeName() {
  const DWORD pid = GetCurrentProcessId();
  const unsigned long long tick = GetTickCount64();
  return "\\\\.\\pipe\\nei_pipe_stream_win_test_" + std::to_string(pid) +
         "_" + std::to_string(tick);
}

// ---------------------------------------------------------------------------
// PullReadLoop state for tests
// ---------------------------------------------------------------------------
constexpr std::size_t kReadBufSize = 4096;

struct PullState {
  AsyncInputStream* stream = nullptr;
  std::string received;
  bool saw_eof = false;
  WaitableEvent* done = nullptr;
};

static void IssuePull(const std::shared_ptr<PullState>& state);

static void IssuePull(const std::shared_ptr<PullState>& state) {
  scoped_refptr<IOBufferWithSize> sized_buf =
      IOBufferPool::GetInstance().AcquireBuffer(kReadBufSize);
  scoped_refptr<IOBuffer> buf(sized_buf.get());
  state->stream->ReadAsync(
      buf, kReadBufSize,
      [state, buf](bool ok, std::size_t bytes) mutable {
        if (!ok || bytes == 0) {
          state->saw_eof = true;
          if (state->done) state->done->Signal();
          return;
        }
        state->received.append(buf->data(), bytes);
        buf.reset();
        IssuePull(state);
      });
}

// ---------------------------------------------------------------------------
// WinPipeInputStreamTest.MessageModeLargeReadHandlesMoreData
//
// This test verifies that a single large message (> read buffer size) that
// arrives as ERROR_MORE_DATA is fully reassembled across multiple pull-reads.
// ---------------------------------------------------------------------------
TEST(WinPipeInputStreamTest, MessageModeLargeReadHandlesMoreData) {
  const std::string pipe_name = BuildUniquePipeName();

  HANDLE server = CreateNamedPipeA(
      pipe_name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 0, 0, 0,
      nullptr);
  ASSERT_NE(server, INVALID_HANDLE_VALUE);

  HANDLE client = CreateFileA(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(client, INVALID_HANDLE_VALUE);

  const BOOL connected = ConnectNamedPipe(server, nullptr);
  const DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
  ASSERT_TRUE(connected || connect_error == ERROR_PIPE_CONNECTED);

  Thread io_thread("pipe-stream-win-test-io");
  Thread::Options options;
  options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(options));

  const scoped_refptr<TaskRunner> runner = io_thread.GetTaskRunner();
  ASSERT_TRUE(runner);

  WaitableEvent setup_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_done(WaitableEvent::ResetPolicy::kAutomatic, false);

  auto state = std::make_shared<PullState>();
  state->done = &read_done;

  // The stream is created on the IO thread and the pull loop is started there.
  std::unique_ptr<AsyncInputStream> stream_holder;

  runner->PostTask(FROM_HERE, [&, server]() {
    MessagePumpForIO* pump = MessagePumpForIO::Current();
    if (pump == nullptr) {
      setup_done.Signal();
      read_done.Signal();
      return;
    }

    stream_holder = CreatePipeInputStream(
        pump, reinterpret_cast<NativeIOHandle>(server));
    state->stream = stream_holder.get();

    IssuePull(state);
    setup_done.Signal();
  });

  setup_done.Wait();

  // Write a single message larger than read_buffer_ (4096) to trigger
  // ERROR_MORE_DATA semantics for message-mode named pipes.
  const std::string payload(10000, 'X');
  DWORD written = 0;
  const BOOL write_ok = WriteFile(client, payload.data(),
                                  static_cast<DWORD>(payload.size()), &written,
                                  nullptr);
  ASSERT_TRUE(write_ok);
  ASSERT_EQ(written, payload.size());

  ASSERT_TRUE(FlushFileBuffers(client));
  ASSERT_TRUE(CloseHandle(client));
  client = INVALID_HANDLE_VALUE;

  ASSERT_TRUE(read_done.TimedWait(std::chrono::seconds(5)));

  // Tear down: close the stream from the IO thread.
  runner->PostTask(FROM_HERE, [&]() {
    if (stream_holder) {
      stream_holder->Close();
      stream_holder.reset();
    }
  });
  io_thread.Stop();

  EXPECT_TRUE(state->saw_eof);
  EXPECT_EQ(state->received.size(), payload.size());
  EXPECT_EQ(state->received, payload);
}

}  // namespace
}  // namespace nei

#endif  // defined(_WIN32)