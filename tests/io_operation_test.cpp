#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <neixx/io/io_operation.h>
#include <neixx/task/sequenced_task_runner.h>
#include <neixx/task/thread_pool.h>

namespace nei {

TEST(IOOperationTokenTest, TokenReflectsStateCorrectly) {
  auto state = MakeRefCounted<IOOperationState>();
  auto token = MakeRefCounted<IOOperationToken>(state);

  EXPECT_FALSE(token->IsDone());
  EXPECT_FALSE(token->IsCancelled());
  EXPECT_FALSE(token->IsTimedOut());
  EXPECT_EQ(token->LastResult(), 0);
}

TEST(IOOperationStateTest, TryCompleteTransitionsState) {
  auto state = MakeRefCounted<IOOperationState>();
  bool callback_called = false;
  int callback_result = 0;

  bool completed = state->TryComplete(42, [&](int result) {
    callback_called = true;
    callback_result = result;
  });

  EXPECT_TRUE(completed);
  EXPECT_TRUE(callback_called);
  EXPECT_EQ(callback_result, 42);
  EXPECT_TRUE(state->IsDone());
  EXPECT_FALSE(state->IsCancelled());
  EXPECT_FALSE(state->IsTimedOut());
  EXPECT_EQ(state->LastResult(), 42);
}

TEST(IOOperationStateTest, TryCompleteIdempotent) {
  auto state = MakeRefCounted<IOOperationState>();
  int call_count = 0;

  bool first = state->TryComplete(10, [&](int) { call_count++; });
  bool second = state->TryComplete(20, [&](int) { call_count++; });

  EXPECT_TRUE(first);
  EXPECT_FALSE(second);  // Already completed, second call should fail
  EXPECT_EQ(call_count, 1);
  EXPECT_EQ(state->LastResult(), 10);
}

TEST(IOOperationStateTest, TryCancelTransitionsState) {
  auto state = MakeRefCounted<IOOperationState>();
  bool callback_called = false;
  int callback_result = 0;

  bool cancelled = state->TryCancel([&](int result) {
    callback_called = true;
    callback_result = result;
  });

  EXPECT_TRUE(cancelled);
  EXPECT_TRUE(callback_called);
  EXPECT_EQ(callback_result, -125);  // kCancelledResult
  EXPECT_TRUE(state->IsDone());
  EXPECT_TRUE(state->IsCancelled());
  EXPECT_FALSE(state->IsTimedOut());
}

TEST(IOOperationStateTest, TryCancelIdempotent) {
  auto state = MakeRefCounted<IOOperationState>();
  int call_count = 0;

  bool first = state->TryCancel([&](int) { call_count++; });
  bool second = state->TryCancel([&](int) { call_count++; });

  EXPECT_TRUE(first);
  EXPECT_FALSE(second);
  EXPECT_EQ(call_count, 1);
  EXPECT_TRUE(state->IsCancelled());
}

TEST(IOOperationStateTest, TryTimeoutTransitionsState) {
  auto state = MakeRefCounted<IOOperationState>();
  bool callback_called = false;
  int callback_result = 0;

  bool timed_out = state->TryTimeout([&](int result) {
    callback_called = true;
    callback_result = result;
  });

  EXPECT_TRUE(timed_out);
  EXPECT_TRUE(callback_called);
  EXPECT_EQ(callback_result, -110);  // kTimedOutResult
  EXPECT_TRUE(state->IsDone());
  EXPECT_TRUE(state->IsTimedOut());
  EXPECT_FALSE(state->IsCancelled());
}

TEST(IOOperationStateTest, StateTransitionsAreMutuallyExclusive) {
  auto state = MakeRefCounted<IOOperationState>();

  // Complete first
  bool completed = state->TryComplete(1, [](int) {});
  EXPECT_TRUE(completed);

  // Try to cancel after completion
  bool cancelled = state->TryCancel([](int) {});
  EXPECT_FALSE(cancelled);

  // Try to timeout after completion
  bool timed_out = state->TryTimeout([](int) {});
  EXPECT_FALSE(timed_out);
}

TEST(IOOperationStateTest, RequestCancelFiresHook) {
  auto state = MakeRefCounted<IOOperationState>();
  bool hook_called = false;

  // Hook is only called when RequestCancel is invoked
  state->BindCancelHook([&]() { hook_called = true; });
  EXPECT_FALSE(hook_called);  // Not called yet

  state->RequestCancel();
  EXPECT_TRUE(hook_called);  // Now called
}

TEST(IOOperationStateTest, BindCancelHookAfterRequest) {
  auto state = MakeRefCounted<IOOperationState>();
  bool hook_called = false;

  state->RequestCancel();
  state->BindCancelHook([&]() { hook_called = true; });

  EXPECT_TRUE(hook_called);  // Hook should fire immediately since cancel was already requested
}

TEST(IOOperationStateTest, CancelHookOnlyFiredOnce) {
  auto state = MakeRefCounted<IOOperationState>();
  int hook_call_count = 0;

  state->BindCancelHook([&]() { hook_call_count++; });
  EXPECT_EQ(hook_call_count, 0);  // Not called during binding

  state->RequestCancel();
  EXPECT_EQ(hook_call_count, 1);  // Called on first RequestCancel

  state->RequestCancel();
  EXPECT_EQ(hook_call_count, 1);  // Should only be called once, even with multiple RequestCancel
}

TEST(IOOperationStateTest, StartTimeoutWithoutTaskRunnerDoesNothing) {
  auto state = MakeRefCounted<IOOperationState>();
  bool callback_called = false;

  // Start timeout with nullptr task_runner (should be no-op)
  state->StartTimeoutWatch(std::chrono::milliseconds(10), nullptr, [&](int) {
    callback_called = true;
  });

  // Wait a bit to ensure timeout would have fired if it were set up
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_FALSE(callback_called);
  EXPECT_FALSE(state->IsTimedOut());
}

TEST(IOOperationStateTest, StartTimeoutWithZeroTimeoutDoesNothing) {
  auto state = MakeRefCounted<IOOperationState>();
  ThreadPool pool(1);
  auto runner = pool.CreateSequencedTaskRunner();
  bool callback_called = false;

  // Start timeout with zero timeout (should be no-op)
  state->StartTimeoutWatch(std::chrono::milliseconds(0), runner.get(), [&](int) {
    callback_called = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_FALSE(callback_called);
  EXPECT_FALSE(state->IsTimedOut());
  pool.Shutdown();
}

TEST(IOOperationStateTest, StartTimeoutFiresCallback) {
  ThreadPool pool(1);
  auto runner = pool.CreateSequencedTaskRunner();
  auto state = MakeRefCounted<IOOperationState>();
  std::atomic<bool> callback_called{false};
  std::atomic<int> callback_result{0};

  state->StartTimeoutWatch(std::chrono::milliseconds(50), runner.get(), [&](int result) {
    callback_result.store(result, std::memory_order_release);
    callback_called.store(true, std::memory_order_release);
  });

  // Wait for timeout to fire
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  pool.Shutdown();

  EXPECT_TRUE(callback_called.load(std::memory_order_acquire));
  EXPECT_EQ(callback_result.load(std::memory_order_acquire), -110);  // kTimedOutResult
  EXPECT_TRUE(state->IsTimedOut());
}

TEST(IOOperationStateTest, TokenCancelCallsRequestCancel) {
  auto state = MakeRefCounted<IOOperationState>();
  auto token = MakeRefCounted<IOOperationToken>(state);
  bool hook_called = false;

  state->BindCancelHook([&]() { hook_called = true; });
  hook_called = false;

  token->Cancel();

  EXPECT_TRUE(hook_called);
}

TEST(IOOperationTokenTest, TokenReflectsTimeoutState) {
  ThreadPool pool(1);
  auto runner = pool.CreateSequencedTaskRunner();
  auto state = MakeRefCounted<IOOperationState>();
  auto token = MakeRefCounted<IOOperationToken>(state);

  state->StartTimeoutWatch(std::chrono::milliseconds(50), runner.get(), [&](int) {});

  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  pool.Shutdown();

  EXPECT_TRUE(token->IsTimedOut());
  EXPECT_TRUE(token->IsDone());
  EXPECT_EQ(token->LastResult(), -110);
}

TEST(IOOperationStateTest, CompleteBeforeCancelHookStillFiresHook) {
  auto state = MakeRefCounted<IOOperationState>();
  bool hook_called = false;
  bool callback_called = false;

  state->TryComplete(42, [&](int) { callback_called = true; });

  state->BindCancelHook([&]() { hook_called = true; });
  state->RequestCancel();

  EXPECT_TRUE(callback_called);
  EXPECT_TRUE(hook_called);  // Hook is called even after operation completed
}

TEST(IOOperationStateTest, MultipleCompleteCallbacksAreIndependent) {
  auto state = MakeRefCounted<IOOperationState>();
  bool first_called = false;
  bool second_called = false;

  // Even though only the first succeeds, the second callback shouldn't be called
  state->TryComplete(1, [&](int) { first_called = true; });
  state->TryComplete(2, [&](int) { second_called = true; });

  EXPECT_TRUE(first_called);
  EXPECT_FALSE(second_called);
}

}  // namespace nei
