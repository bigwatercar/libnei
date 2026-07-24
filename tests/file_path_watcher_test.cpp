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
#include <neixx/functional/bind.h>
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

// =============================================================================
// Basic API tests (8)
// =============================================================================

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

// =============================================================================
// Regression tests (Bug 1/2/3 — 2026-07-24)
// =============================================================================

// ---------------------------------------------------------------------------
// 靶点 1 — DestroyWhileIoPending
//
// Arrange:  create watcher on IO thread.
// Act:      flood watched directory with file creations from main thread
//           while the watcher is alive, then post a destroy-task to the
//           IO thread.  The destroy runs while IOCP/inotify events are
//           still queued or in-flight.
// Assert:   a barrier task posted after the destroy confirms the IO thread
//           drained all residual completions without a crash / UAF.
// ---------------------------------------------------------------------------
TEST_F(FilePathWatcherTest, DestroyWhileIoPending) {
  WaitableEvent watch_ready(WaitableEvent::ResetPolicy::kManual);

  // Create watcher + arm watch on the IO thread.
  auto runner = io_runner();
  io_runner()->PostTask(FROM_HERE, [runner, this, &watch_ready]() {
    auto watcher = std::make_unique<FilePathWatcher>(runner);
    bool ok = watcher->Watch(temp_dir_, /*recursive=*/false,
                             [](const std::string&,
                                FilePathWatcher::ChangeType) {});
    ASSERT_TRUE(ok);

    // Stash the watcher in a shared_ptr on the IO thread so the
    // destroy-task (posted later) can reach it.
    auto stash = std::make_shared<std::unique_ptr<FilePathWatcher>>(
        std::move(watcher));

    // Post the destroy-task right after arming.
    runner->PostTask(FROM_HERE, [stash]() { stash->reset(); });

    watch_ready.Signal();
  });

  ASSERT_TRUE(watch_ready.TimedWait(milliseconds(5000)));

  // Flood the directory from the main thread.  The watcher is still alive
  // on the IO thread; the destroy-task is queued behind the watch-arming
  // task but may execute concurrently with this flood on a multi-core
  // machine.
  constexpr int kFileCount = 200;
  for (int i = 0; i < kFileCount; ++i) {
    CreateEmptyFile(temp_dir_ + "/stress_" + std::to_string(i) + ".txt");
  }

  // Barrier: post a no-op task.  When it runs we know the destroy-task
  // (and any residual IOCP/inotify completions) have been processed.
  WaitableEvent barrier(WaitableEvent::ResetPolicy::kManual);
  io_runner()->PostTask(FROM_HERE, [&barrier]() { barrier.Signal(); });
  ASSERT_TRUE(barrier.TimedWait(milliseconds(10000)));

  SUCCEED();
}

