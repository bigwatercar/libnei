#pragma once

#ifndef NEIXX_IO_ASYNC_FILE_WIN_H_
#define NEIXX_IO_ASYNC_FILE_WIN_H_

#if defined(_WIN32)

#include <memory>
#include <string>
#include <vector>

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
  };

  explicit AsyncFileWin(scoped_refptr<TaskRunner> io_task_runner);
  ~AsyncFileWin() override;

  AsyncFileWin(const AsyncFileWin&) = delete;
  AsyncFileWin& operator=(const AsyncFileWin&) = delete;

  void OpenAsync(const std::string& path,
                 OpenMode mode,
                 OpenDisposition disposition,
                 const scoped_refptr<TaskRunner>& background_runner,
                 OpenCallback callback) override;

  bool AsyncRead(std::int64_t offset,
                 std::size_t size,
                 ReadCallback callback) override;

  bool AsyncWrite(std::int64_t offset,
                  std::vector<std::uint8_t> buffer,
                  WriteCallback callback) override;

  static void ResetStageCountersForTesting();
  static StageCounters GetStageCountersForTesting();

  void Close() override;
  bool is_open() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nei

#endif  // defined(_WIN32)

#endif  // NEIXX_IO_ASYNC_FILE_WIN_H_
