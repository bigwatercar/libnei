#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/io/async_stream.h>
#include <neixx/io/io_buffer.h>
#include <neixx/io/stream_reader.h>
#include <neixx/io/stream_writer.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/platform_thread.h>
#include <neixx/threading/thread.h>

namespace nei {
namespace {

class FakeAsyncInputStream final : public AsyncInputStream {
 public:
  struct State {
    scoped_refptr<TaskRunner> io_runner;
    scoped_refptr<TaskRunner> logic_runner;
    std::string payload;
    bool succeed = true;
    std::chrono::milliseconds delay{0};
    WaitableEvent* invoked_event = nullptr;
    WaitableEvent* logic_barrier_event = nullptr;
  };

  FakeAsyncInputStream(scoped_refptr<TaskRunner> io_runner,
                       scoped_refptr<TaskRunner> logic_runner,
                       std::string payload,
                       bool succeed,
                       std::chrono::milliseconds delay,
                       WaitableEvent* invoked_event,
                       WaitableEvent* logic_barrier_event) {
    state_ = std::make_shared<State>();
    state_->io_runner = std::move(io_runner);
    state_->logic_runner = std::move(logic_runner);
    state_->payload = std::move(payload);
    state_->succeed = succeed;
    state_->delay = delay;
    state_->invoked_event = invoked_event;
    state_->logic_barrier_event = logic_barrier_event;
  }

  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t buf_len,
                 IOReadCallback callback) override {
    auto state = state_;
    state->io_runner->PostDelayedTask(
        FROM_HERE,
        [state, buf = std::move(buf), buf_len, callback = std::move(callback)]() mutable {
          std::size_t copied = 0;
          bool ok = false;
          if (state->succeed && !state->payload.empty() && buf_len > 0) {
            copied = (std::min)(buf_len, state->payload.size());
            std::memcpy(buf->data(), state->payload.data(), copied);
            ok = true;
          }
          callback(ok, copied);
          if (state->invoked_event != nullptr) {
            state->invoked_event->Signal();
          }
          if (state->logic_runner && state->logic_barrier_event != nullptr) {
            state->logic_runner->PostTask(FROM_HERE,
                                    [barrier = state->logic_barrier_event]() {
                                      barrier->Signal();
                                    });
          }
        },
        TimeDelta::FromMilliseconds(state->delay.count()));
  }

  void Close() override {}

 private:
  std::shared_ptr<State> state_;
};

class FakeAsyncOutputStream final : public AsyncOutputStream {
 public:
  struct State {
    scoped_refptr<TaskRunner> io_runner;
    scoped_refptr<TaskRunner> logic_runner;
    bool succeed = true;
    std::chrono::milliseconds delay{0};
    WaitableEvent* invoked_event = nullptr;
    WaitableEvent* logic_barrier_event = nullptr;
    mutable std::mutex lock;
    std::string written_payload;
  };

  FakeAsyncOutputStream(scoped_refptr<TaskRunner> io_runner,
                        scoped_refptr<TaskRunner> logic_runner,
                        bool succeed,
                        std::chrono::milliseconds delay,
                        WaitableEvent* invoked_event,
                        WaitableEvent* logic_barrier_event) {
    state_ = std::make_shared<State>();
    state_->io_runner = std::move(io_runner);
    state_->logic_runner = std::move(logic_runner);
    state_->succeed = succeed;
    state_->delay = delay;
    state_->invoked_event = invoked_event;
    state_->logic_barrier_event = logic_barrier_event;
  }

  void WriteAsync(scoped_refptr<IOBuffer> buf,
                  std::size_t buf_len,
                  IOWriteCallback callback) override {
    auto state = state_;
    state->io_runner->PostDelayedTask(
        FROM_HERE,
        [state, buf = std::move(buf), buf_len, callback = std::move(callback)]() mutable {
          if (state->succeed) {
            std::lock_guard<std::mutex> guard(state->lock);
            state->written_payload.append(reinterpret_cast<const char*>(buf->data()), buf_len);
            callback(true, buf_len);
          } else {
            callback(false, 0u);
          }
          if (state->invoked_event != nullptr) {
            state->invoked_event->Signal();
          }
          if (state->logic_runner && state->logic_barrier_event != nullptr) {
            state->logic_runner->PostTask(FROM_HERE,
                                    [barrier = state->logic_barrier_event]() {
                                      barrier->Signal();
                                    });
          }
        },
        TimeDelta::FromMilliseconds(state->delay.count()));
  }

  void Close() override {}

  std::string written_payload() const {
    std::lock_guard<std::mutex> guard(state_->lock);
    return state_->written_payload;
  }

