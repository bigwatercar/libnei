#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include <neixx/common/location.h>
#include <neixx/io/async_stream.h>
#include <neixx/io/io_buffer.h>
#include <neixx/ipc/message_channel.h>
#include <neixx/ipc/rpc_endpoint.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace nei {
namespace {

// =============================================================================
// LoopbackDuplexStream  --  in-memory duplex byte-stream pair
// =============================================================================
//
// Two instances share a DuplexState: end A reads from |a_to_b| and writes
// into |b_to_a|; end B is the mirror.  Reads complete with queued bytes
// (bounded by |max_read_chunk| so tests can exercise frame fragmentation) and
// park the callback when the queue is empty -- the peer's next write wakes
// it.  Close() on one end marks the peer's read side as EOF.  All callbacks
// fire on the caller's thread (the single IO thread tests use), so delivery
// is deterministic.
//
// NOTE: tests construct messages with exact-size IOBufferWithSize -- the
// pool's AcquireBuffer() returns bucket-normalized sizes whose size() does
// not equal the requested payload length.

struct DuplexState {
  std::mutex mutex;
  std::string a_to_b;
  std::string b_to_a;
  std::size_t max_read_chunk = 65536;
  bool a_eof = false; // End A's reads see EOF (end B closed).
  bool b_eof = false; // End B's reads see EOF (end A closed).

  struct Parked {
    scoped_refptr<IOBuffer> buf;
    std::size_t len = 0;
    AsyncInputStream::IOReadCallback cb;

    explicit operator bool() const {
      return cb != nullptr;
    }
  };

  Parked parked_a; // Read issued by end A, waiting for a_to_b bytes.
  Parked parked_b; // Read issued by end B, waiting for b_to_a bytes.
};

class LoopbackDuplexStream final : public AsyncInputStream, public AsyncOutputStream {
public:
  LoopbackDuplexStream(std::shared_ptr<DuplexState> state, bool is_a)
      : state_(std::move(state))
      , is_a_(is_a) {
  }

  // ---- AsyncInputStream ------------------------------------------------

  void ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOReadCallback callback) override {
    bool deliver = false;
    bool eof = false;
    std::string chunk;

    {
      std::lock_guard<std::mutex> guard(state_->mutex);
      std::string &mine = is_a_ ? state_->a_to_b : state_->b_to_a;
      const bool &eof_flag = is_a_ ? state_->a_eof : state_->b_eof;
      if (!mine.empty()) {
        const std::size_t n = std::min({buf_len, mine.size(), state_->max_read_chunk});
        chunk.assign(mine, 0, n);
        mine.erase(0, n);
        deliver = true;
      } else if (eof_flag) {
        deliver = true;
        eof = true;
      } else {
        DuplexState::Parked &slot = is_a_ ? state_->parked_a : state_->parked_b;
        slot = DuplexState::Parked{std::move(buf), buf_len, std::move(callback)};
      }
    }

    if (deliver) {
      if (eof) {
        callback(false, 0);
      } else {
        std::memcpy(buf->data(), chunk.data(), chunk.size());
        callback(true, chunk.size());
      }
    }
  }

  // ---- AsyncOutputStream ------------------------------------------------

