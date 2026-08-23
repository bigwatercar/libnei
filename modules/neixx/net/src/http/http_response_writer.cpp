#include <neixx/net/http/http_response_writer.h>

#include <cstdio>

namespace nei::net::http {

std::string HttpResponseWriter::Serialize(const HttpResponse &response) {
  std::string result = SerializeHeaders(response);

  // Check if chunked Transfer-Encoding is requested.
  bool is_chunked = false;
  for (const auto &h : response.headers) {
    std::string lower = h.name;
    for (char &c : lower)
      if (c >= 'A' && c <= 'Z')
        c = static_cast<char>(c + 32);
    if (lower == "transfer-encoding") {
      std::string val = h.value;
      for (char &c : val)
        if (c >= 'A' && c <= 'Z')
          c = static_cast<char>(c + 32);
      if (val.find("chunked") != std::string::npos)
        is_chunked = true;
      break;
    }
  }

  if (is_chunked) {
    if (!response.body.empty()) {
      result += SerializeChunk(response.body.data(), response.body.size());
    }
    result += SerializeLastChunk();
  } else if (!response.body.empty()) {
    result += response.body;
  }

  return result;
}

std::string HttpResponseWriter::SerializeHeaders(const HttpResponse &response) {
  std::string result = SerializeStatusLine(response);

  // Build a mutable copy of headers so we can add Content-Length if needed.
  HttpHeaders headers = response.headers;
  bool has_content_length = false;
  bool has_transfer_encoding = false;

  for (const auto &h : headers) {
    // Case-insensitive check.
    std::string lower_name = h.name;
    for (char &c : lower_name) {
      if (c >= 'A' && c <= 'Z')
        c = static_cast<char>(c + 32);
    }
    if (lower_name == "content-length")
      has_content_length = true;
    if (lower_name == "transfer-encoding")
      has_transfer_encoding = true;
  }

  // Auto-add Content-Length when no Transfer-Encoding and no Content-Length
  // is already provided.  An empty body must still carry "Content-Length: 0"
  // (or be chunked) so a keep-alive client knows where the message ends;
  // otherwise it would wait for the connection to close (read-until-close).
  // Status codes that never carry a body (1xx, 204, 304) must NOT have a
  // Content-Length header at all (RFC 9110 §6.3 / §8.6).
  const int code = response.status.raw_code();
  const bool bodyless_status = (code >= 100 && code < 200) || code == 204 || code == 304;
  if (!bodyless_status && !has_transfer_encoding && !has_content_length) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%zu", response.body.size());
    headers.push_back(HttpHeader("Content-Length", buf));
  }

  // Write headers.
  for (const auto &h : headers) {
    result += h.name;
    result += ": ";
    result += h.value;
    result += "\r\n";
  }

  result += "\r\n"; // End of headers.
  return result;
}

std::string HttpResponseWriter::SerializeStatusLine(const HttpResponse &response) {
  // Determine HTTP version string.
  const char *version_str = "HTTP/1.1";
  if (response.http_version == HttpVersion::kHttp10) {
    version_str = "HTTP/1.0";
  }

  // Use the raw numeric code for the wire, fallback to the enum view.
  int code = response.status.raw_code();
  if (code < 100 || code > 999) {
    code = static_cast<int>(response.status.code());
  }

  // Status text from the enum or our lookup.
  const char *reason = HttpStatusCodeToString(response.status.code());

  char status_line[128];
  std::snprintf(status_line, sizeof(status_line), "%s %d %s\r\n", version_str, code, reason);
  return status_line;
}

std::string HttpResponseWriter::SerializeChunk(const void *data, size_t len) {
  std::string result = SerializeChunkHeader(len);
  if (len > 0) {
    result.append(static_cast<const char *>(data), len);
    result += "\r\n";
  }
  return result;
}

std::string HttpResponseWriter::SerializeChunkHeader(size_t len) {
  char hex_size[32];
  std::snprintf(hex_size, sizeof(hex_size), "%zx\r\n", len);
  return hex_size;
}

std::string HttpResponseWriter::SerializeLastChunk() {
  return "0\r\n\r\n";
}

} // namespace nei::net::http
