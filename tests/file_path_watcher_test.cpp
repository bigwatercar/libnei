// =============================================================================
// FilePathWatcher unit tests — create/modify/delete file change detection
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <neixx/common/location.h>
#include <neixx/files/file_path_watcher.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nei {
namespace {

using std::chrono::milliseconds;

// ===========================================================================
// Test helpers
// ===========================================================================

std::string CreateTempDir() {
#if defined(_WIN32)
  char tmp_path[MAX_PATH];
  DWORD len = ::GetTempPathA(MAX_PATH, tmp_path);
  EXPECT_GT(len, 0u);
  char dir_path[MAX_PATH];
  DWORD unique = ::GetTempFileNameA(tmp_path, "fpw", 0, dir_path);
  EXPECT_GT(unique, 0u);
  ::DeleteFileA(dir_path);
  EXPECT_NE(::CreateDirectoryA(dir_path, nullptr), 0);
  return std::string(dir_path);
#else
  char tmpl[] = "/tmp/libnei_fpw_test_XXXXXX";
  char* dir_path = mkdtemp(tmpl);
  EXPECT_NE(dir_path, nullptr);
  return std::string(dir_path);
#endif
}

void RemoveTempDir(const std::string& path) {
#if defined(_WIN32)
  std::string cmd = "rmdir /s /q \"" + path + "\" >nul 2>&1";
  (void)std::system(cmd.c_str());
#else
  std::string cmd = "rm -rf \"" + path + "\"";
  (void)std::system(cmd.c_str());
#endif
}

void CreateEmptyFile(const std::string& path) {
  std::ofstream ofs(path, std::ios::binary);
  ofs.close();
}

void SleepMs(int ms) {
  std::this_thread::sleep_for(milliseconds(ms));
}

// Destroys the watcher on its IO thread and waits.
void DestroyWatcherOnIO(scoped_refptr<TaskRunner> io_runner,
                        std::unique_ptr<FilePathWatcher>& watcher) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kManual);
  io_runner->PostTask(FROM_HERE, [&]() { watcher.reset(); done.Signal(); });
  done.TimedWait(milliseconds(5000));
}

// ===========================================================================
// Test fixture
// ===========================================================================

class FilePathWatcherTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = CreateTempDir();
    ASSERT_FALSE(temp_dir_.empty());

    Thread::Options opts;
    opts.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(io_thread_.StartWithOptions(opts));
    io_runner_ = io_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);
  }

  void TearDown() override {
    io_thread_.Stop();
    if (!temp_dir_.empty()) {
      RemoveTempDir(temp_dir_);
    }
  }

  scoped_refptr<TaskRunner> io_runner() const { return io_runner_; }

  std::string temp_dir_;
  Thread io_thread_{"fpw_io_thread"};
  scoped_refptr<TaskRunner> io_runner_;
};

// ===========================================================================
// Tests
// ===========================================================================

TEST_F(FilePathWatcherTest, DetectFileCreation) {
  auto watcher = std::make_unique<FilePathWatcher>(io_runner());
  WaitableEvent done(WaitableEvent::ResetPolicy::kManual);
  FilePathWatcher::ChangeType detected_type = FilePathWatcher::ChangeType::kModified;

  io_runner()->PostTask(FROM_HERE, [&]() {
    bool ok = watcher->Watch(
        temp_dir_, false,
        [&](const std::string&, FilePathWatcher::ChangeType type) {
          detected_type = type;
          done.Signal();
        });
    ASSERT_TRUE(ok);
  });

  SleepMs(200);
  CreateEmptyFile(temp_dir_ + "/test_file.txt");

  ASSERT_TRUE(done.TimedWait(milliseconds(5000)));
  EXPECT_EQ(detected_type, FilePathWatcher::ChangeType::kCreated);

  DestroyWatcherOnIO(io_runner(), watcher);
}

TEST_F(FilePathWatcherTest, DetectFileModification) {
  CreateEmptyFile(temp_dir_ + "/mod_test.txt");
  SleepMs(100);

  auto watcher = std::make_unique<FilePathWatcher>(io_runner());
  WaitableEvent done(WaitableEvent::ResetPolicy::kManual);
  FilePathWatcher::ChangeType detected_type = FilePathWatcher::ChangeType::kCreated;

  io_runner()->PostTask(FROM_HERE, [&]() {
    bool ok = watcher->Watch(
        temp_dir_, false,
        [&](const std::string&, FilePathWatcher::ChangeType type) {
          detected_type = type;
          done.Signal();
        });
    ASSERT_TRUE(ok);
  });

  SleepMs(200);

  {
    std::ofstream ofs(temp_dir_ + "/mod_test.txt", std::ios::app);
    ofs << "modified\n";
    ofs.close();
  }

  ASSERT_TRUE(done.TimedWait(milliseconds(5000)));
  EXPECT_EQ(detected_type, FilePathWatcher::ChangeType::kModified);

  DestroyWatcherOnIO(io_runner(), watcher);
}

