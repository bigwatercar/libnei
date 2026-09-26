/**
 * @file storage_device_monitor_example.cpp
 * @brief Demonstrates whole-disk storage-device monitoring via nei::StorageDeviceMonitor
 *        (raw NETLINK_KOCKET_UEVENT netlink socket on Linux; block subsystem,
 *        DEVTYPE == "disk").  No libudev dependency.
 *
 * Usage:
 *   1. Build the libnei examples.
 *   2. Run the demo (no special privileges needed on a normal Linux host
 *      where udev is running).
 *   3. While it runs, plug/unplug an internal or USB whole-disk drive
 *      (e.g. a USB HDD/SSD).  Each add/remove/change emits an [EVENT] line.
 *
 * Cross-check with the raw kernel/udev source while running:
 *      udevadm monitor --kernel
 *
 * The callback is always invoked on the bound IO TaskRunner (the same IOThread
 * that drives the netlink fd), so no extra thread is needed here.
 */

#include <atomic>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include <neixx/common/location.h>
#include <neixx/io/io_thread.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/storage/storage_device_monitor.h>
#include <neixx/task/task_runner.h>

using namespace nei;
using std::chrono::milliseconds;

namespace {

  // [DIAG] Run for a fixed number of seconds so we can trigger an external udev
  // event while the monitor is alive (no signal handling in this diagnostic build).
  constexpr int kRunSeconds = 180;

void PrintEvent(const StorageDeviceMonitor::DeviceEventType type,
                const StorageDeviceMonitor::DeviceInfo &info) {
  const char *action =
      (type == StorageDeviceMonitor::DeviceEventType::kAdded) ? "ADDED  "
      : (type == StorageDeviceMonitor::DeviceEventType::kRemoved) ? "REMOVED"
      : "CHANGED";

  std::printf("[EVENT] %s  path=%-12s name=%-12s subsystem=%s\n",
              action,
              info.device_path.c_str(),
              info.name.c_str(),
              info.subsystem.c_str());
  std::fflush(stdout);
}

}  // namespace

int main() {
  std::printf("=== libnei Storage Device Monitor Demo ===\n\n");
  std::printf("Monitoring whole-disk block devices (DEVTYPE == disk):\n"
              "  add ->  ADDED   (e.g. a disk appears)\n"
              "  remove -> REMOVED (e.g. a disk disappears)\n"
              "  change -> CHANGED\n\n");
  std::printf("Tip:  cross-check with `udevadm monitor --udev` in another terminal.\n\n");

  // ---- Boot the IO thread (drives the udev netlink fd) -----------------
  if (!IOThread::Start()) {
    std::printf("[ERROR] Failed to start IO thread.\n");
    return 1;
  }
  auto runner = GetGlobalIOTaskRunner();
  if (!runner) {
    std::printf("[ERROR] Failed to obtain IO task runner.\n");
    IOThread::Shutdown();
    return 1;
  }

  // ---- Create the monitor ------------------------------------------------
  std::printf("[*] Creating StorageDeviceMonitor...\n");
  auto monitor = std::make_unique<StorageDeviceMonitor>(
      runner,
      [](StorageDeviceMonitor::DeviceEventType type,
         const StorageDeviceMonitor::DeviceInfo &info) {
        PrintEvent(type, info);
      });

  // ---- Start it on the IO sequence -------------------------------------
  WaitableEvent started(WaitableEvent::ResetPolicy::kManual);
  bool start_ok = false;
  runner->PostTask(FROM_HERE, [&monitor, &started, &start_ok]() {
    start_ok = monitor->Start();
    started.Signal();
  });
  if (!started.TimedWait(milliseconds(5000))) {
    std::printf("[ERROR] IO Start() task did not run.\n");
    IOThread::Shutdown();
    return 1;
  }
  if (!start_ok) {
    std::printf("[!] Start() returned false.\n"
                "    udev netlink is not reachable in this environment.\n"
                "    On a normal Linux host with udev running this succeeds; "
                "then plug/unplug a whole disk to see [EVENT] lines.\n");
    IOThread::Shutdown();
    return 1;
  }
  std::printf("[*] Monitor started. Watch for whole-disk add/remove/change.\n");
  std::printf("[*] [DIAG] running for %ds; fire `udevadm trigger -s block` now "
              "to see a [SDM-dbg] change event\n\n", kRunSeconds);

  // ---- Keep main alive for a fixed duration; events dispatch on IO thread
  for (int i = 0; i < kRunSeconds * 10; ++i) {
    std::this_thread::sleep_for(milliseconds(100));
  }

  // ---- Stop on the IO sequence, then tear down --------------------------
  std::printf("\n[*] Stopping monitor...\n");
  WaitableEvent stop_done(WaitableEvent::ResetPolicy::kManual);
  runner->PostTask(FROM_HERE, [&monitor, &stop_done]() {
    monitor->Stop();
    stop_done.Signal();
  });
  if (!stop_done.TimedWait(milliseconds(5000))) {
    std::printf("[ERROR] IO Stop() task did not run.\n");
    return 1;
  }
  std::printf("[*] Monitor stopped. Exiting.\n");

  IOThread::Shutdown();
  return 0;
}
