// =============================================================================
// multipart.h — multipart/form-data (RFC 7578) encoding and parsing
// =============================================================================
//
// Two complementary pieces for multipart/form-data:
//   - MultipartFormData: builds a request body (fields and files) with a
//     generated boundary; the caller sets "Content-Type: multipart/form-data;
//     boundary=<b>" on the request.
//   - ParseMultipartBody: splits a received multipart body into its parts
//     (used by server handlers on HttpRequest::body).
//
// Wire format (RFC 7578):
//   --<boundary>\r\n
//   Content-Disposition: form-data; name="field" [; filename="f"]\r\n
//   [Content-Type: ...]\r\n
//   \r\n
//   <part data>\r\n
//   ... more parts ...
//   --<boundary>--\r\n

#ifndef NEIXX_NET_HTTP_MULTIPART_H_
#define NEIXX_NET_HTTP_MULTIPART_H_

#include <cstddef>
#include <string>
#include <vector>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>

namespace nei::net::http {

// One parsed part of a multipart body.
struct NEI_API MultipartPart {
  // Field name (from Content-Disposition name="...").
  std::string name;
  // Original filename when this part is a file upload (else empty).
  std::string filename;
  // Content-Type of the part (empty when not specified).
  std::string content_type;
  // Part payload bytes.
  std::string data;
};

NEI_SUPPRESS_MSC_WARNING_4251_BEGIN

// Incremental builder for a multipart/form-data request body.
class NEI_API MultipartFormData {
public:
  MultipartFormData();
  ~MultipartFormData();

  MultipartFormData(const MultipartFormData &) = delete;
  MultipartFormData &operator=(const MultipartFormData &) = delete;

  // Adds a plain form field.
  void AddField(std::string name, std::string value);

  // Adds a file part.  |content_type| may be empty (then no Content-Type
  // header is emitted for the part).
  void AddFile(std::string name, std::string filename, std::string content_type, std::string data);

  // Returns the fully serialized body.  Call after all parts are added.
  std::string GetBody() const;

  // The boundary used (set "Content-Type: multipart/form-data; boundary=..."
  // with this value on the request).
  const std::string &GetBoundary() const {
    return boundary_;
  }

private:
  std::string boundary_;
  std::vector<std::pair<std::string, std::string>> pending_headers_; // part headers
  std::vector<std::string> pending_payloads_;                        // part payloads
};

NEI_SUPPRESS_MSC_WARNING_4251_END

// Parses |body| (a multipart/form-data payload received from the wire) into
// |parts| using |boundary| (without leading "--").  Returns false on
// malformed input (missing initial/final boundary).  Existing parts are
// appended.
NEI_API bool
ParseMultipartBody(const std::string &body, const std::string &boundary, std::vector<MultipartPart> *parts);

} // namespace nei::net::http

#endif // NEIXX_NET_HTTP_MULTIPART_H_
