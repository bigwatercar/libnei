#pragma once

#ifndef NEIXX_NET_HTTP_HTTP_PARSER_H_
#define NEIXX_NET_HTTP_HTTP_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>

// Forward-declare llhttp types (defined in 3rdparty/llhttp/include/llhttp.h).
// Consumers do NOT need to include llhttp.h directly.
struct llhttp__internal_s;
typedef llhttp__internal_s llhttp_t;
struct llhttp_settings_s;
typedef llhttp_settings_s llhttp_settings_t;

namespace nei::net::http {

// =============================================================================
// Http1Parser — C++ wrapper around llhttp for HTTP/1.x message parsing
// =============================================================================
//
// Http1Parser wraps the Node.js llhttp C parser and provides a callback-based
// interface for incremental HTTP message parsing.  It supports both request
// and response parsing.
//
// Usage (parsing a request):
//   Http1Parser parser(Http1Parser::Type::kRequest);
//   parser.SetDelegate(&my_delegate);
//   while (more_data) {
//       int64_t consumed = parser.Execute(data, len);
//       if (consumed < 0) {
//           // Parse error — check parser.error_message()
//           break;
//       }
//       data += consumed; len -= consumed;
//       if (parser.is_message_complete()) {
//           // Message fully parsed — callbacks already invoked
//           parser.Reset();  // Reset for next message on same connection
//       }
//   }
//
// Thread safety: Http1Parser is NOT thread-safe.  It should be used from a
// single thread (typically the I/O thread for a given connection).
//
// Pipelining: multiple HTTP messages can be parsed on the same connection by
// calling Reset() after each complete message.

NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
class NEI_API Http1Parser {
public:
    // -----------------------------------------------------------------------
    // Parser type
    // -----------------------------------------------------------------------
    enum class Type : uint8_t {
        kRequest = 1,   // Parse an HTTP request (method, URL, headers, body)
        kResponse = 2,  // Parse an HTTP response (status, headers, body)
    };

    // -----------------------------------------------------------------------
    // Delegate — receives parsed message components as callbacks
    // -----------------------------------------------------------------------
    //
    // All callbacks are invoked from within Execute().  Return 0 to continue;
    // return a negative value to abort parsing (the error will be surfaced
    // in Execute()'s return value).
    //
    // For requests: OnUrl → OnHeaderField/OnHeaderValue pairs →
    //   OnHeadersComplete → OnBody (0+) → OnMessageComplete
    //
    // For responses: OnStatus → OnHeaderField/OnHeaderValue pairs →
    //   OnHeadersComplete → OnBody (0+) → OnMessageComplete
    class NEI_API Delegate {
    public:
        virtual ~Delegate() = default;

        // Called when a new message begins.
        virtual void OnMessageBegin() {}

        // -- Request-specific callbacks --

        // Called with the HTTP method (e.g. "GET").  Only for requests.
        virtual void OnMethod(const char* data, size_t length) { (void)data; (void)length; }

        // Called with the raw URL string.  Only for requests.
        virtual void OnUrl(const char* data, size_t length) { (void)data; (void)length; }

        // -- Response-specific callbacks --

        // Called with the status code string (e.g. "200").  Only for responses.
        virtual void OnStatus(const char* data, size_t length) { (void)data; (void)length; }

        // -- Common callbacks --

        // Called with the HTTP version string (e.g. "1.1").
        virtual void OnHttpVersion(const char* data, size_t length) { (void)data; (void)length; }

        // Called for each header field name (e.g. "Content-Type").
        virtual void OnHeaderField(const char* data, size_t length) { (void)data; (void)length; }

        // Called for each header field value (e.g. "text/html").
        virtual void OnHeaderValue(const char* data, size_t length) { (void)data; (void)length; }

        // Called when all headers have been parsed.  Return a non-zero value
        // to skip body parsing (e.g. for HEAD requests).
        virtual int OnHeadersComplete() { return 0; }

        // Called for each body chunk (may be called multiple times for large
        // bodies, or zero times for body-less messages).
        virtual void OnBody(const char* data, size_t length) { (void)data; (void)length; }