  void WriteAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOWriteCallback callback) override {
    bool complete_peer = false;
    DuplexState::Parked parked;
    std::string chunk;

    {
      std::lock_guard<std::mutex> guard(state_->mutex);
      std::string &peer_queue = is_a_ ? state_->b_to_a : state_->a_to_b;
      peer_queue.append(reinterpret_cast<const char *>(buf->data()), buf_len);

      DuplexState::Parked &peer_parked = is_a_ ? state_->parked_b : state_->parked_a;
      if (peer_parked) {
        parked = std::move(peer_parked);
        peer_parked = DuplexState::Parked();
        const std::size_t n = std::min({parked.len, peer_queue.size(), state_->max_read_chunk});
        chunk.assign(peer_queue, 0, n);
        peer_queue.erase(0, n);
        complete_peer = true;
      }
    }

    if (complete_peer) {
      std::memcpy(parked.buf->data(), chunk.data(), chunk.size());
      parked.cb(true, chunk.size());
    }
    callback(true, buf_len);
  }

  // Marks the peer's read side as EOF (SHUT_WR semantics) and wakes any
  // parked peer read with an EOF result.
  void Close() override {
    DuplexState::Parked parked;
    {
      std::lock_guard<std::mutex> guard(state_->mutex);
      bool &peer_eof = is_a_ ? state_->b_eof : state_->a_eof;
      peer_eof = true;
      DuplexState::Parked &peer_parked = is_a_ ? state_->parked_b : state_->parked_a;
      if (peer_parked) {
        parked = std::move(peer_parked);
        peer_parked = DuplexState::Parked();
      }
    }
    if (parked) {
      parked.cb(false, 0);
    }
  }

private:
  std::shared_ptr<DuplexState> state_;
  bool is_a_;
};

// Creates an exact-size message payload (NOT pool-normalized).
MessageChannel::Message MakeMessage(std::string_view payload) {
  auto buf = scoped_refptr<IOBufferWithSize>(new IOBufferWithSize(payload.size()));
  if (!payload.empty()) {
    std::memcpy(buf->data(), payload.data(), payload.size());
  }
  return buf;
}

RpcEndpoint::MessageBuffer MakeRpcPayload(std::string_view payload) {
  return MakeMessage(payload);
}

// ---------------------------------------------------------------------------
// MessageChannel tests
// ---------------------------------------------------------------------------

