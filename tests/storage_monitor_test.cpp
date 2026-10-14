// =============================================================================
// StorageMonitor unit tests - fake mountinfo / sysfs / dev-disk trees
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <dbt.h>
#endif

#include <neixx/common/location.h>
#include <neixx/io/io_thread.h>
#include <neixx/storage/storage_monitor.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_pool_instance.h>

namespace nei {
namespace {

namespace fs = std::filesystem;

struct RecordingObserver : RemovableStorageObserver {
  std::vector<StorageInfo> attached;
  std::vector<StorageInfo> detached;
  std::vector<std::string> order; // "A:<id>" / "D:<id>"

  void OnRemovableStorageAttached(const StorageInfo &info) override {
    attached.push_back(info);
    order.push_back("A:" + info.device_id);
  }

  void OnRemovableStorageDetached(const StorageInfo &info) override {
    detached.push_back(info);
    order.push_back("D:" + info.device_id);
  }
};

class StorageMonitorTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Other suites may leave a global pool behind; start from a known state.
    ThreadPoolInstance::ResetForTesting();
    if (WantsThreadPool())
      ThreadPoolInstance::CreateAndStartWithDefaultParams();
    IOThread::Start();
    io_runner_ = GetGlobalIOTaskRunner();
    ASSERT_TRUE(io_runner_);

    root_ = fs::temp_directory_path() / ("libnei_storage_test_" + std::to_string(counter_++) + "_" + UniqueSuffix());
    fs::create_directories(root_);
#if !defined(_WIN32)
    BuildFakeSystem();
#endif
  }

  void TearDown() override {
    IOThread::ResetForTesting();
    ThreadPoolInstance::ResetForTesting();
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  virtual bool WantsThreadPool() const {
    return false;
  }

  static std::string UniqueSuffix() {
    return std::to_string(reinterpret_cast<std::uintptr_t>(&counter_));
  }

  void WriteFile(const fs::path &p, const std::string &content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
  }

#if !defined(_WIN32)
  // sda1: fixed disk (8:1), no identifiers -> path fallback.  sdb1: USB stick
  // partition (8:17) with filesystem UUID and label.  sdc1: removable=1 disk
  // (8:33) with only a partition UUID.  sdd1: fixed partition with only a disk
  // wwid.  sde (8:49): whole removable disk with only a device serial.
  void BuildFakeSystem() {
    const fs::path sys = root_ / "sys";
    auto add_disk = [&](const std::string &bus_dir,
                        const std::string &disk,
                        const std::string &part,
                        const std::string &majmin,
                        const std::string &removable) {
      const fs::path disk_dir = sys / "devices/pci0" / bus_dir / "block" / disk;
      WriteFile(disk_dir / "removable", removable + "\n");
      WriteFile(disk_dir / "device/vendor", "SanDisk \n");
      WriteFile(disk_dir / "device/model", "Cruzer Blade     \n");
      WriteFile(disk_dir / part / "partition", "1\n");
      WriteFile(disk_dir / part / "size", "2048\n");
      fs::create_directories(sys / "dev/block");
      fs::create_directory_symlink(fs::path("../../devices/pci0") / bus_dir / "block" / disk / part,
                                   sys / "dev/block" / majmin);
    };
    add_disk("ata1", "sda", "sda1", "8:1", "0");
    add_disk("usb1", "sdb", "sdb1", "8:17", "0");
    add_disk("ata2", "sdc", "sdc1", "8:33", "1");
    add_disk("ata3", "sdd", "sdd1", "8:65", "0");
    WriteFile(sys / "devices/pci0/ata3/block/sdd/device/wwid", "naa.5001b448b1a3e0c3\n");

    const fs::path sde = sys / "devices/pci0/usb2/block/sde";
    WriteFile(sde / "removable", "1\n");
    WriteFile(sde / "size", "4096\n");
    WriteFile(sde / "device/serial", "USB-SERIAL-9 \n");
    fs::create_directories(sys / "dev/block");
    fs::create_directory_symlink("../../devices/pci0/usb2/block/sde", sys / "dev/block/8:49");

    WriteFile(sys / "devices/virtual/block/loop0/size", "1024\n");
    fs::create_directory_symlink("../../devices/virtual/block/loop0", sys / "dev/block/7:0");

    const fs::path by_uuid = root_ / "dev/disk/by-uuid";
    const fs::path by_partuuid = root_ / "dev/disk/by-partuuid";
    const fs::path by_label = root_ / "dev/disk/by-label";
    fs::create_directories(by_uuid);
    fs::create_directories(by_partuuid);
    fs::create_directories(by_label);
    fs::create_symlink("../../sdb1", by_uuid / "ABCD-1234");
    fs::create_symlink("../../sdb1", by_label / "MY\\x20USB");
    // sdb1 has both, so the filesystem UUID must win over the partition UUID.
    fs::create_symlink("../../sdb1", by_partuuid / "11111111-2222-3333-4444-555555555555");
    fs::create_symlink("../../sdc1", by_partuuid / "99999999-8888-7777-6666-555555555555");

    options_.mountinfo_path = (root_ / "mountinfo").string();
    options_.sysfs_root = sys.string();
    options_.dev_disk_root = (root_ / "dev/disk").string();
    options_.watch_changes = false;
  }
#endif

  void SetMountInfo(const std::vector<std::string> &lines) {
    std::string content;
    for (const auto &l : lines)
      content += l + "\n";
    WriteFile(root_ / "mountinfo", content);
  }

  void RunOnIO(std::function<void()> fn) {
    WaitableEvent done(WaitableEvent::ResetPolicy::kManual);
    io_runner_->PostTask(FROM_HERE, [&]() {
      fn();
      done.Signal();
    });
    ASSERT_TRUE(done.TimedWait(std::chrono::milliseconds(5000)));
  }

  static constexpr const char *kRoot = "30 1 8:1 / / rw,relatime shared:1 - ext4 /dev/sda1 rw";
  static constexpr const char *kUsb = "36 30 8:17 / /media/usb\\040stick rw - vfat /dev/sdb1 rw";

  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  fs::path root_;
  StorageMonitor::Options options_;
  static inline int counter_ = 0;
};

// Same as above plus a started ThreadPoolInstance, so event-driven rescans
// and ejects run on the pool.
class StorageMonitorAsyncTest : public StorageMonitorTest {
protected:
  void SetUp() override {
    StorageMonitorTest::SetUp();
    options_.on_event_rescan_for_testing = [this]() { ++hook_count_; };
  }