        // Called when the complete message has been parsed.
        virtual void OnMessageComplete() {}

        // Called once per parsed chunk-extension name (for Trailer headers, etc.)
        virtual void OnChunkExtensionName(const char* data, size_t length) { (void)data; (void)length; }

        // Called once per parsed chunk-extension value.
        virtual void OnChunkExtensionValue(const char* data, size_t length) { (void)data; (void)length; }
    };

    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------
    explicit Http1Parser(Type type);
    ~Http1Parser();

    // Non-copyable, non-movable (owns llhttp internal state).
    Http1Parser(const Http1Parser&) = delete;
    Http1Parser& operator=(const Http1Parser&) = delete;
    Http1Parser(Http1Parser&&) = delete;
    Http1Parser& operator=(Http1Parser&&) = delete;

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    // Set the delegate that receives parsing callbacks.  Must be set before
    // calling Execute().  The delegate must outlive the parser.
    void SetDelegate(Delegate* delegate);

    // Enable lenient parsing mode (tolerates minor spec violations).
    void SetLenient(bool lenient);

    // -----------------------------------------------------------------------
    // Parsing
    // -----------------------------------------------------------------------

    // Feed data to the parser.  Returns the number of bytes consumed, or a
    // negative value on error.  On error, check error_message().
    //
    // The parser may consume fewer bytes than provided if a message boundary
    // is reached.  In that case, is_message_complete() returns true and the
    // caller should handle the complete message before feeding more data.
    //
    // Callback methods on the delegate are invoked synchronously from within
    // this call.
    int64_t Execute(const char* data, size_t length);

    // Reset the parser state for a new message on the same connection.
    // Preserves the delegate and type.
    void Reset();

    // -----------------------------------------------------------------------
    // State queries
    // -----------------------------------------------------------------------

    // Returns true when a complete HTTP message has been parsed.
    bool is_message_complete() const;

    // Returns true if a parse error has occurred.
    bool has_error() const;

    // Human-readable error message for the last error.
    const std::string& error_message() const;

    // Returns true if the current message has an Upgrade header (e.g.
    // WebSocket upgrade).
    bool is_upgrade() const;

    // Returns true if the parser is currently paused (HPE_PAUSED).
    bool is_paused() const;

    // Resume a paused parser.  Pausing occurs when the delegate returns
    // HPE_PAUSED from a callback.
    void Resume();

    // -----------------------------------------------------------------------
    // Parsed value accessors (valid during/after delegate callbacks)
    // -----------------------------------------------------------------------

    // For responses: the numeric HTTP status code (e.g. 200, 404).
    // Valid after on_status_complete / on_headers_complete.
    uint16_t status_code() const;

    // HTTP major version (e.g. 1 for HTTP/1.1).
    uint8_t http_major() const;

    // HTTP minor version (e.g. 1 for HTTP/1.1).
    uint8_t http_minor() const;

    // Whether the parsed message has keep-alive semantics.
    bool should_keep_alive() const;

private:
    // Static C callback trampolines → forward to Delegate via data pointer.
    static int OnMessageBeginCb(llhttp_t* p);
    static int OnMethodCb(llhttp_t* p, const char* at, size_t len);
    static int OnUrlCb(llhttp_t* p, const char* at, size_t len);
    static int OnStatusCb(llhttp_t* p, const char* at, size_t len);
    static int OnHttpVersionCb(llhttp_t* p, const char* at, size_t len);
    static int OnHeaderFieldCb(llhttp_t* p, const char* at, size_t len);
    static int OnHeaderValueCb(llhttp_t* p, const char* at, size_t len);
    static int OnHeadersCompleteCb(llhttp_t* p);
    static int OnBodyCb(llhttp_t* p, const char* at, size_t len);
    static int OnMessageCompleteCb(llhttp_t* p);
    static int OnChunkExtensionNameCb(llhttp_t* p, const char* at, size_t len);
    static int OnChunkExtensionValueCb(llhttp_t* p, const char* at, size_t len);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
NEI_SUPPRESS_MSC_WARNING_4251_END

}  // namespace nei::net::http

#endif  // NEIXX_NET_HTTP_HTTP_PARSER_H_
