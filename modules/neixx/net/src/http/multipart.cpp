// =============================================================================
// multipart.cpp — multipart/form-data (RFC 7578) encoding and parsing
// =============================================================================

#include <neixx/net/http/multipart.h>

#include <cstdio>
#include <cstring>
#include <random>

namespace nei::net::http {

namespace {

constexpr std::string_view kCrLf = "\r\n";

std::string GenerateBoundary() {
  // 16 random hex bytes prefixed with a stable marker.
  std::random_device rd;
  std::mt19937_64 gen(rd());
  char buf[40];
  std::snprintf(buf,
                sizeof(buf),
                "----libneiFormBoundary%016llx%016llx",
                static_cast<unsigned long long>(gen()),
                static_cast<unsigned long long>(gen()));
  return buf;
}

// Parses "Content-Disposition: form-data; name="x"; filename="y"" into
// |name| and |filename|.  Tokens without '=' (e.g. "form-data") are skipped.
void ParseContentDisposition(std::string_view value, std::string *name, std::string *filename) {
  name->clear();
  filename->clear();
  std::size_t pos = 0;
  while (pos < value.size()) {
    // Skip ';' and surrounding whitespace to the start of the next token.
    while (pos < value.size() && (value[pos] == ';' || value[pos] == ' ' || value[pos] == '\t'))
      ++pos;
    const std::size_t semi = value.find(';', pos);
    std::string_view token = semi == std::string_view::npos ? value.substr(pos) : value.substr(pos, semi - pos);
    pos = semi == std::string_view::npos ? value.size() : semi + 1;
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
      token = token.substr(0, token.size() - 1);

    const std::size_t eq = token.find('=');
    if (eq == std::string_view::npos)
      continue; // e.g. "form-data"
    const std::string_view key = token.substr(0, eq);
    std::string_view val = token.substr(eq + 1);
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
      val = val.substr(1, val.size() - 2);
    if (key == "name") {
      *name = std::string(val);
    } else if (key == "filename") {
      *filename = std::string(val);
    }
  }
}

} // namespace

// ---------------------------------------------------------------------------
// MultipartFormData
// ---------------------------------------------------------------------------
MultipartFormData::MultipartFormData()
    : boundary_(GenerateBoundary()) {
}

MultipartFormData::~MultipartFormData() = default;

void MultipartFormData::AddField(std::string name, std::string value) {
  pending_headers_.emplace_back("Content-Disposition: form-data; name=\"" + name + "\"", std::string());
  pending_payloads_.push_back(std::move(value));
}

void MultipartFormData::AddFile(std::string name, std::string filename, std::string content_type, std::string data) {
  std::string headers = "Content-Disposition: form-data; name=\"" + name + "\"; filename=\"" + filename + "\"";
  if (!content_type.empty())
    headers += std::string(kCrLf) + "Content-Type: " + content_type;
  pending_headers_.emplace_back(std::move(headers), std::string());
  pending_payloads_.push_back(std::move(data));
}

std::string MultipartFormData::GetBody() const {
  std::string body;
  for (std::size_t i = 0; i < pending_payloads_.size(); ++i) {
    body += "--";
    body += boundary_;
    body += kCrLf;
    body += pending_headers_[i].first;
    body += kCrLf;
    body += kCrLf;
    body += pending_payloads_[i];
    body += kCrLf;
  }
  body += "--";
  body += boundary_;
  body += "--";
  body += kCrLf;
  return body;
}

// ---------------------------------------------------------------------------
// ParseMultipartBody
// ---------------------------------------------------------------------------
bool ParseMultipartBody(const std::string &body, const std::string &boundary, std::vector<MultipartPart> *parts) {
  if (!parts)
    return false;

  const std::string delim = "--" + boundary;
  // Find the first delimiter.
  std::size_t pos = body.find(delim);
  if (pos == std::string::npos)
    return false;
  // Skip the delimiter itself and the trailing CRLF (or "--" for a final
  // empty body).
  pos += delim.size();
  if (pos < body.size() && body.compare(pos, 2, "--") == 0)
    return true; // No parts; empty body.
  if (pos + 2 <= body.size() && body.compare(pos, 2, kCrLf) == 0)
    pos += 2;
  else if (pos < body.size() && body[pos] == '\n')
    ++pos; // tolerate bare LF

  while (pos < body.size()) {
    // Locate the next delimiter.
    const std::size_t next = body.find(delim, pos);
    if (next == std::string::npos)
      return false; // malformed: missing closing boundary

    const std::string_view block(body.data() + pos, next - pos);
    // A block is "headers\r\n\r\npayload".  The final delimiter may be
    // followed by "--".
    const bool is_final = next + delim.size() < body.size() && body.compare(next + delim.size(), 2, "--") == 0;

    // Split headers from payload.
    std::string_view headers = block;
    std::string_view payload = block;
    const std::size_t hdr_end = block.find("\r\n\r\n");
    if (hdr_end != std::string_view::npos) {
      headers = block.substr(0, hdr_end);
      payload = block.substr(hdr_end + 4);
    } else {
      // Fallback: bare LF separator.
      const std::size_t lf = block.find("\n\n");
      if (lf != std::string_view::npos) {
        headers = block.substr(0, lf);
        payload = block.substr(lf + 2);
      }
    }
    // Payload is followed by CRLF before the next delimiter — strip one.
    if (payload.size() >= 2 && payload.compare(payload.size() - 2, 2, "\r\n") == 0)
      payload = payload.substr(0, payload.size() - 2);
    else if (!payload.empty() && payload.back() == '\n')
      payload = payload.substr(0, payload.size() - 1);

    MultipartPart part;
    // Parse headers.
    std::string_view hdr = headers;
    while (!hdr.empty()) {
      std::string_view line = hdr;
      const std::size_t nl = hdr.find('\n');
      if (nl != std::string_view::npos) {
        line = hdr.substr(0, nl);
        hdr = hdr.substr(nl + 1);
        if (!line.empty() && line.back() == '\r')
          line = line.substr(0, line.size() - 1);
      } else {
        hdr = std::string_view();
      }
      if (line.size() >= 20 && line.compare(0, 20, "Content-Disposition:") == 0) {
        ParseContentDisposition(line.substr(20), &part.name, &part.filename);
      } else if (line.size() >= 13 && line.compare(0, 13, "Content-Type:") == 0) {
        part.content_type = std::string(line.substr(13));
        // Trim leading spaces.
        while (!part.content_type.empty() && part.content_type.front() == ' ')
          part.content_type.erase(0, 1);
      }
    }
    part.data = std::string(payload);
    parts->push_back(std::move(part));

    if (is_final)
      return true;
    pos = next + delim.size();
    if (pos + 2 <= body.size() && body.compare(pos, 2, "\r\n") == 0)
      pos += 2;
    else if (pos < body.size() && body[pos] == '\n')
      ++pos;
  }
  return false;
}

} // namespace nei::net::http