  bool WantsThreadPool() const override {
    return true;
  }

  bool WaitForHooks(int n) {
    for (int i = 0; i < 300; ++i) {
      if (hook_count_.load() >= n)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  // Runs EjectDevice on the IO thread and waits for the callback.  Verifies the
  // callback never runs before EjectDevice returns.
  EjectStatus EjectAndWait(StorageMonitor *monitor, const std::string &id) {
    struct State {
      WaitableEvent done{WaitableEvent::ResetPolicy::kManual};
      EjectStatus status = EjectStatus::kFailure;
      bool returned = false;
      bool ran_inline = false;
    };

    auto state = std::make_shared<State>();
    io_runner_->PostTask(FROM_HERE, [=]() {
      monitor->EjectDevice(id, [state](EjectStatus s) {
        state->ran_inline = !state->returned;
        state->status = s;
        state->done.Signal();
      });
      state->returned = true;
    });
    EXPECT_TRUE(state->done.TimedWait(std::chrono::milliseconds(5000)));
    EXPECT_FALSE(state->ran_inline);
    return state->status;
  }

  std::atomic<int> hook_count_{0};
};

#if defined(_WIN32)

int CountMonitorWindows() {
  int count = 0;
  ::EnumWindows(
      [](HWND hwnd, LPARAM lparam) -> BOOL {
        wchar_t name[64] = {};
        ::GetClassNameW(hwnd, name, 64);
        if (std::wstring(name).rfind(L"NeiStorageMonitor_", 0) == 0)
          ++*reinterpret_cast<int *>(lparam);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&count));
  return count;
}

// Sends WM_DEVICECHANGE only to the monitor's own windows, not system-wide.
void SendDeviceChangeToMonitorWindows() {
  ::EnumWindows(
      [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t name[64] = {};
        ::GetClassNameW(hwnd, name, 64);
        if (std::wstring(name).rfind(L"NeiStorageMonitor_", 0) == 0)
          ::SendMessageW(hwnd, WM_DEVICECHANGE, DBT_DEVNODES_CHANGED, 0);
        return TRUE;
      },
      0);
}

TEST_F(StorageMonitorTest, EnumeratesSystemDrive) {
  RunOnIO([&]() {
    options_.watch_changes = false;
    StorageMonitor monitor(io_runner_, options_);
    ASSERT_TRUE(monitor.Start());

    char drive[8] = {};
    ASSERT_GT(::GetEnvironmentVariableA("SystemDrive", drive, sizeof(drive)), 0u);
    const std::string system_drive = drive; // e.g. "C:"

    StorageInfo info;
    // Case-insensitive and tolerant of forward slashes.
    ASSERT_TRUE(monitor.GetStorageInfoForPath(system_drive + "\\Windows\\System32", &info));
    EXPECT_EQ(info.location, system_drive + "\\");
    EXPECT_FALSE(info.device_id.empty());
    EXPECT_GT(info.total_size_bytes, 0u);

    std::string lower = system_drive + "/windows";
    lower[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[0])));
    EXPECT_TRUE(monitor.GetStorageInfoForPath(lower, &info));

    EXPECT_FALSE(monitor.GetStorageInfoForPath("relative\\path", &info));
  });
}

TEST_F(StorageMonitorTest, BothNotificationBackendsStartStopRestart) {
  for (bool legacy : {false, true}) {
    RunOnIO([&]() {
      options_.use_legacy_window_notifications = legacy;
      StorageMonitor monitor(io_runner_, options_);
      ASSERT_TRUE(monitor.Start()) << "legacy=" << legacy;
      EXPECT_TRUE(monitor.is_started());
      monitor.Stop();
      EXPECT_FALSE(monitor.is_started());
      EXPECT_EQ(CountMonitorWindows(), 0);
      ASSERT_TRUE(monitor.Start());
    });
    EXPECT_EQ(CountMonitorWindows(), 0) << "window leaked after destruction, legacy=" << legacy;
  }
}

TEST_F(StorageMonitorTest, DefaultBackendUsesNoWindowWhenCmAvailable) {
  HMODULE cfgmgr = ::LoadLibraryExW(L"cfgmgr32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (cfgmgr == nullptr || ::GetProcAddress(cfgmgr, "CM_Register_Notification") == nullptr)
    GTEST_SKIP() << "CM_Register_Notification not available (pre-Win8)";
  ::FreeLibrary(cfgmgr);

  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    ASSERT_TRUE(monitor.Start());
    EXPECT_EQ(CountMonitorWindows(), 0);
  });
}