 private:
  std::shared_ptr<State> state_;
};

TEST(StreamReaderWriterTest, ReadStringReturnsPayloadOnLogicSequence) {
  Thread io_thread("srw-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread logic_thread("srw-logic");
  ASSERT_TRUE(logic_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> logic_runner = logic_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(logic_runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};

  const bool posted = logic_runner->PostTask(FROM_HERE, [&]() {
    struct ReadStringState {
      FakeAsyncInputStream input;
      StreamReader reader;
      explicit ReadStringState(scoped_refptr<TaskRunner> io_runner,
                               scoped_refptr<TaskRunner> logic_runner)
          : input(std::move(io_runner), std::move(logic_runner), "hello-reader",
                  true, std::chrono::milliseconds(1), nullptr, nullptr),
            reader(&input) {}
    };

    auto state = std::make_shared<ReadStringState>(io_runner, logic_runner);
    const auto logic_tid = PlatformThread::CurrentId();

    state->reader.ReadString(64, [state, &ok, &done, logic_tid](bool success,
                                                                std::string&& data) {
      ok.store(success && data == "hello-reader" &&
                   PlatformThread::CurrentId() == logic_tid,
               std::memory_order_release);
      done.Signal();
    });
  });
  ASSERT_TRUE(posted);

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(ok.load(std::memory_order_acquire));

  logic_thread.Stop();
  io_thread.Stop();
}

TEST(StreamReaderWriterTest, ReadBytesReturnsVectorPayload) {
  Thread io_thread("srw-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread logic_thread("srw-logic");
  ASSERT_TRUE(logic_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> logic_runner = logic_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(logic_runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};

  const bool posted = logic_runner->PostTask(FROM_HERE, [&]() {
    struct ReadBytesState {
      FakeAsyncInputStream input;
      StreamReader reader;
      explicit ReadBytesState(scoped_refptr<TaskRunner> io_runner,
                              scoped_refptr<TaskRunner> logic_runner)
          : input(std::move(io_runner), std::move(logic_runner), "ABCDEF", true,
                  std::chrono::milliseconds(1), nullptr, nullptr),
            reader(&input) {}
    };

    auto state = std::make_shared<ReadBytesState>(io_runner, logic_runner);

    state->reader.ReadBytes(6, [state, &ok, &done](bool success,
                                                   std::vector<std::uint8_t>&& data) {
      const std::string text(data.begin(), data.end());
      ok.store(success && text == "ABCDEF", std::memory_order_release);
      done.Signal();
    });
  });
  ASSERT_TRUE(posted);

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(ok.load(std::memory_order_acquire));

  logic_thread.Stop();
  io_thread.Stop();
}

TEST(StreamReaderWriterTest, WriteStringCopiesPayloadAndReportsBytes) {
  Thread io_thread("srw-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread logic_thread("srw-logic");
  ASSERT_TRUE(logic_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> logic_runner = logic_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(logic_runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};

  const bool posted = logic_runner->PostTask(FROM_HERE, [&]() {
    struct WriteState {
      FakeAsyncOutputStream output;
      StreamWriter writer;
      explicit WriteState(scoped_refptr<TaskRunner> io_runner,
                          scoped_refptr<TaskRunner> logic_runner)
          : output(std::move(io_runner), std::move(logic_runner), true,
                   std::chrono::milliseconds(1), nullptr, nullptr),
            writer(&output) {}
    };

    auto state = std::make_shared<WriteState>(io_runner, logic_runner);

    state->writer.WriteString("writer-payload",
                              [state, &ok, &done](bool success,
                                                  std::size_t bytes_written) {
      ok.store(success && bytes_written == 14 &&
                   state->output.written_payload() == "writer-payload",
               std::memory_order_release);
      done.Signal();
    });
  });
  ASSERT_TRUE(posted);

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(ok.load(std::memory_order_acquire));

  logic_thread.Stop();
  io_thread.Stop();
}

TEST(StreamReaderWriterTest, StreamReaderDestructionDropsLateCallback) {
  Thread io_thread("srw-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread logic_thread("srw-logic");
  ASSERT_TRUE(logic_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> logic_runner = logic_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(logic_runner);

  WaitableEvent io_invoked(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent logic_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> callback_count{0};

  const bool posted = logic_runner->PostTask(FROM_HERE, [&]() {
    FakeAsyncInputStream input(io_runner, logic_runner, "drop-me", true,
                               std::chrono::milliseconds(10), &io_invoked,
                               &logic_barrier);
    auto reader = std::make_unique<StreamReader>(&input);
    reader->ReadString(32, [&](bool, std::string&&) {
      callback_count.fetch_add(1, std::memory_order_acq_rel);
    });
    reader.reset();
  });
  ASSERT_TRUE(posted);

  ASSERT_TRUE(io_invoked.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(logic_barrier.TimedWait(std::chrono::seconds(5)));
  EXPECT_EQ(callback_count.load(std::memory_order_acquire), 0);

  logic_thread.Stop();
  io_thread.Stop();
}

TEST(StreamReaderWriterTest, StreamWriterDestructionDropsLateCallback) {
  Thread io_thread("srw-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread logic_thread("srw-logic");
  ASSERT_TRUE(logic_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> logic_runner = logic_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(logic_runner);

  WaitableEvent io_invoked(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent logic_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<int> callback_count{0};

  const bool posted = logic_runner->PostTask(FROM_HERE, [&]() {
    FakeAsyncOutputStream output(io_runner, logic_runner, true,
                                 std::chrono::milliseconds(10), &io_invoked,
                                 &logic_barrier);
    auto writer = std::make_unique<StreamWriter>(&output);
    writer->WriteString("drop-callback", [&](bool, std::size_t) {
      callback_count.fetch_add(1, std::memory_order_acq_rel);
    });
    writer.reset();
  });
  ASSERT_TRUE(posted);

  ASSERT_TRUE(io_invoked.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(logic_barrier.TimedWait(std::chrono::seconds(5)));
  EXPECT_EQ(callback_count.load(std::memory_order_acquire), 0);

  logic_thread.Stop();
  io_thread.Stop();
}

TEST(StreamReaderWriterTest, ReadStringReturnsFailureWhenStreamFails) {
  Thread io_thread("srw-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread logic_thread("srw-logic");
  ASSERT_TRUE(logic_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> logic_runner = logic_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(logic_runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};

  const bool posted = logic_runner->PostTask(FROM_HERE, [&]() {
    struct ReadStringFailState {
      FakeAsyncInputStream input;
      StreamReader reader;
      explicit ReadStringFailState(scoped_refptr<TaskRunner> io_runner,
                                   scoped_refptr<TaskRunner> logic_runner)
          : input(std::move(io_runner), std::move(logic_runner), "ignored",
                  false, std::chrono::milliseconds(1), nullptr, nullptr),
            reader(&input) {}
    };

    auto state = std::make_shared<ReadStringFailState>(io_runner, logic_runner);

    state->reader.ReadString(64, [state, &ok, &done](bool success,
                                                     std::string&& data) {
      ok.store(!success && data.empty(), std::memory_order_release);
      done.Signal();
    });
  });
  ASSERT_TRUE(posted);

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(ok.load(std::memory_order_acquire));

  logic_thread.Stop();
  io_thread.Stop();
}

TEST(StreamReaderWriterTest, ReadBytesReturnsFailureWhenStreamFails) {
  Thread io_thread("srw-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread logic_thread("srw-logic");
  ASSERT_TRUE(logic_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> logic_runner = logic_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(logic_runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};

  const bool posted = logic_runner->PostTask(FROM_HERE, [&]() {
    struct ReadBytesFailState {
      FakeAsyncInputStream input;
      StreamReader reader;
      explicit ReadBytesFailState(scoped_refptr<TaskRunner> io_runner,
                                  scoped_refptr<TaskRunner> logic_runner)
          : input(std::move(io_runner), std::move(logic_runner), "ignored",
                  false, std::chrono::milliseconds(1), nullptr, nullptr),
            reader(&input) {}
    };

    auto state = std::make_shared<ReadBytesFailState>(io_runner, logic_runner);

    state->reader.ReadBytes(32, [state, &ok, &done](bool success,
                                                    std::vector<std::uint8_t>&& data) {
      ok.store(!success && data.empty(), std::memory_order_release);
      done.Signal();
    });
  });
  ASSERT_TRUE(posted);

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(ok.load(std::memory_order_acquire));

  logic_thread.Stop();
  io_thread.Stop();
}

TEST(StreamReaderWriterTest, WriteStringReturnsFailureWhenStreamFails) {
  Thread io_thread("srw-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread logic_thread("srw-logic");
  ASSERT_TRUE(logic_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> logic_runner = logic_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(logic_runner);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};

  const bool posted = logic_runner->PostTask(FROM_HERE, [&]() {
    struct WriteFailState {
      FakeAsyncOutputStream output;
      StreamWriter writer;
      explicit WriteFailState(scoped_refptr<TaskRunner> io_runner,
                              scoped_refptr<TaskRunner> logic_runner)
          : output(std::move(io_runner), std::move(logic_runner), false,
                   std::chrono::milliseconds(1), nullptr, nullptr),
            writer(&output) {}
    };

    auto state = std::make_shared<WriteFailState>(io_runner, logic_runner);

    state->writer.WriteString("attempt",
                              [state, &ok, &done](bool success,
                                                  std::size_t bytes_written) {
      ok.store(!success && bytes_written == 0, std::memory_order_release);
      done.Signal();
    });
  });
  ASSERT_TRUE(posted);

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(ok.load(std::memory_order_acquire));

  logic_thread.Stop();
  io_thread.Stop();
}

}  // namespace
}  // namespace nei
