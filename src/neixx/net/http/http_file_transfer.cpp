// HttpFileTransfer — large-file convenience helpers (download/upload to disk).

#include <neixx/net/http/http_file_transfer.h>

#include <cstdint>
#include <cstring>
#include <memory>

#include <neixx/io/async_file.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>

namespace nei {
namespace net::http {

namespace {

// ---------------------------------------------------------------------------
// DownloadToFile state
// ---------------------------------------------------------------------------
struct DownloadState : public RefCountedThreadSafe<DownloadState> {
  // In-flight write watermark for download backpressure: while more than
  // kHighWatermark file writes are queued (each holding an IOBuffer), the
  // download is paused via BodyChunkCallback returning false; it resumes once
  // the queue drains to kLowWatermark.  Keeps memory bounded when the disk
  // cannot keep up with the network.
  static constexpr int kHighWatermark = 8;
  static constexpr int kLowWatermark = 2;

  std::unique_ptr<AsyncFile> file;
  std::uint64_t offset = 0;
  int pending_writes = 0;
  bool body_done = false;
  bool success = true;
  bool finishing = false;
  bool download_paused = false;
  std::size_t total_bytes = 0;
  // SendStreaming handle, used to resume the download after the write queue
  // drains below the low watermark.
  HttpRequestHandle handle;
  std::function<void(bool, std::size_t)> on_done;
};

// Close the file and fire on_done once the body has fully drained and all
// in-flight file writes have completed.
void FinishIfDone(const scoped_refptr<DownloadState> &state) {
  if (state->body_done && state->pending_writes == 0 && !state->finishing) {
    state->finishing = true;
    state->file->CloseAsync([state]() {
      auto cb = std::move(state->on_done);
      if (cb)
        cb(state->success, state->total_bytes);
    });
  }
}

// ---------------------------------------------------------------------------
// UploadFromFile state
// ---------------------------------------------------------------------------
struct UploadState : public RefCountedThreadSafe<UploadState> {
  std::unique_ptr<AsyncFile> file;
  std::unique_ptr<HttpResponse> response;
  std::uint64_t offset = 0;
  std::function<void(bool, std::unique_ptr<HttpResponse>)> on_done;
  static constexpr std::size_t kChunkSize = 64 * 1024;
};

} // namespace

// ===========================================================================
// DownloadToFile
// ===========================================================================

void DownloadToFile(scoped_refptr<HttpClient> client,
                    const HttpRequest &request,
                    const net::IPEndPoint &endpoint,
                    net::SSLContext *ssl_ctx,
                    scoped_refptr<SingleThreadTaskRunner> io_runner,
                    scoped_refptr<SequencedTaskRunner> background_runner,
                    const std::filesystem::path &file_path,
                    std::function<void(bool, std::size_t)> on_done) {
  auto state = MakeRefCounted<DownloadState>();
  state->on_done = std::move(on_done);
  state->file = AsyncFile::Create(io_runner);
  if (!state->file) {
    auto cb = std::move(state->on_done);
    if (cb)
      cb(false, 0);
    return;
  }

  state->file->OpenAsync(file_path,
                         AsyncFile::OpenMode::kWriteOnly,
                         AsyncFile::OpenDisposition::kCreateAlways,
                         background_runner,
                         [state, client, request, endpoint, ssl_ctx, io_runner](bool ok, AsyncFile::Error) {
                           if (!ok) {
                             state->file->CloseAsync(nullptr);
                             auto cb = std::move(state->on_done);
                             if (cb)
                               cb(false, 0);
                             return;
                           }

                           auto download_handle = client->SendStreaming(
                               request,
                               endpoint,
                               ssl_ctx,
                               io_runner,
                               // on_headers: reject non-2xx by marking failure (body is discarded).
                               [state](HttpStatus status, const HttpHeaders &) {
                                 const int code = status.raw_code();
                                 if (code < 200 || code >= 300)
                                   state->success = false;
                               },
                               // on_body: copy each chunk into a pool buffer and write to the
                               // file at the running offset (offset-based writes are independent).
                               // Backpressure: when the in-flight write queue hits the high
                               // watermark, pause the download; the write callback resumes it
                               // once the queue drains to the low watermark.
                               [state](const char *data, std::size_t len, bool done) -> bool {
                                 if (done) {
                                   state->body_done = true;
                                   FinishIfDone(state);
                                   return true;
                                 }
                                 if (len == 0)
                                   return true;
                                 state->total_bytes += len;
                                 if (!state->success) {
                                   // Non-2xx: discard the (error) body bytes.
                                   return true;
                                 }
                                 auto buf = IOBufferPool::GetInstance().AcquireBuffer(len);
                                 std::memcpy(buf->data(), data, len);
                                 ++state->pending_writes;
                                 const std::uint64_t write_offset = state->offset;
                                 state->offset += len;
                                 state->file->WriteAsync(
                                     buf, len, write_offset, [state, buf](bool ok, std::size_t, AsyncFile::Error) {
                                       --state->pending_writes;
                                       if (!ok)
                                         state->success = false;
                                       if (state->download_paused && state->pending_writes <= state->kLowWatermark) {
                                         state->download_paused = false;
                                         state->handle.Resume();
                                       }
                                       FinishIfDone(state);
                                     });
                                 if (state->pending_writes >= state->kHighWatermark) {
                                   // The current chunk is queued; pause delivery until the
                                   // write queue drains.
                                   state->download_paused = true;
                                   return false;
                                 }
                                 return true;
                               });

                           state->handle = std::move(download_handle);
                         });
}

// ===========================================================================
// UploadFromFile
// ===========================================================================

void UploadFromFile(scoped_refptr<HttpClient> client,
                    const HttpRequest &request,
                    const net::IPEndPoint &endpoint,
                    net::SSLContext *ssl_ctx,
                    scoped_refptr<SingleThreadTaskRunner> io_runner,
                    scoped_refptr<SequencedTaskRunner> background_runner,
                    const std::filesystem::path &file_path,
                    std::function<void(bool, std::unique_ptr<HttpResponse>)> on_done) {
  auto state = MakeRefCounted<UploadState>();
  state->on_done = std::move(on_done);
  state->file = AsyncFile::Create(io_runner);
  if (!state->file) {
    auto cb = std::move(state->on_done);
    if (cb)
      cb(false, nullptr);
    return;
  }

  state->file->OpenAsync(
      file_path,
      AsyncFile::OpenMode::kReadOnly,
      AsyncFile::OpenDisposition::kOpenExisting,
      background_runner,
      [state, client, request, endpoint, ssl_ctx, io_runner](bool ok, AsyncFile::Error) {
        if (!ok) {
          state->file->CloseAsync(nullptr);
          auto cb = std::move(state->on_done);
          if (cb)
            cb(false, nullptr);
          return;
        }

        AsyncFile *file = state->file.get();
        // Pull-based provider: each client pull reads the next file chunk and
        // delivers it via on_chunk (one chunk in flight → bounded memory).
        HttpClient::RequestBodyProvider provider = [state, file](HttpClient::UploadBodyChunkCallback on_chunk) {
          auto buf = IOBufferPool::GetInstance().AcquireBuffer(UploadState::kChunkSize);
          file->ReadAsync(buf,
                          UploadState::kChunkSize,
                          state->offset,
                          [state, buf, on_chunk](bool ok, std::size_t n, AsyncFile::Error) {
                            if (!ok || n == 0) {
                              on_chunk(nullptr, 0, true); // EOF / read error.
                              return;
                            }
                            state->offset += n;
                            on_chunk(reinterpret_cast<const char *>(buf->data()), n, false);
                          });
        };

        client->SendBody(
            request, endpoint, ssl_ctx, io_runner, std::move(provider), [state](std::unique_ptr<HttpResponse> resp) {
              state->response = std::move(resp);
              const bool ok = state->response && state->response->status.raw_code() >= 200
                              && state->response->status.raw_code() < 300;
              state->file->CloseAsync([state, ok]() {
                auto cb = std::move(state->on_done);
                if (cb)
                  cb(ok, std::move(state->response));
              });
            });
      });
}

} // namespace net::http
} // namespace nei