TEST_F(StorageMonitorTest, LegacyWindowPathRescansOnDeviceChange) {
  auto event_fired = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kManual);
  options_.use_legacy_window_notifications = true;
  options_.on_event_rescan_for_testing = [event_fired]() { event_fired->Signal(); };

  std::unique_ptr<StorageMonitor> monitor;
  RunOnIO([&]() {
    monitor = std::make_unique<StorageMonitor>(io_runner_, options_);
    ASSERT_TRUE(monitor->Start());
  });
  EXPECT_EQ(CountMonitorWindows(), 1);

  SendDeviceChangeToMonitorWindows();
  EXPECT_TRUE(event_fired->TimedWait(std::chrono::milliseconds(3000)));

  RunOnIO([&]() { monitor.reset(); });
}

TEST_F(StorageMonitorTest, DestroyedMonitorIgnoresPendingDelayedRescan) {
  auto event_fired = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kManual);
  options_.use_legacy_window_notifications = true;
  options_.on_event_rescan_for_testing = [event_fired]() { event_fired->Signal(); };

  std::unique_ptr<StorageMonitor> monitor;
  RunOnIO([&]() {
    monitor = std::make_unique<StorageMonitor>(io_runner_, options_);
    ASSERT_TRUE(monitor->Start());
  });
  SendDeviceChangeToMonitorWindows();
  RunOnIO([&]() { monitor.reset(); }); // delayed rescan still queued

  // Outlive the 1s settle delay; the stale task must not touch the freed monitor.
  std::this_thread::sleep_for(std::chrono::milliseconds(1300));
  RunOnIO([]() {});
}

