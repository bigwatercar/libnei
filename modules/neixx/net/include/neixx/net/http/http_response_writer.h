#pragma once

#ifndef NEIXX_NET_HTTP_HTTP_RESPONSE_WRITER_H_
#define NEIXX_NET_HTTP_HTTP_RESPONSE_WRITER_H_

#include <string>

#include <nei/build/nei_export.h>
#include <neixx/net/http/http_response.h>

namespace nei::net::http {

// =============================================================================
// HttpResponseWriter — serialize HttpResponse to wire-format bytes
// =============================================================================
//
// Produces HTTP/1.1 response bytes suitable for writing directly to a socket.
// Supports Content-Length-based and chunked Transfer-Encoding.
//
// Basic usage (buffered):
//   HttpResponse resp;
//   resp.SetStatus(HttpStatusCode::kOk);
//   resp.headers.push_back({"Content-Type", "text/html"});
//   resp.body = "<h1>Hello</h1>";
//
//   std::string wire = HttpResponseWriter::Serialize(resp);
//   socket->WriteAsync(MakeIOBuffer(wire), wire.size(), callback);
//
// Streaming usage (chunked):
//   HttpResponse resp;
//   resp.SetStatus(HttpStatusCode::kOk);
//   resp.headers.push_back({"Transfer-Encoding", "chunked"});
//
//   std::string headers = SerializeHeaders(resp);   // no Content-Length
//   socket->WriteAsync(MakeIOBuffer(headers), ...);
//   socket->WriteAsync(MakeIOBuffer(SerializeChunk(chunk1)), ...);
//   socket->WriteAsync(MakeIOBuffer(SerializeChunk(chunk2)), ...);
//   socket->WriteAsync(MakeIOBuffer(SerializeLastChunk()), ...);

class NEI_API HttpResponseWriter {
public:
    // Serialize a complete HTTP response to bytes.
    // If Transfer-Encoding: chunked is set, wraps the body in chunk format
    // and appends the terminating "0\r\n\r\n".  Otherwise, writes the body
    // as-is (Content-Length mode).
    static std::string Serialize(const HttpResponse& response);

    // Serialize status-line + headers only (no body, no extra CRLF).
    // Useful as the first step of a streaming/chunked response.
    static std::string SerializeHeaders(const HttpResponse& response);

    // Format: "HTTP/1.x <code> <reason>\r\n"
    static std::string SerializeStatusLine(const HttpResponse& response);

    // ---- Chunked transfer encoding helpers ----

    // Wrap |data| in a single HTTP chunk: "<hex-size>\r\n<data>\r\n".
    // Returns only the hex-size line if |len| is 0.
    static std::string SerializeChunk(const void* data, size_t len);

    // Returns only the chunk size line: "<hex-size>\r\n".  Use with a
    // zero-copy body buffer to avoid the memcpy in SerializeChunk:
    //   write(SerializeChunkHeader(len));   // "<hex-size>\r\n"
    //   write_io(buf, len);                 // body bytes (no copy)
    //   write("\r\n");                      // chunk trailer
    static std::string SerializeChunkHeader(size_t len);

    // Returns the terminating chunk: "0\r\n\r\n".
    static std::string SerializeLastChunk();

private:
    HttpResponseWriter() = delete;
};

}  // namespace nei::net::http

#endif  // NEIXX_NET_HTTP_HTTP_RESPONSE_WRITER_H_
