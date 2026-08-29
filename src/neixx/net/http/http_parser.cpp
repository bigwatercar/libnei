// Http1Parser — C++ wrapper around llhttp C HTTP/1.x message parser.

#include <neixx/net/http/http_parser.h>

#include <cstring>

// Pull in the real llhttp definitions.
extern "C" {
#include <llhttp.h>
}

namespace nei::net::http {

// ===========================================================================
// Http1Parser::Impl — PIMPL
// ===========================================================================
struct Http1Parser::Impl {
  llhttp_t parser;
  llhttp_settings_t settings;
  Type type;
  Delegate *delegate = nullptr;
  bool message_complete = false;
  std::string error_msg;
  bool paused = false;

  Impl(Type t)
      : type(t) {
    std::memset(&parser, 0, sizeof(parser));
    std::memset(&settings, 0, sizeof(settings));
    SetupSettings();
    llhttp_init(&parser, (type == Type::kRequest) ? HTTP_REQUEST : HTTP_RESPONSE, &settings);
    // Store 'this' as user data for static callback trampolines.
    parser.data = this;
  }

  void SetupSettings() {
    settings.on_message_begin = Http1Parser::OnMessageBeginCb;

    if (type == Type::kRequest) {
      settings.on_method = Http1Parser::OnMethodCb;
      settings.on_url = Http1Parser::OnUrlCb;
    } else {
      settings.on_status = Http1Parser::OnStatusCb;
    }

    settings.on_version = Http1Parser::OnHttpVersionCb;
    settings.on_header_field = Http1Parser::OnHeaderFieldCb;
    settings.on_header_value = Http1Parser::OnHeaderValueCb;
    settings.on_headers_complete = Http1Parser::OnHeadersCompleteCb;
    settings.on_body = Http1Parser::OnBodyCb;
    settings.on_message_complete = Http1Parser::OnMessageCompleteCb;

    // Chunk extension callbacks (for Trailer fields in chunked encoding).
    settings.on_chunk_extension_name = Http1Parser::OnChunkExtensionNameCb;
    settings.on_chunk_extension_value = Http1Parser::OnChunkExtensionValueCb;
  }