TEST_F(StorageMonitorAsyncTest, SimulatedEventRescansOffThread) {
  std::unique_ptr<StorageMonitor> monitor;
  RunOnIO([&]() {
    options_.watch_changes = false;
    monitor = std::make_unique<StorageMonitor>(io_runner_, options_);
    ASSERT_TRUE(monitor->Start());
    monitor->SimulateChangeEventForTesting();
  });
  ASSERT_TRUE(WaitForHooks(1));
  RunOnIO([&]() {
    EXPECT_FALSE(monitor->GetAllAvailableStorages().empty());
    monitor.reset();
  });
}

TEST_F(StorageMonitorAsyncTest, EjectUnknownDeviceReportsNoSuchDevice) {
  std::unique_ptr<StorageMonitor> monitor;
  RunOnIO([&]() {
    options_.watch_changes = false;
    monitor = std::make_unique<StorageMonitor>(io_runner_, options_);
    ASSERT_TRUE(monitor->Start());
  });
  EXPECT_EQ(EjectAndWait(monitor.get(), "volume:{no-such}"), EjectStatus::kNoSuchDevice);
  RunOnIO([&]() { monitor.reset(); });
}

TEST_F(StorageMonitorAsyncTest, EjectFixedDriveIsRefused) {
  std::unique_ptr<StorageMonitor> monitor;
  std::string id;
  RunOnIO([&]() {
    options_.watch_changes = false;
    monitor = std::make_unique<StorageMonitor>(io_runner_, options_);
    ASSERT_TRUE(monitor->Start());
    char drive[8] = {};
    ASSERT_GT(::GetEnvironmentVariableA("SystemDrive", drive, sizeof(drive)), 0u);
    StorageInfo info;
    ASSERT_TRUE(monitor->GetStorageInfoForPath(std::string(drive) + "\\", &info));
    ASSERT_EQ(info.type, StorageInfo::Type::kFixedMassStorage);
    id = info.device_id;
  });
  EXPECT_EQ(EjectAndWait(monitor.get(), id), EjectStatus::kNotRemovable);
  RunOnIO([&]() { monitor.reset(); });
}

#else

TEST_F(StorageMonitorTest, InitialEnumerationReportsFixedAndRemovable) {
  SetMountInfo({kRoot, kUsb});
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    ASSERT_TRUE(monitor.Start());
    auto all = monitor.GetAllAvailableStorages();
    ASSERT_EQ(all.size(), 2u);

    StorageInfo usb, fixed;
    ASSERT_TRUE(monitor.GetStorageInfoForPath("/media/usb stick/dir/file", &usb));
    ASSERT_TRUE(monitor.GetStorageInfoForPath("/etc/passwd", &fixed));

    EXPECT_EQ(usb.type, StorageInfo::Type::kRemovableMassStorage);
    EXPECT_EQ(usb.device_id, "uuid:ABCD-1234");
    EXPECT_EQ(usb.location, "/media/usb stick");
    EXPECT_EQ(usb.label, "MY USB");
    EXPECT_EQ(usb.vendor, "SanDisk");
    EXPECT_EQ(usb.model, "Cruzer Blade");
    EXPECT_EQ(usb.total_size_bytes, 2048u * 512u);

    EXPECT_EQ(fixed.type, StorageInfo::Type::kFixedMassStorage);
    EXPECT_EQ(fixed.device_id, "path:/dev/sda1");
    EXPECT_EQ(fixed.location, "/");
  });
}