// ---------------------------------------------------------------------------
// 靶点 2 — CancelPreventsPendingCallbacks
//
// Arrange:  watcher on IO thread, callback runner = separate thread.
//           This forces DeliverChange through the PostTask + WeakPtr path.
// Act:      trigger an OS event, then immediately Cancel() on the IO
//           thread before the posted callback is drained.
// Assert:   the callback counter remains zero — WeakPtr check silently
//           dropped the queued invocation.
// ---------------------------------------------------------------------------
TEST_F(FilePathWatcherTest, CancelPreventsPendingCallbacks) {
  Thread cb_thread("fpw_cb");
  Thread::Options cb_opts;
  cb_opts.message_pump_type = MessagePumpType::DEFAULT;
  ASSERT_TRUE(cb_thread.StartWithOptions(cb_opts));
  scoped_refptr<TaskRunner> cb_runner = cb_thread.GetTaskRunner();

  // Gate: block cb_thread from processing anything until we say so.
  WaitableEvent cb_gate(WaitableEvent::ResetPolicy::kManual);
  cb_runner->PostTask(FROM_HERE, [&cb_gate]() { cb_gate.Wait(); });

  std::atomic<int> callbacks_fired{0};
  auto watcher = std::make_unique<FilePathWatcher>(cb_runner);

  WaitableEvent watch_ready(WaitableEvent::ResetPolicy::kManual);
  io_runner()->PostTask(FROM_HERE, [&]() {
    bool ok = watcher->Watch(
        temp_dir_, /*recursive=*/false,
        [&callbacks_fired](const std::string&,
                           FilePathWatcher::ChangeType) {
          ++callbacks_fired;
        });
    ASSERT_TRUE(ok);
    watch_ready.Signal();
  });
  ASSERT_TRUE(watch_ready.TimedWait(milliseconds(5000)));

  // Trigger an OS event.  DeliverChange will post the callback to
  // cb_runner, but cb_thread is blocked behind the gate.
  CreateEmptyFile(temp_dir_ + "/ghost.txt");

  // Cancel while the callback is queued but not yet executed.
  WaitableEvent cancel_done(WaitableEvent::ResetPolicy::kManual);
  io_runner()->PostTask(FROM_HERE, [&]() {
    watcher->Cancel();
    cancel_done.Signal();
  });
  ASSERT_TRUE(cancel_done.TimedWait(milliseconds(5000)));

  // Open the gate.  The queued callback will run, hit the WeakPtr +
  // watching_ + callback_ triple check, and silently return.
  cb_gate.Signal();

  // Barrier: wait until cb_thread has drained all queued tasks.
  WaitableEvent cb_drained(WaitableEvent::ResetPolicy::kManual);
  cb_runner->PostTask(FROM_HERE, [&cb_drained]() { cb_drained.Signal(); });
  ASSERT_TRUE(cb_drained.TimedWait(milliseconds(5000)));

  EXPECT_EQ(callbacks_fired.load(), 0);

  DestroyWatcherOnIO(io_runner(), watcher);
  cb_thread.Stop();
}

// ---------------------------------------------------------------------------
// 靶点 3 — RecursiveWatchDetectsNewSubdirectoryChanges
//
// Arrange:  recursive watch on a temp directory.
// Act:      create a new subdirectory, then immediately create a file
//           inside that subdirectory.
// Assert:   the callback fires AND the relative path contains the
//           subdirectory component (proving the watcher dynamically
//           added the inotify watch for the new subdir).
// ---------------------------------------------------------------------------
TEST_F(FilePathWatcherTest, RecursiveWatchDetectsNewSubdirectoryChanges) {
  auto watcher = std::make_unique<FilePathWatcher>(io_runner());

  WaitableEvent got_subdir(WaitableEvent::ResetPolicy::kManual);
  WaitableEvent got_file(WaitableEvent::ResetPolicy::kManual);
  std::string first_path;
  std::string second_path;
  int callback_seq = 0;

  io_runner()->PostTask(FROM_HERE, [&]() {
    bool ok = watcher->Watch(
        temp_dir_, /*recursive=*/true,
        [&](const std::string& path, FilePathWatcher::ChangeType) {
          ++callback_seq;
          if (callback_seq == 1) {
            first_path = path;
            got_subdir.Signal();
          } else {
            second_path = path;
            got_file.Signal();
          }
        });
    ASSERT_TRUE(ok);
  });

  // Let the initial recursive scan arm.
  SleepMs(200);

  // Create a new subdirectory.
  std::string subdir = temp_dir_ + "/new_subdir";
#if defined(_WIN32)
  EXPECT_NE(::CreateDirectoryA(subdir.c_str(), nullptr), 0);
#else
  EXPECT_EQ(mkdir(subdir.c_str(), 0755), 0);
#endif

  // Wait for the subdirectory-creation callback.
  ASSERT_TRUE(got_subdir.TimedWait(milliseconds(5000)));
  EXPECT_NE(first_path.find("new_subdir"), std::string::npos);

  // Now create a file inside the new subdirectory.  This is the acid test —
  // if recursive watch is broken, this event will never fire.
  CreateEmptyFile(subdir + "/target.txt");

  ASSERT_TRUE(got_file.TimedWait(milliseconds(5000)));
  EXPECT_NE(second_path.find("new_subdir"), std::string::npos);
  EXPECT_NE(second_path.find("target.txt"), std::string::npos);

  DestroyWatcherOnIO(io_runner(), watcher);
}

}  // namespace
}  // namespace nei