  void Reinit() {
    message_complete = false;
    error_msg.clear();
    paused = false;
    llhttp_init(&parser, (type == Type::kRequest) ? HTTP_REQUEST : HTTP_RESPONSE, &settings);
    parser.data = this;
  }
};

// ===========================================================================
// Http1Parser
// ===========================================================================

Http1Parser::Http1Parser(Type type)
    : impl_(std::make_unique<Impl>(type)) {
}

Http1Parser::~Http1Parser() = default;

void Http1Parser::SetDelegate(Delegate *delegate) {
  impl_->delegate = delegate;
}

void Http1Parser::SetLenient(bool lenient) {
  if (lenient) {
    impl_->parser.lenient_flags |= static_cast<uint16_t>(LENIENT_HEADERS | LENIENT_CHUNKED_LENGTH | LENIENT_KEEP_ALIVE
                                                         | LENIENT_TRANSFER_ENCODING | LENIENT_VERSION);
  } else {
    impl_->parser.lenient_flags = 0;
  }
}

int64_t Http1Parser::Execute(const char *data, size_t length) {
  if (!impl_->delegate) {
    impl_->error_msg = "No delegate set";
    return -1;
  }

  llhttp_errno_t err = llhttp_execute(&impl_->parser, data, length);

  if (err == HPE_OK) {
    // All data consumed; return full length.  If message is complete,
    // is_message_complete() was already set by OnMessageCompleteCb.
    return static_cast<int64_t>(length);
  }

  if (err == HPE_PAUSED || err == HPE_PAUSED_UPGRADE || err == HPE_PAUSED_H2_UPGRADE) {
    impl_->paused = true;
    // llhttp_execute pauses at the first unconsumed byte; re-derive the
    // consumed count via llhttp_get_error_pos.  For upgrade messages the
    // pause fires after the ENTIRE header block, so error_pos may point
    // one past the input buffer (data + length) — report the full length
    // in that case.  Reporting 0 there made callers re-scan the request
    // bytes (e.g. as WebSocket frames after an upgrade), corrupting the
    // upgraded-protocol stream.
    const char *error_pos = llhttp_get_error_pos(&impl_->parser);
    if (error_pos && error_pos >= data) {
      const std::size_t consumed = static_cast<std::size_t>(error_pos - data);
      return consumed <= length ? static_cast<int64_t>(consumed) : static_cast<int64_t>(length);
    }
    return 0; // No data consumed.
  }

  // Error path.
  impl_->error_msg =
      std::string(llhttp_errno_name(err)) + ": "
      + (llhttp_get_error_reason(&impl_->parser) ? llhttp_get_error_reason(&impl_->parser) : "unknown error");
  return -1;
}

void Http1Parser::Reset() {
  impl_->Reinit();
}

bool Http1Parser::is_message_complete() const {
  return impl_->message_complete;
}

bool Http1Parser::has_error() const {
  return !impl_->error_msg.empty();
}

const std::string &Http1Parser::error_message() const {
  return impl_->error_msg;
}

bool Http1Parser::is_upgrade() const {
  return (impl_->parser.upgrade != 0);
}

bool Http1Parser::is_paused() const {
  return impl_->paused;
}

void Http1Parser::Resume() {
  if (impl_->paused) {
    impl_->paused = false;
    llhttp_resume(&impl_->parser);
  }
}

uint16_t Http1Parser::status_code() const {
  return impl_->parser.status_code;
}

uint8_t Http1Parser::http_major() const {
  return impl_->parser.http_major;
}

uint8_t Http1Parser::http_minor() const {
  return impl_->parser.http_minor;
}

bool Http1Parser::should_keep_alive() const {
  return llhttp_should_keep_alive(&impl_->parser) != 0;
}

// ===========================================================================
// Static callback trampolines — shim from llhttp C callbacks to C++ Delegate
// ===========================================================================

#define PARSER_FROM_LLHTTP(p) static_cast<Http1Parser::Impl *>(static_cast<llhttp__internal_t *>(p)->data)

int Http1Parser::OnMessageBeginCb(llhttp_t *p) {
  auto *impl = PARSER_FROM_LLHTTP(p);
  if (impl->delegate)
    impl->delegate->OnMessageBegin();
  return 0;
}

int Http1Parser::OnMethodCb(llhttp_t *p, const char *at, size_t len) {
  auto *impl = PARSER_FROM_LLHTTP(p);
  if (impl->delegate)
    impl->delegate->OnMethod(at, len);
  return 0;
}

int Http1Parser::OnUrlCb(llhttp_t *p, const char *at, size_t len) {
  auto *impl = PARSER_FROM_LLHTTP(p);
  if (impl->delegate)
    impl->delegate->OnUrl(at, len);
  return 0;
}

int Http1Parser::OnStatusCb(llhttp_t *p, const char *at, size_t len) {
  auto *impl = PARSER_FROM_LLHTTP(p);
  if (impl->delegate)
    impl->delegate->OnStatus(at, len);
  return 0;
}

int Http1Parser::OnHttpVersionCb(llhttp_t *p, const char *at, size_t len) {
  auto *impl = PARSER_FROM_LLHTTP(p);
  if (impl->delegate)
    impl->delegate->OnHttpVersion(at, len);
  return 0;
}

int Http1Parser::OnHeaderFieldCb(llhttp_t *p, const char *at, size_t len) {
  auto *impl = PARSER_FROM_LLHTTP(p);
  if (impl->delegate)
    impl->delegate->OnHeaderField(at, len);
  return 0;
}

int Http1Parser::OnHeaderValueCb(llhttp_t *p, const char *at, size_t len) {
  auto *impl = PARSER_FROM_LLHTTP(p);
  if (impl->delegate)
    impl->delegate->OnHeaderValue(at, len);
  return 0;
}

int Http1Parser::OnHeadersCompleteCb(llhttp_t *p) {
  auto *impl = PARSER_FROM_LLHTTP(p);
  if (impl->delegate) {
    int ret = impl->delegate->OnHeadersComplete();
    if (ret != 0) {
      // Non-zero return → skip body (used for HEAD requests, etc.)
      return ret;
    }
  }
  return 0;
}

int Http1Parser::OnBodyCb(llhttp_t *p, const char *at, size_t len) {
  auto *impl = PARSER_FROM_LLHTTP(p);
  if (impl->delegate)
    impl->delegate->OnBody(at, len);
  return 0;
}

int Http1Parser::OnMessageCompleteCb(llhttp_t *p) {
  auto *impl = PARSER_FROM_LLHTTP(p);
  impl->message_complete = true;
  if (impl->delegate)
    impl->delegate->OnMessageComplete();
  return 0; // Continue parsing (for pipelining).
}

int Http1Parser::OnChunkExtensionNameCb(llhttp_t *p, const char *at, size_t len) {
  auto *impl = PARSER_FROM_LLHTTP(p);
  if (impl->delegate)
    impl->delegate->OnChunkExtensionName(at, len);
  return 0;
}

int Http1Parser::OnChunkExtensionValueCb(llhttp_t *p, const char *at, size_t len) {
  auto *impl = PARSER_FROM_LLHTTP(p);
  if (impl->delegate)
    impl->delegate->OnChunkExtensionValue(at, len);
  return 0;
}

#undef PARSER_FROM_LLHTTP

} // namespace nei::net::http
