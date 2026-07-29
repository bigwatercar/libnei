#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <memory>
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
#include <neixx/threading/thread.h>

namespace {

class DemoPipeState {
 public:
  std::mutex lock;
  std::string payload;
  bool input_closed = false;
  bool output_closed = false;
};

class DemoAsyncInputStream final : public nei::AsyncInputStream {
 public:
  DemoAsyncInputStream(nei::scoped_refptr<nei::TaskRunner> io_runner,
                       std::shared_ptr<DemoPipeState> state)
      : io_runner_(std::move(io_runner)), state_(std::move(state)) {}

  void ReadAsync(nei::scoped_refptr<nei::IOBuffer> buf,
                 std::size_t buf_len,
                 IOReadCallback callback) override {
    if (!io_runner_ || !callback || !buf) {
      if (callback) {
        callback(false, 0u);
      }
      return;
    }

    io_runner_->PostTask(
        FROM_HERE,
        [state = state_, buf = std::move(buf), buf_len, callback = std::move(callback)]() mutable {
          std::size_t copied = 0;
          bool ok = false;
          {
            std::lock_guard<std::mutex> guard(state->lock);
            if (!state->input_closed && !state->payload.empty() && buf_len > 0) {
              copied = (std::min)(buf_len, state->payload.size());
              std::memcpy(buf->data(), state->payload.data(), copied);
              state->payload.erase(0, copied);
              ok = true;
            }
          }
          callback(ok, copied);
        });
  }

  void Close() override {
    std::lock_guard<std::mutex> guard(state_->lock);
    state_->input_closed = true;
  }

 private:
  nei::scoped_refptr<nei::TaskRunner> io_runner_;
  std::shared_ptr<DemoPipeState> state_;
};

class DemoAsyncOutputStream final : public nei::AsyncOutputStream {
 public:
  DemoAsyncOutputStream(nei::scoped_refptr<nei::TaskRunner> io_runner,
                        std::shared_ptr<DemoPipeState> state)
      : io_runner_(std::move(io_runner)), state_(std::move(state)) {}

  void WriteAsync(nei::scoped_refptr<nei::IOBuffer> buf,
                  std::size_t buf_len,
                  IOWriteCallback callback) override {
    if (!io_runner_ || !callback || !buf) {
      if (callback) {
        callback(false, 0u);
      }
      return;
    }

    io_runner_->PostTask(
        FROM_HERE,
        [state = state_, buf = std::move(buf), buf_len, callback = std::move(callback)]() mutable {
          bool ok = false;
          {
            std::lock_guard<std::mutex> guard(state->lock);
            if (!state->output_closed) {
              state->payload.append(reinterpret_cast<const char*>(buf->data()), buf_len);
              ok = true;
            }
          }
          callback(ok, ok ? buf_len : 0u);
        });
  }

  void Close() override {
    std::lock_guard<std::mutex> guard(state_->lock);
    state_->output_closed = true;
  }

 private:
  nei::scoped_refptr<nei::TaskRunner> io_runner_;
  std::shared_ptr<DemoPipeState> state_;
};

struct ScenarioState {
  explicit ScenarioState(nei::scoped_refptr<nei::TaskRunner> runner,
                         nei::WaitableEvent* done_event,
                         std::atomic<bool>* result)
      : io_runner(std::move(runner)), done(done_event), ok(result) {
    pipe = std::make_shared<DemoPipeState>();
    input = std::make_unique<DemoAsyncInputStream>(io_runner, pipe);
    output = std::make_unique<DemoAsyncOutputStream>(io_runner, pipe);
    reader = std::make_unique<nei::StreamReader>(input.get());
    writer = std::make_unique<nei::StreamWriter>(output.get());
  }

  void Finish(bool value) {
    ok->store(value, std::memory_order_release);
    done->Signal();
  }

  nei::scoped_refptr<nei::TaskRunner> io_runner;
  std::shared_ptr<DemoPipeState> pipe;
  std::unique_ptr<DemoAsyncInputStream> input;
  std::unique_ptr<DemoAsyncOutputStream> output;
  std::unique_ptr<nei::StreamReader> reader;
  std::unique_ptr<nei::StreamWriter> writer;
  nei::WaitableEvent* done = nullptr;
  std::atomic<bool>* ok = nullptr;
};

void RunOneScenarioAsync(nei::scoped_refptr<nei::TaskRunner> io_runner,
                         nei::WaitableEvent* done,
                         std::atomic<bool>* ok) {
  auto state = std::make_shared<ScenarioState>(std::move(io_runner), done, ok);

  // The whole demo is a non-blocking callback chain on the logic sequence.
  // This keeps the logic runner pump alive so StreamReader/StreamWriter can
  // trampoline physical IO completions back to this sequence and pass WeakPtr
  // guards correctly.
  state->writer->WriteString("hello stream wrapper",
                             [state](bool write_ok, std::size_t written) {
    if (!write_ok || written == 0) {
      state->Finish(false);
      return;
    }

    state->reader->ReadString(written, [state](bool read_ok, std::string&& text) {
      if (!read_ok || text != "hello stream wrapper") {
        state->Finish(false);
        return;
      }

      std::cout << "[demo] ReadString -> " << text << std::endl;

      state->writer->WriteString("ABCDEF", [state](bool w2_ok, std::size_t w2) {
        if (!w2_ok || w2 != 6) {
          state->Finish(false);
          return;
        }

        state->reader->ReadBytes(w2,
                                 [state](bool b_ok,
                                         std::vector<std::uint8_t>&& data) {
          if (!b_ok) {
            state->Finish(false);
            return;
          }

          const std::string bytes_text(data.begin(), data.end());
          std::cout << "[demo] ReadBytes  -> " << bytes_text << std::endl;
          state->Finish(bytes_text == "ABCDEF");
        });
      });
    });
  });
}

}  // namespace

int main() {
  nei::Thread io_thread("demo-io-thread");
  nei::Thread::Options io_options;
  io_options.message_pump_type = nei::MessagePumpType::IO;
  if (!io_thread.StartWithOptions(io_options)) {
    std::cerr << "Failed to start IO thread" << std::endl;
    return 1;
  }

  nei::Thread logic_thread("demo-logic-thread");
  if (!logic_thread.Start()) {
    std::cerr << "Failed to start logic thread" << std::endl;
    io_thread.Stop();
    return 1;
  }

  const nei::scoped_refptr<nei::TaskRunner> io_runner = io_thread.GetTaskRunner();
  const nei::scoped_refptr<nei::TaskRunner> logic_runner = logic_thread.GetTaskRunner();

  if (!io_runner || !logic_runner) {
    std::cerr << "Failed to get task runners" << std::endl;
    logic_thread.Stop();
    io_thread.Stop();
    return 1;
  }

  nei::WaitableEvent done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};

  logic_runner->PostTask(FROM_HERE, [&]() {
    RunOneScenarioAsync(io_runner, &done, &ok);
  });

  if (!done.TimedWait(std::chrono::seconds(10))) {
    std::cerr << "Demo timed out" << std::endl;
    logic_thread.Stop();
    io_thread.Stop();
    return 1;
  }

  logic_thread.Stop();
  io_thread.Stop();

  if (!ok.load(std::memory_order_acquire)) {
    std::cerr << "stream_reader_writer_demo FAILED" << std::endl;
    return 1;
  }

  std::cout << "stream_reader_writer_demo PASSED" << std::endl;
  return 0;
}