TEST_F(StorageMonitorTest, RemovableFlagWithoutUsbPathIsRemovable) {
  SetMountInfo({"40 30 8:33 / /mnt/card rw - vfat /dev/sdc1 rw"});
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    ASSERT_TRUE(monitor.Start());
    auto all = monitor.GetAllAvailableStorages();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].type, StorageInfo::Type::kRemovableMassStorage);
    EXPECT_TRUE(all[0].label.empty());
  });
}

TEST_F(StorageMonitorTest, DeviceIdPrefersTheMostStableIdentifierAvailable) {
  SetMountInfo({kRoot,
                kUsb,
                "40 30 8:33 / /mnt/card rw - vfat /dev/sdc1 rw",
                "41 30 8:65 / /mnt/wwid rw - ext4 /dev/sdd1 rw",
                "42 30 8:49 / /mnt/serial rw - vfat /dev/sde rw"});
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    ASSERT_TRUE(monitor.Start());
    StorageInfo info;

    // Filesystem UUID wins even though this partition also has a partition UUID.
    ASSERT_TRUE(monitor.GetStorageInfoForPath("/media/usb stick/f", &info));
    EXPECT_EQ(info.device_id, "uuid:ABCD-1234");

    // No filesystem UUID: the partition UUID is used next.
    ASSERT_TRUE(monitor.GetStorageInfoForPath("/mnt/card", &info));
    EXPECT_EQ(info.device_id, "partuuid:99999999-8888-7777-6666-555555555555");

    // No UUIDs at all: the disk wwid plus the partition index.
    ASSERT_TRUE(monitor.GetStorageInfoForPath("/mnt/wwid", &info));
    EXPECT_EQ(info.device_id, "wwid:naa.5001b448b1a3e0c3p1");

    // Whole disk with only a serial: no partition suffix.
    ASSERT_TRUE(monitor.GetStorageInfoForPath("/mnt/serial", &info));
    EXPECT_EQ(info.device_id, "serial:USB-SERIAL-9");

    // Nothing to identify the device: device-node fallback.
    ASSERT_TRUE(monitor.GetStorageInfoForPath("/etc/fstab", &info));
    EXPECT_EQ(info.device_id, "path:/dev/sda1");

    // Every volume still gets its own id.
    std::set<std::string> ids;
    for (const auto &s : monitor.GetAllAvailableStorages())
      EXPECT_TRUE(ids.insert(s.device_id).second) << s.device_id;
    EXPECT_EQ(ids.size(), 5u);
  });
}

TEST_F(StorageMonitorTest, NonDeviceAndUnknownMountsAreIgnored) {
  SetMountInfo({kRoot,
                "31 30 0:5 / /proc rw - proc proc rw",
                "32 30 0:20 / /tmp rw - tmpfs tmpfs rw",
                "33 30 7:0 / /snap/core rw - squashfs /dev/loop0 ro",
                "34 30 9:9 / /mnt/ghost rw - ext4 /dev/sdz9 rw"});
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    ASSERT_TRUE(monitor.Start());
    EXPECT_EQ(monitor.GetAllAvailableStorages().size(), 1u);
  });
}

TEST_F(StorageMonitorTest, InitialStoragesDoNotNotify) {
  SetMountInfo({kRoot, kUsb});
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    RecordingObserver obs;
    monitor.AddObserver(&obs);
    ASSERT_TRUE(monitor.Start());
    EXPECT_TRUE(obs.order.empty());
    monitor.RemoveObserver(&obs);
  });
}

TEST_F(StorageMonitorTest, RescanNotifiesAttachAndDetachOfRemovableOnly) {
  SetMountInfo({kRoot});
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    RecordingObserver obs;
    monitor.AddObserver(&obs);
    ASSERT_TRUE(monitor.Start());

    SetMountInfo({kRoot, kUsb});
    monitor.Rescan();
    ASSERT_EQ(obs.attached.size(), 1u);
    EXPECT_EQ(obs.attached[0].device_id, "uuid:ABCD-1234");
    EXPECT_EQ(monitor.GetAllAvailableStorages().size(), 2u);

    monitor.Rescan(); // no change
    EXPECT_EQ(obs.order.size(), 1u);

    SetMountInfo({kRoot});
    monitor.Rescan();
    ASSERT_EQ(obs.detached.size(), 1u);
    EXPECT_EQ(obs.detached[0].location, "/media/usb stick");
    EXPECT_EQ(monitor.GetAllAvailableStorages().size(), 1u);

    // Fixed disk disappearing is not reported.
    SetMountInfo({});
    monitor.Rescan();
    EXPECT_EQ(obs.order.size(), 2u);
    monitor.RemoveObserver(&obs);
  });
}

