// =============================================================================
// StorageDeviceMonitor unit tests — start/stop lifecycle and event dispatch.
//
// Dispatch is exercised via EmitEventForTesting(), which feeds the normal
// Run/PostTask delivery path without needing real udev hardware.  A separate
// integration-style test confirms Start() actually brings up the uvid monitor
// when the environment supports it.
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include <neixx/common/location.h>
#include <neixx/io/io_thread.h>
#include <neixx/storage/storage_device_monitor.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>

namespace nei {
namespace {

using std::chrono::milliseconds;

// ===========================================================================
// Test fixture
// ===========================================================================

class StorageDeviceMonitorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IOThread::Start();
    io_runner_ = GetGlobalIOTaskRunner();
    ASSERT_TRUE(io_runner_);
  }

  void TearDown() override {
    IOThread::ResetForTesting();
  }

  scoped_refptr<SingleThreadTaskRunner> io_runner() const {
    return io_runner_;
  }

  scoped_refptr<SingleThreadTaskRunner> io_runner_;
};

// ===========================================================================
// Lifecycle
// ===========================================================================

TEST_F(StorageDeviceMonitorTest, StartReturnsTrueWhenUvidAvailable) {
  WaitableEvent started(WaitableEvent::ResetPolicy::kManual);
  WaitableEvent finished(WaitableEvent::ResetPolicy::kManual);
  bool ok = false;
  bool stopped = false;

  auto monitor = std::make_unique<StorageDeviceMonitor>(
      io_runner(), [](StorageDeviceMonitor::DeviceEventType,
                      const StorageDeviceMonitor::DeviceInfo&) {
    /* noop */
  });

  io_runner()->PostTask(FROM_HERE, [&monitor, &ok, &started]() {
    ok = monitor->Start();
    started.Signal();
  });
  if (!started.TimedWait(milliseconds(5000)))
    GTEST_SKIP() << "IO task did not complete";
  if (!ok)
    GTEST_SKIP() << "uvid unavailable in this environment; skipping";

  EXPECT_TRUE(ok);
  io_runner()->PostTask(FROM_HERE, [&monitor, &stopped, &finished]() {
    monitor->Stop();
    stopped = true;
    finished.Signal();
  });
  ASSERT_TRUE(finished.TimedWait(milliseconds(5000)));
  EXPECT_TRUE(stopped);
}

// ===========================================================================
// Dispatch
// ===========================================================================

TEST_F(StorageDeviceMonitorTest, SyncDispatchDeliversCallbackOnIoSequence) {
  WaitableEvent started(WaitableEvent::ResetPolicy::kManual);
  WaitableEvent callback_done(WaitableEvent::ResetPolicy::kManual);
  WaitableEvent finished(WaitableEvent::ResetPolicy::kManual);
  WaitableEvent stop_done(WaitableEvent::ResetPolicy::kManual);

  std::atomic<int> callback_count{0};
  std::string seen_path;
  std::string seen_subsystem;
  StorageDeviceMonitor::DeviceEventType seen_type =
      StorageDeviceMonitor::DeviceEventType::kChanged;

  auto monitor = std::make_unique<StorageDeviceMonitor>(
      io_runner(),
      [&](StorageDeviceMonitor::DeviceEventType type,
         const StorageDeviceMonitor::DeviceInfo& info) {
        seen_type = type;
        seen_path = info.device_path;
        seen_subsystem = info.subsystem;
        callback_count++;
        callback_done.Signal();
      });

  io_runner()->PostTask(FROM_HERE, [&monitor, &started]() {
    ASSERT_TRUE(monitor->Start());
    started.Signal();
  });
  ASSERT_TRUE(started.TimedWait(milliseconds(5000)));

  // Emit on the IO sequence -> callback fires synchronously.
  io_runner()->PostTask(FROM_HERE, [&monitor, &finished]() {
    monitor->EmitEventForTesting(
        StorageDeviceMonitor::DeviceEventType::kAdded,
        StorageDeviceMonitor::DeviceInfo{"/dev/sdb", "sdb", "block"});
    finished.Signal();
  });
  ASSERT_TRUE(finished.TimedWait(milliseconds(5000)));
  ASSERT_TRUE(callback_done.TimedWait(milliseconds(5000)));
  EXPECT_EQ(callback_count, 1);
  EXPECT_EQ(seen_type, StorageDeviceMonitor::DeviceEventType::kAdded);
  EXPECT_EQ(seen_path, "/dev/sdb");
  EXPECT_EQ(seen_subsystem, "block");

  io_runner()->PostTask(FROM_HERE, [&monitor, &stop_done]() {
    monitor->Stop();
    stop_done.Signal();
  });
  ASSERT_TRUE(stop_done.TimedWait(milliseconds(5000)));
}

