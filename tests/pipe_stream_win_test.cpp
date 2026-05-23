#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/io/pipe_stream_factory.h>
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

TEST(WinPipeInputStreamTest, MessageModeLargeReadHandlesMoreData) {
  const std::string pipe_name = BuildUniquePipeName();

  HANDLE server = CreateNamedPipeA(
      pipe_name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 0, 0, 0, nullptr);
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

  struct SharedState {
    std::mutex lock;
    std::string received;
    bool saw_eof = false;
    std::unique_ptr<AsyncInputStream> stream;
  };
  auto state = std::make_shared<SharedState>();

  runner->PostTask(FROM_HERE, [state, server, &setup_done, &read_done]() {
    MessagePumpForIO* pump = MessagePumpForIO::Current();
    if (pump == nullptr) {
      setup_done.Signal();
      read_done.Signal();
      return;
    }

    state->stream = CreatePipeInputStream(
        pump, reinterpret_cast<NativeIOHandle>(server));
    state->stream->ReadAsync([state, &read_done](std::vector<std::uint8_t>&& data) {
      std::lock_guard<std::mutex> lock(state->lock);
      if (data.empty()) {
        state->saw_eof = true;
        read_done.Signal();
        return;
      }
      state->received.append(reinterpret_cast<const char*>(data.data()), data.size());
    });

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

  runner->PostTask(FROM_HERE, [state]() {
    if (state->stream) {
      state->stream->Close();
      state->stream.reset();
    }
  });
  io_thread.Stop();

  {
    std::lock_guard<std::mutex> lock(state->lock);
    EXPECT_TRUE(state->saw_eof);
    EXPECT_EQ(state->received.size(), payload.size());
    EXPECT_EQ(state->received, payload);
  }
}

}  // namespace
}  // namespace nei

#endif  // defined(_WIN32)