TEST_F(FilePathWatcherTest, DetectFileDeletion) {
  CreateEmptyFile(temp_dir_ + "/del_test.txt");
  SleepMs(100);

  auto watcher = std::make_unique<FilePathWatcher>(io_runner());
  WaitableEvent done(WaitableEvent::ResetPolicy::kManual);
  FilePathWatcher::ChangeType detected_type = FilePathWatcher::ChangeType::kModified;

  io_runner()->PostTask(FROM_HERE, [&]() {
    bool ok = watcher->Watch(
        temp_dir_, false,
        [&](const std::string&, FilePathWatcher::ChangeType type) {
          detected_type = type;
          done.Signal();
        });
    ASSERT_TRUE(ok);
  });

  SleepMs(200);
  EXPECT_EQ(std::remove((temp_dir_ + "/del_test.txt").c_str()), 0);

  ASSERT_TRUE(done.TimedWait(milliseconds(5000)));
  EXPECT_EQ(detected_type, FilePathWatcher::ChangeType::kDeleted);

  DestroyWatcherOnIO(io_runner(), watcher);
}

TEST_F(FilePathWatcherTest, CancelStopsCallbacks) {
  auto watcher = std::make_unique<FilePathWatcher>(io_runner());
  std::atomic<int> callback_count{0};

  io_runner()->PostTask(FROM_HERE, [&]() {
    bool ok = watcher->Watch(
        temp_dir_, false,
        [&](const std::string&, FilePathWatcher::ChangeType) {
          ++callback_count;
        });
    ASSERT_TRUE(ok);
  });

  SleepMs(200);

  // Cancel synchronously on the IO thread.
  WaitableEvent cancel_done(WaitableEvent::ResetPolicy::kManual);
  io_runner()->PostTask(FROM_HERE, [&]() { watcher->Cancel(); cancel_done.Signal(); });
  ASSERT_TRUE(cancel_done.TimedWait(milliseconds(2000)));

  CreateEmptyFile(temp_dir_ + "/after_cancel.txt");
  SleepMs(500);
  EXPECT_EQ(callback_count.load(), 0);

  DestroyWatcherOnIO(io_runner(), watcher);
}

TEST_F(FilePathWatcherTest, WatchFailsWithEmptyPath) {
  auto watcher = std::make_unique<FilePathWatcher>(io_runner());
  WaitableEvent done(WaitableEvent::ResetPolicy::kManual);

  io_runner()->PostTask(FROM_HERE, [&]() {
    bool ok = watcher->Watch("", false,
                             [](const std::string&, FilePathWatcher::ChangeType) {});
    EXPECT_FALSE(ok);
    done.Signal();
  });

  ASSERT_TRUE(done.TimedWait(milliseconds(2000)));
  DestroyWatcherOnIO(io_runner(), watcher);
}

TEST_F(FilePathWatcherTest, WatchFailsWithNullCallback) {
  auto watcher = std::make_unique<FilePathWatcher>(io_runner());
  WaitableEvent done(WaitableEvent::ResetPolicy::kManual);

  io_runner()->PostTask(FROM_HERE, [&]() {
    bool ok = watcher->Watch(temp_dir_, false, nullptr);
    EXPECT_FALSE(ok);
    done.Signal();
  });

  ASSERT_TRUE(done.TimedWait(milliseconds(2000)));
  DestroyWatcherOnIO(io_runner(), watcher);
}

TEST_F(FilePathWatcherTest, DestroyImplicitlyCancels) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kManual);

  io_runner()->PostTask(FROM_HERE, [&]() {
    auto watcher = std::make_unique<FilePathWatcher>(io_runner());
    bool ok = watcher->Watch(
        temp_dir_, false,
        [](const std::string&, FilePathWatcher::ChangeType) {
          ADD_FAILURE() << "Callback fired after watcher destroyed";
        });
    ASSERT_TRUE(ok);
    watcher.reset();
    done.Signal();
  });

  ASSERT_TRUE(done.TimedWait(milliseconds(2000)));
  CreateEmptyFile(temp_dir_ + "/after_destroy.txt");
  SleepMs(300);
  SUCCEED();
}

TEST_F(FilePathWatcherTest, RewatchAfterCancel) {
  auto watcher = std::make_unique<FilePathWatcher>(io_runner());
  WaitableEvent done1(WaitableEvent::ResetPolicy::kManual);
  WaitableEvent done2(WaitableEvent::ResetPolicy::kManual);

  io_runner()->PostTask(FROM_HERE, [&]() {
    bool ok = watcher->Watch(
        temp_dir_, false,
        [&](const std::string&, FilePathWatcher::ChangeType) {
          done1.Signal();
        });
    ASSERT_TRUE(ok);
  });
  SleepMs(200);

  CreateEmptyFile(temp_dir_ + "/file1.txt");
  ASSERT_TRUE(done1.TimedWait(milliseconds(5000)));

  WaitableEvent rewatch_done(WaitableEvent::ResetPolicy::kManual);
  io_runner()->PostTask(FROM_HERE, [&]() {
    watcher->Cancel();
    bool ok = watcher->Watch(
        temp_dir_, false,
        [&](const std::string&, FilePathWatcher::ChangeType) {
          done2.Signal();
        });
    ASSERT_TRUE(ok);
    rewatch_done.Signal();
  });
  ASSERT_TRUE(rewatch_done.TimedWait(milliseconds(2000)));

  CreateEmptyFile(temp_dir_ + "/file2.txt");
  ASSERT_TRUE(done2.TimedWait(milliseconds(5000)));

  DestroyWatcherOnIO(io_runner(), watcher);
}

}  // namespace
}  // namespace nei