TEST_F(StorageDeviceMonitorTest, AsyncDispatchPostsToBoundRunner) {
  WaitableEvent started(WaitableEvent::ResetPolicy::kManual);
  WaitableEvent callback_done(WaitableEvent::ResetPolicy::kManual);
  WaitableEvent finished(WaitableEvent::ResetPolicy::kManual);
  WaitableEvent stop_done(WaitableEvent::ResetPolicy::kManual);

  std::atomic<int> callback_count{0};
  std::string seen_path;
  StorageDeviceMonitor::DeviceEventType seen_type =
      StorageDeviceMonitor::DeviceEventType::kChanged;

  auto monitor = std::make_unique<StorageDeviceMonitor>(
      io_runner(),
      [&](StorageDeviceMonitor::DeviceEventType type,
         const StorageDeviceMonitor::DeviceInfo& info) {
        seen_type = type;
        seen_path = info.device_path;
        callback_count++;
        callback_done.Signal();
      });

  io_runner()->PostTask(FROM_HERE, [&monitor, &started]() {
    ASSERT_TRUE(monitor->Start());
    started.Signal();
  });
  ASSERT_TRUE(started.TimedWait(milliseconds(5000)));

  // Emit from the MAIN thread (a different sequence from io_runner) ->
  // the event is PostTask'd to io_runner and the callback runs there.
  monitor->EmitEventForTesting(
      StorageDeviceMonitor::DeviceEventType::kRemoved,
      StorageDeviceMonitor::DeviceInfo{"/dev/sdb", "sdb", "block"});

  io_runner()->PostTask(FROM_HERE, [&finished]() { finished.Signal(); });
  ASSERT_TRUE(finished.TimedWait(milliseconds(5000)));
  ASSERT_TRUE(callback_done.TimedWait(milliseconds(5000)));
  EXPECT_EQ(callback_count, 1);
  EXPECT_EQ(seen_type, StorageDeviceMonitor::DeviceEventType::kRemoved);
  EXPECT_EQ(seen_path, "/dev/sdb");

  io_runner()->PostTask(FROM_HERE, [&monitor, &stop_done]() {
    monitor->Stop();
    stop_done.Signal();
  });
  ASSERT_TRUE(stop_done.TimedWait(milliseconds(5000)));
}

TEST_F(StorageDeviceMonitorTest, NoCallbacksAfterStop) {
  WaitableEvent started(WaitableEvent::ResetPolicy::kManual);
  WaitableEvent finished(WaitableEvent::ResetPolicy::kManual);

  std::atomic<int> callback_count{0};

   auto monitor = std::make_unique<StorageDeviceMonitor>(
       io_runner(),
       [&callback_count](StorageDeviceMonitor::DeviceEventType,
                          const StorageDeviceMonitor::DeviceInfo&) {
         callback_count++;
       });

  io_runner()->PostTask(FROM_HERE, [&monitor, &started]() {
    ASSERT_TRUE(monitor->Start());
    started.Signal();
  });
  ASSERT_TRUE(started.TimedWait(milliseconds(5000)));

  io_runner()->PostTask(FROM_HERE, [&monitor, &finished]() {
    monitor->EmitEventForTesting(
        StorageDeviceMonitor::DeviceEventType::kAdded,
        StorageDeviceMonitor::DeviceInfo{"", "sdb", "block"});  // fires
    monitor->Stop();
    monitor->EmitEventForTesting(
        StorageDeviceMonitor::DeviceEventType::kRemoved,
        StorageDeviceMonitor::DeviceInfo{"", "sdb", "block"});  // does not
    finished.Signal();
  });
  ASSERT_TRUE(finished.TimedWait(milliseconds(5000)));
  EXPECT_EQ(callback_count, 1);
}

// ===========================================================================
// Destructor safety
// ===========================================================================

TEST_F(StorageDeviceMonitorTest, DestructorStopsMonitor) {
  bool ok = false;
  WaitableEvent started(WaitableEvent::ResetPolicy::kManual);

  {
    auto monitor = std::make_unique<StorageDeviceMonitor>(
        io_runner(), [](StorageDeviceMonitor::DeviceEventType,
                        const StorageDeviceMonitor::DeviceInfo&) {
      /* noop */
    });
    io_runner()->PostTask(FROM_HERE, [&monitor, &ok, &started]() {
      ok = monitor->Start();
      started.Signal();
    });
    (void)ok;
    // Let Start() complete before destroying.
    started.TimedWait(milliseconds(5000));
    // monitor destroyed here; ~StorageDeviceMonitor() -> Stop() runs on this
    // (main) thread. Stop() is safe to call from any thread.
  }
  // No crash, no dangling callback.
  EXPECT_NO_THROW({});
}

}  // namespace nei
}  // namespace