TEST_F(StorageMonitorTest, MovedMountPointNotifiesDetachThenAttach) {
  SetMountInfo({kUsb});
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    RecordingObserver obs;
    monitor.AddObserver(&obs);
    ASSERT_TRUE(monitor.Start());

    SetMountInfo({"36 30 8:17 / /mnt/other rw - vfat /dev/sdb1 rw"});
    monitor.Rescan();
    ASSERT_EQ(obs.order.size(), 2u);
    EXPECT_EQ(obs.order[0], "D:uuid:ABCD-1234");
    EXPECT_EQ(obs.order[1], "A:uuid:ABCD-1234");
    EXPECT_EQ(obs.attached[0].location, "/mnt/other");
    monitor.RemoveObserver(&obs);
  });
}

TEST_F(StorageMonitorTest, DeviceMountedTwiceIsReportedOnceAtFirstMountPoint) {
  SetMountInfo({kUsb, "37 30 8:17 /sub /srv/bind rw - vfat /dev/sdb1 rw"});
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    ASSERT_TRUE(monitor.Start());
    auto all = monitor.GetAllAvailableStorages();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].location, "/media/usb stick");
  });
}

TEST_F(StorageMonitorTest, ObserverMayUnregisterInsideCallback) {
  struct SelfRemoving : RemovableStorageObserver {
    StorageMonitor *monitor = nullptr;
    int calls = 0;

    void OnRemovableStorageAttached(const StorageInfo &) override {
      ++calls;
      monitor->RemoveObserver(this);
    }

    void OnRemovableStorageDetached(const StorageInfo &) override {
      ++calls;
    }
  };

  SetMountInfo({kRoot});
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    SelfRemoving a;
    RecordingObserver b;
    a.monitor = &monitor;
    monitor.AddObserver(&a);
    monitor.AddObserver(&b);
    ASSERT_TRUE(monitor.Start());

    SetMountInfo({kRoot, kUsb});
    monitor.Rescan();
    EXPECT_EQ(a.calls, 1);
    EXPECT_EQ(b.attached.size(), 1u);

    SetMountInfo({kRoot});
    monitor.Rescan();
    EXPECT_EQ(a.calls, 1); // unregistered
    EXPECT_EQ(b.detached.size(), 1u);
    monitor.RemoveObserver(&b);
  });
}

TEST_F(StorageMonitorTest, GetStorageInfoForPathUsesLongestMountPointOnComponentBoundary) {
  SetMountInfo({kRoot, kUsb});
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    ASSERT_TRUE(monitor.Start());

    StorageInfo info;
    ASSERT_TRUE(monitor.GetStorageInfoForPath("/media/usb stick", &info));
    EXPECT_EQ(info.device_id, "uuid:ABCD-1234");

    // "/media/usb stickX" is a sibling directory on the root volume.
    ASSERT_TRUE(monitor.GetStorageInfoForPath("/media/usb stickX/f", &info));
    EXPECT_EQ(info.location, "/");

    EXPECT_FALSE(monitor.GetStorageInfoForPath("relative/path", &info));
  });
}

TEST_F(StorageMonitorTest, RescanBeforeStartIsNoOp) {
  SetMountInfo({kUsb});
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    monitor.Rescan();
    EXPECT_TRUE(monitor.GetAllAvailableStorages().empty());
  });
}

