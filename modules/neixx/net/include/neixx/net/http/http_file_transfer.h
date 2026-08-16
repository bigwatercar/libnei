#pragma once

#ifndef NEIXX_NET_HTTP_HTTP_FILE_TRANSFER_H_
#define NEIXX_NET_HTTP_HTTP_FILE_TRANSFER_H_

#include <cstddef>
#include <filesystem>
#include <functional>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/task/task_runner.h>

namespace nei {

namespace net {
class IPEndPoint;
class SSLContext;
} // namespace net

namespace net::http {

// =============================================================================
// HttpFileTransfer — large-file convenience helpers
// =============================================================================
// Streams a request/response body directly between the network and disk via
// HttpClient streaming + AsyncFile, keeping memory bounded regardless of file
// size.  All callbacks run on the supplied |io_runner|; |background_runner| is
// used for the (blocking) file open only.
//
//   auto bg = ThreadPoolInstance::Get()->CreateSequencedTaskRunner(TaskTraits());
//   HttpRequest req; req.method = HttpMethod::kGet; req.url = Url("...");
//   DownloadToFile(client, req, endpoint, nullptr, io_runner, bg,
//                  "out.bin", [](bool ok, size_t n) { ... });

// Streams a GET response body to |file_path| (create-always) using
// SendStreaming + AsyncFile.  On HTTP 2xx with all bytes written,
// |on_done(true, bytes_written)| fires on the I/O thread; otherwise
// |on_done(false, ...)|.  |client| must outlive the transfer.
NEI_API void DownloadToFile(
    scoped_refptr<HttpClient> client,
    const HttpRequest &request,
    const net::IPEndPoint &endpoint,
    net::SSLContext *ssl_ctx,
    scoped_refptr<SingleThreadTaskRunner> io_runner,
    scoped_refptr<SequencedTaskRunner> background_runner,
    const std::filesystem::path &file_path,
    std::function<void(bool success, std::size_t bytes_written)> on_done);

// Streams |file_path| as the request body (SendBody pull provider).  The
// request must carry Content-Length (or Transfer-Encoding: chunked).  On
// completion, |on_done(true, response)| fires on the I/O thread when the
// response is a 2xx, otherwise |on_done(false, response)| (response may be
// null on transport failure).  |client| must outlive the transfer.
NEI_API void UploadFromFile(
    scoped_refptr<HttpClient> client,
    const HttpRequest &request,
    const net::IPEndPoint &endpoint,
    net::SSLContext *ssl_ctx,
    scoped_refptr<SingleThreadTaskRunner> io_runner,
    scoped_refptr<SequencedTaskRunner> background_runner,
    const std::filesystem::path &file_path,
    std::function<void(bool success, std::unique_ptr<HttpResponse> response)> on_done);

} // namespace net::http
} // namespace nei

#endif // NEIXX_NET_HTTP_HTTP_FILE_TRANSFER_H_