TEST(IpcMessageChannelTest, RoundTripSingleMessage) {
  Thread thread("ipc-io");
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(thread.StartWithOptions(opts));
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();

  auto state = std::make_shared<DuplexState>();
  LoopbackDuplexStream stream_a(state, true);
  LoopbackDuplexStream stream_b(state, false);

  MessageChannel channel_a(runner, runner, &stream_a, &stream_a);
  MessageChannel channel_b(runner, runner, &stream_b, &stream_b);

  WaitableEvent received(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::string got;
  channel_b.StartReading(
      [&](MessageChannel::Message msg) {
        got.assign(reinterpret_cast<const char *>(msg->data()), msg->size());
        received.Signal();
      },
      []() {});

  channel_a.Send(MakeMessage("hello-ipc"));
  ASSERT_TRUE(received.TimedWait(std::chrono::seconds(5)));
  EXPECT_EQ(got, "hello-ipc");
}

TEST(IpcMessageChannelTest, MultipleMessagesPreserveOrder) {
  Thread thread("ipc-io");
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(thread.StartWithOptions(opts));
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();

  auto state = std::make_shared<DuplexState>();
  LoopbackDuplexStream stream_a(state, true);
  LoopbackDuplexStream stream_b(state, false);

  MessageChannel channel_a(runner, runner, &stream_a, &stream_a);
  MessageChannel channel_b(runner, runner, &stream_b, &stream_b);

  constexpr int kCount = 5;
  std::vector<std::string> received;
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  channel_b.StartReading(
      [&](MessageChannel::Message msg) {
        received.emplace_back(reinterpret_cast<const char *>(msg->data()), msg->size());
        if (received.size() == kCount) {
          done.Signal();
        }
      },
      []() {});

  for (int i = 0; i < kCount; ++i) {
    channel_a.Send(MakeMessage("msg-" + std::to_string(i)));
  }
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  ASSERT_EQ(received.size(), static_cast<std::size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(received[i], "msg-" + std::to_string(i));
  }
}

TEST(IpcMessageChannelTest, FrameFragmentedAcrossReads) {
  Thread thread("ipc-io");
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(thread.StartWithOptions(opts));
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();

  auto state = std::make_shared<DuplexState>();
  state->max_read_chunk = 3; // Splits the 8-byte header and the payload.
  LoopbackDuplexStream stream_a(state, true);
  LoopbackDuplexStream stream_b(state, false);

  MessageChannel channel_a(runner, runner, &stream_a, &stream_a);
  MessageChannel channel_b(runner, runner, &stream_b, &stream_b);

  WaitableEvent received(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::string got;
  channel_b.StartReading(
      [&](MessageChannel::Message msg) {
        got.assign(reinterpret_cast<const char *>(msg->data()), msg->size());
        received.Signal();
      },
      []() {});

  channel_a.Send(MakeMessage("fragmented-payload"));
  ASSERT_TRUE(received.TimedWait(std::chrono::seconds(5)));
  EXPECT_EQ(got, "fragmented-payload");
}

TEST(IpcMessageChannelTest, MultipleFramesInOneRead) {
  Thread thread("ipc-io");
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(thread.StartWithOptions(opts));
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();

  auto state = std::make_shared<DuplexState>();
  LoopbackDuplexStream stream_a(state, true);
  LoopbackDuplexStream stream_b(state, false);

  MessageChannel channel_a(runner, runner, &stream_a, &stream_a);
  MessageChannel channel_b(runner, runner, &stream_b, &stream_b);

  std::vector<std::string> received;
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  channel_b.StartReading(
      [&](MessageChannel::Message msg) {
        received.emplace_back(reinterpret_cast<const char *>(msg->data()), msg->size());
        if (received.size() == 2) {
          done.Signal();
        }
      },
      []() {});

  // Both frames land in the queue before B's first read drains it.
  channel_a.Send(MakeMessage("first"));
  channel_a.Send(MakeMessage("second"));
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  ASSERT_EQ(received.size(), 2u);
  EXPECT_EQ(received[0], "first");
  EXPECT_EQ(received[1], "second");
}

TEST(IpcMessageChannelTest, CorruptMagicTearsDownChannel) {
  Thread thread("ipc-io");
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(thread.StartWithOptions(opts));
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();

  auto state = std::make_shared<DuplexState>();
  LoopbackDuplexStream stream_a(state, true);
  LoopbackDuplexStream stream_b(state, false);

  MessageChannel channel_b(runner, runner, &stream_b, &stream_b);

  WaitableEvent errored(WaitableEvent::ResetPolicy::kAutomatic, false);
  channel_b.StartReading([](MessageChannel::Message) {}, [&]() { errored.Signal(); });

  // Hand-crafted frame: valid length, WRONG magic word.
  auto raw = scoped_refptr<IOBufferWithSize>(new IOBufferWithSize(8 + 5));
  const uint8_t bad_frame[8 + 5] = {5, 0, 0, 0, 0xDE, 0xAD, 0xBE, 0xEF, 'h', 'e', 'l', 'l', 'o'};
  std::memcpy(raw->data(), bad_frame, sizeof(bad_frame));
  stream_a.WriteAsync(raw, sizeof(bad_frame), [](bool, std::size_t) {});
  ASSERT_TRUE(errored.TimedWait(std::chrono::seconds(5)));
}

TEST(IpcMessageChannelTest, OversizedFrameTearsDownChannel) {
  Thread thread("ipc-io");
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(thread.StartWithOptions(opts));
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();

  auto state = std::make_shared<DuplexState>();
  LoopbackDuplexStream stream_a(state, true);
  LoopbackDuplexStream stream_b(state, false);

  MessageChannel channel_b(runner, runner, &stream_b, &stream_b);

  WaitableEvent errored(WaitableEvent::ResetPolicy::kAutomatic, false);
  channel_b.StartReading([](MessageChannel::Message) {}, [&]() { errored.Signal(); });

  // Frame header claiming 200 MiB (> kMaxMessageSize 128 MiB) with valid magic.
  auto raw = scoped_refptr<IOBufferWithSize>(new IOBufferWithSize(8));
  const uint8_t huge[8] = {0, 0, 128, 12, 0x58, 0x45, 0x49, 0x4E}; // 200 MiB LE + 'NEIX'
  std::memcpy(raw->data(), huge, sizeof(huge));
  stream_a.WriteAsync(raw, sizeof(huge), [](bool, std::size_t) {});
  ASSERT_TRUE(errored.TimedWait(std::chrono::seconds(5)));
}

TEST(IpcMessageChannelTest, CloseDrainsPendingWritesThenErrorsOnce) {
  Thread thread("ipc-io");
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(thread.StartWithOptions(opts));
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();

  auto state = std::make_shared<DuplexState>();
  LoopbackDuplexStream stream_a(state, true);
  LoopbackDuplexStream stream_b(state, false);

  MessageChannel channel_a(runner, runner, &stream_a, &stream_a);
  MessageChannel channel_b(runner, runner, &stream_b, &stream_b);

  std::atomic<int> received_count{0};
  std::atomic<int> a_error_count{0};
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  // Close() is a LOCAL graceful shutdown: the closing side's on_error fires
  // once its pending writes drain; the peer only stops receiving messages
  // (it gets no error notification).
  channel_a.StartReading([](MessageChannel::Message) {},
                         [&]() {
                           a_error_count.fetch_add(1);
                           if (received_count.load() == 2 && a_error_count.load() == 1) {
                             done.Signal();
                           }
                         });
  channel_b.StartReading(
      [&](MessageChannel::Message) {
        received_count.fetch_add(1);
        if (received_count.load() == 2 && a_error_count.load() == 1) {
          done.Signal();
        }
      },
      []() {});

  channel_a.Send(MakeMessage("drain-1"));
  channel_a.Send(MakeMessage("drain-2"));
  channel_a.Close(); // Graceful: pending writes drain, then A's error fires once.

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  EXPECT_EQ(received_count.load(), 2);
  EXPECT_EQ(a_error_count.load(), 1);
}

TEST(IpcMessageChannelTest, SendAfterErrorIsSilentlyDropped) {
  Thread thread("ipc-io");
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(thread.StartWithOptions(opts));
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();

  auto state = std::make_shared<DuplexState>();
  LoopbackDuplexStream stream_a(state, true);
  LoopbackDuplexStream stream_b(state, false);

  MessageChannel channel_a(runner, runner, &stream_a, &stream_a);
  MessageChannel channel_b(runner, runner, &stream_b, &stream_b);

  WaitableEvent a_errored(WaitableEvent::ResetPolicy::kAutomatic, false);
  channel_a.StartReading([](MessageChannel::Message) {}, [&]() { a_errored.Signal(); });
  channel_b.StartReading([](MessageChannel::Message) {}, []() {});

  // Push a corrupt frame into A's read side to put channel_a in error state.
  auto raw = scoped_refptr<IOBufferWithSize>(new IOBufferWithSize(8));
  const uint8_t bad_magic[8] = {1, 0, 0, 0, 0xBA, 0xAD, 0xF0, 0x0D};
  std::memcpy(raw->data(), bad_magic, sizeof(bad_magic));
  stream_b.WriteAsync(raw, sizeof(bad_magic), [](bool, std::size_t) {});
  ASSERT_TRUE(a_errored.TimedWait(std::chrono::seconds(5)));

  // Send on the errored channel must not crash and must not deliver.
  channel_a.Send(MakeMessage("never-delivered"));

  WaitableEvent b_received(WaitableEvent::ResetPolicy::kAutomatic, false);
  channel_b.StartReading([&](MessageChannel::Message) { b_received.Signal(); }, []() {});
  EXPECT_FALSE(b_received.TimedWait(std::chrono::milliseconds(200)));
}

// ---------------------------------------------------------------------------
// RpcEndpoint tests
// ---------------------------------------------------------------------------

TEST(IpcRpcEndpointTest, RequestResponseRoundTrip) {
  Thread thread("ipc-io");
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(thread.StartWithOptions(opts));
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();

  auto state = std::make_shared<DuplexState>();
  LoopbackDuplexStream stream_a(state, true);
  LoopbackDuplexStream stream_b(state, false);

  RpcEndpoint endpoint_a(runner, runner, &stream_a, &stream_a);
  RpcEndpoint endpoint_b(runner, runner, &stream_b, &stream_b);

  endpoint_a.SetRequestHandler([](RpcEndpoint::MessageBuffer payload, RpcEndpoint::ReplyCallback reply) {
    const std::string request(reinterpret_cast<const char *>(payload->data()), payload->size());
    if (request == "ping") {
      reply(MakeRpcPayload("pong"));
    }
  });
  endpoint_a.Start([]() {});
  endpoint_b.SetRequestHandler([](RpcEndpoint::MessageBuffer, RpcEndpoint::ReplyCallback) {}); // no-op
  endpoint_b.Start([]() {});

  WaitableEvent responded(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::string got;
  endpoint_b.SendRequest(MakeRpcPayload("ping"), TimeDelta::FromSeconds(1), [&](RpcEndpoint::MessageBuffer response) {
    if (response) {
      got.assign(reinterpret_cast<const char *>(response->data()), response->size());
    }
    responded.Signal();
  });

  ASSERT_TRUE(responded.TimedWait(std::chrono::seconds(5)));
  EXPECT_EQ(got, "pong");
}

TEST(IpcRpcEndpointTest, RequestTimeoutFiresNullResponse) {
  Thread thread("ipc-io");
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(thread.StartWithOptions(opts));
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();

  auto state = std::make_shared<DuplexState>();
  LoopbackDuplexStream stream_a(state, true);
  LoopbackDuplexStream stream_b(state, false);

  RpcEndpoint endpoint_a(runner, runner, &stream_a, &stream_a);
  RpcEndpoint endpoint_b(runner, runner, &stream_b, &stream_b);

  // Handler installed but never replies -- the request must time out.
  endpoint_a.SetRequestHandler([](RpcEndpoint::MessageBuffer, RpcEndpoint::ReplyCallback) {});
  endpoint_a.Start([]() {});
  endpoint_b.SetRequestHandler([](RpcEndpoint::MessageBuffer, RpcEndpoint::ReplyCallback) {});
  endpoint_b.Start([]() {});

  WaitableEvent responded(WaitableEvent::ResetPolicy::kAutomatic, false);
  bool got_null = false;
  endpoint_b.SendRequest(
      MakeRpcPayload("no-reply"), TimeDelta::FromMilliseconds(100), [&](RpcEndpoint::MessageBuffer response) {
        got_null = (response == nullptr);
        responded.Signal();
      });

  ASSERT_TRUE(responded.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(got_null);
}

TEST(IpcRpcEndpointTest, ChannelErrorAbortsPendingRequest) {
  Thread thread("ipc-io");
  Thread::Options opts;
  opts.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(thread.StartWithOptions(opts));
  scoped_refptr<SingleThreadTaskRunner> runner = thread.GetTaskRunner();

  auto state = std::make_shared<DuplexState>();
  LoopbackDuplexStream stream_a(state, true);
  LoopbackDuplexStream stream_b(state, false);

  RpcEndpoint endpoint_a(runner, runner, &stream_a, &stream_a);
  RpcEndpoint endpoint_b(runner, runner, &stream_b, &stream_b);

  endpoint_a.SetRequestHandler([](RpcEndpoint::MessageBuffer, RpcEndpoint::ReplyCallback) {});
  endpoint_a.Start([]() {});
  endpoint_b.SetRequestHandler([](RpcEndpoint::MessageBuffer, RpcEndpoint::ReplyCallback) {});
  endpoint_b.Start([]() {});

  WaitableEvent responded(WaitableEvent::ResetPolicy::kAutomatic, false);
  bool got_null = false;
  endpoint_b.SendRequest(
      MakeRpcPayload("pending"), TimeDelta::FromSeconds(5), [&](RpcEndpoint::MessageBuffer response) {
        got_null = (response == nullptr);
        responded.Signal();
      });

  // Close end A: B's read side hits EOF -> B's channel errors -> all pending
  // requests are aborted with null (well before the 5s timeout).
  stream_a.Close();

  ASSERT_TRUE(responded.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(got_null);
}

} // namespace
} // namespace nei