TEST_F(StorageMonitorTest, StartWatchingRealMountTableAndStop) {
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_); // default Options: real procfs, epoll watch
    ASSERT_TRUE(monitor.Start());
    EXPECT_TRUE(monitor.is_started());
    EXPECT_TRUE(monitor.Start()); // idempotent
    monitor.Stop();
    EXPECT_FALSE(monitor.is_started());
    ASSERT_TRUE(monitor.Start()); // restartable
  });
}

TEST_F(StorageMonitorTest, EventWithoutThreadPoolRescansInline) {
  SetMountInfo({kRoot});
  int hooks = 0;
  options_.on_event_rescan_for_testing = [&hooks]() { ++hooks; };
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    RecordingObserver obs;
    monitor.AddObserver(&obs);
    ASSERT_TRUE(monitor.Start());

    SetMountInfo({kRoot, kUsb});
    monitor.SimulateChangeEventForTesting();
    EXPECT_EQ(hooks, 1); // synchronous when there is no pool
    EXPECT_EQ(obs.attached.size(), 1u);
    monitor.RemoveObserver(&obs);
  });
}

TEST_F(StorageMonitorAsyncTest, EventRescanRunsOnPoolAndNotifiesOnMonitorSequence) {
  SetMountInfo({kRoot});
  std::unique_ptr<StorageMonitor> monitor;
  RecordingObserver obs;
  RunOnIO([&]() {
    monitor = std::make_unique<StorageMonitor>(io_runner_, options_);
    monitor->AddObserver(&obs);
    ASSERT_TRUE(monitor->Start());
    SetMountInfo({kRoot, kUsb});
    monitor->SimulateChangeEventForTesting();
    EXPECT_TRUE(obs.attached.empty()); // not applied inline
  });
  ASSERT_TRUE(WaitForHooks(1));
  RunOnIO([&]() {
    ASSERT_EQ(obs.attached.size(), 1u);
    EXPECT_EQ(obs.attached[0].device_id, "uuid:ABCD-1234");
    monitor->RemoveObserver(&obs);
    monitor.reset();
  });
}

TEST_F(StorageMonitorAsyncTest, BurstOfEventsIsCoalescedAndConverges) {
  SetMountInfo({kRoot});
  std::unique_ptr<StorageMonitor> monitor;
  RecordingObserver obs;
  RunOnIO([&]() {
    monitor = std::make_unique<StorageMonitor>(io_runner_, options_);
    monitor->AddObserver(&obs);
    ASSERT_TRUE(monitor->Start());
    SetMountInfo({kRoot, kUsb});
    for (int i = 0; i < 20; ++i)
      monitor->SimulateChangeEventForTesting();
  });
  ASSERT_TRUE(WaitForHooks(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  RunOnIO([&]() {
    EXPECT_EQ(obs.attached.size(), 1u); // one notification despite 20 events
    EXPECT_LE(hook_count_.load(), 2);   // in-flight scan plus at most one follow-up
    EXPECT_EQ(monitor->GetAllAvailableStorages().size(), 2u);
    monitor->RemoveObserver(&obs);
    monitor.reset();
  });
}

TEST_F(StorageMonitorAsyncTest, DestroyingMonitorWithScanInFlightIsSafe) {
  SetMountInfo({kRoot, kUsb});
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    ASSERT_TRUE(monitor.Start());
    for (int i = 0; i < 5; ++i)
      monitor.SimulateChangeEventForTesting();
  }); // destroyed with scans pending
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  RunOnIO([]() {});
}

TEST_F(StorageMonitorAsyncTest, EjectFixedDeviceIsRefused) {
  SetMountInfo({kRoot, kUsb});
  std::unique_ptr<StorageMonitor> monitor;
  RunOnIO([&]() {
    monitor = std::make_unique<StorageMonitor>(io_runner_, options_);
    ASSERT_TRUE(monitor->Start());
  });
  EXPECT_EQ(EjectAndWait(monitor.get(), "path:/dev/sda1"), EjectStatus::kNotRemovable);
  RunOnIO([&]() { monitor.reset(); });
}

