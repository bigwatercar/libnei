#include <neixx/io/async_file.h>

#include <memory>
#include <utility>

#include <neixx/task/task_runner.h>

#if defined(_WIN32)
#include <async_file_win.h>
#else
#include <async_file_posix.h>
#endif

namespace nei {

std::unique_ptr<AsyncFile> AsyncFile::Create(
    scoped_refptr<TaskRunner> io_task_runner) {
#if defined(_WIN32)
  return std::make_unique<AsyncFileWin>(std::move(io_task_runner));
#else
  return std::make_unique<AsyncFilePosix>(std::move(io_task_runner));
#endif
}

}  // namespace nei