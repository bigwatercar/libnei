#pragma once

#ifndef NEIXX_IO_ASYNC_FILE_WIN_H_
#define NEIXX_IO_ASYNC_FILE_WIN_H_

#if defined(_WIN32)

#include <memory>
#include <string>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_file.h>

namespace nei {

class TaskRunner;

class NEI_API AsyncFileWin final : public AsyncFile {
 public:
  struct StageCounters {
    std::uint64_t open_reached = 0;
    std::uint64_t write_reached = 0;
    std::uint64_t read_reached = 0;
    std::uint64_t write_post_seq = 0;
    std::uint64_t write_exec_seq = 0;
    std::uint64_t read_post_seq = 0;
    std::uint64_t read_exec_seq = 0;
    std::uint64_t iocp_completed = 0;
    std::uint64_t context_hit = 0;
    std::uint64_t context_miss = 0;
    std::uint64_t read_finalize_attempted = 0;
    std::uint64_t read_posted = 0;
    std::uint64_t callback_weak_dropped = 0;
    std::uint64_t callback_post_failed = 0;
  };

  explicit AsyncFileWin(scoped_refptr<TaskRunner> io_task_runner);
  AsyncFileWin(AsyncFileWin&& other) noexcept;
  AsyncFileWin& operator=(AsyncFileWin&& other) noexcept;
  ~AsyncFileWin() override;

  AsyncFileWin(const AsyncFileWin&) = delete;
  AsyncFileWin& operator=(const AsyncFileWin&) = delete;

  void OpenAsync(const std::string& path,
                 OpenMode mode,
                 OpenDisposition disposition,
                 const scoped_refptr<TaskRunner>& background_runner,
                 OpenCallback callback) override;

  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t bytes_to_read,
                 std::uint64_t offset,
                 ReadCallback callback) override;

  void WriteAsync(scoped_refptr<IOBuffer> buf,
                  std::size_t bytes_to_write,
                  std::uint64_t offset,
                  WriteCallback callback) override;

  static void ResetStageCountersForTesting();
  static StageCounters GetStageCountersForTesting();

  void Close(CloseCallback callback) override;
  bool is_open() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nei

#endif  // defined(_WIN32)

#endif  // NEIXX_IO_ASYNC_FILE_WIN_H_