TEST_F(StorageMonitorAsyncTest, EjectUnknownDeviceReportsNoSuchDevice) {
  SetMountInfo({kRoot});
  std::unique_ptr<StorageMonitor> monitor;
  RunOnIO([&]() {
    monitor = std::make_unique<StorageMonitor>(io_runner_, options_);
    ASSERT_TRUE(monitor->Start());
  });
  EXPECT_EQ(EjectAndWait(monitor.get(), "uuid:nope"), EjectStatus::kNoSuchDevice);
  RunOnIO([&]() { monitor.reset(); });
}

TEST_F(StorageMonitorAsyncTest, EjectOfNonexistentMountPointFails) {
  // The fake mount point does not exist, so the unmount must not report success.
  SetMountInfo({kRoot, kUsb});
  std::unique_ptr<StorageMonitor> monitor;
  RunOnIO([&]() {
    monitor = std::make_unique<StorageMonitor>(io_runner_, options_);
    ASSERT_TRUE(monitor->Start());
  });
  EXPECT_NE(EjectAndWait(monitor.get(), "uuid:ABCD-1234"), EjectStatus::kOk);
  RunOnIO([&]() { monitor.reset(); });
}

TEST_F(StorageMonitorAsyncTest, EjectResolvesDeviceFromLiveStateNotSnapshot) {
  // A real directory at the stale mount point: unmounting it would fail with
  // EINVAL, so the test distinguishes "re-resolved" from "used the snapshot".
  const std::string stale_mount = (root_ / "stale_mnt").string();
  fs::create_directories(stale_mount);
  SetMountInfo({kRoot, "36 30 8:17 / " + stale_mount + " rw - vfat /dev/sdb1 rw"});
  std::unique_ptr<StorageMonitor> monitor;
  RunOnIO([&]() {
    monitor = std::make_unique<StorageMonitor>(io_runner_, options_);
    ASSERT_TRUE(monitor->Start());
    StorageInfo info;
    ASSERT_TRUE(monitor->GetStorageInfoForPath(stale_mount, &info));
    EXPECT_EQ(info.device_id, "uuid:ABCD-1234");
    // The snapshot still lists the device; the mount table no longer has it.
    SetMountInfo({kRoot});
  });
  // Must not treat the stale snapshot as authority.
  EXPECT_EQ(EjectAndWait(monitor.get(), "uuid:ABCD-1234"), EjectStatus::kNoSuchDevice);
  RunOnIO([&]() { monitor.reset(); });
}

TEST_F(StorageMonitorAsyncTest, EjectTypeComesFromLiveStateNotSnapshot) {
  constexpr const char *kCard = "40 30 8:33 / /mnt/card rw - vfat /dev/sdc1 rw";
  SetMountInfo({kRoot, kCard});
  std::unique_ptr<StorageMonitor> monitor;
  RunOnIO([&]() {
    monitor = std::make_unique<StorageMonitor>(io_runner_, options_);
    ASSERT_TRUE(monitor->Start());
    StorageInfo info;
    ASSERT_TRUE(monitor->GetStorageInfoForPath("/mnt/card", &info));
    ASSERT_EQ(info.type, StorageInfo::Type::kRemovableMassStorage);
    // The volume stops advertising itself as removable after the snapshot.
    WriteFile(root_ / "sys/devices/pci0/ata2/block/sdc/removable", "0\n");
  });
  EXPECT_EQ(EjectAndWait(monitor.get(), "partuuid:99999999-8888-7777-6666-555555555555"), EjectStatus::kNotRemovable);
  RunOnIO([&]() { monitor.reset(); });
}

TEST_F(StorageMonitorTest, EjectCallbackNotRunIfMonitorDestroyedFirst) {
  SetMountInfo({kRoot});
  auto ran = std::make_shared<std::atomic<bool>>(false);
  RunOnIO([&]() {
    StorageMonitor monitor(io_runner_, options_);
    ASSERT_TRUE(monitor.Start());
    monitor.EjectDevice("uuid:nope", [ran](EjectStatus) { ran->store(true); });
  }); // destroyed before the posted callback runs
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  RunOnIO([]() {});
  EXPECT_FALSE(ran->load());
}

#endif // _WIN32

} // namespace
} // namespace nei
