// =============================================================================
// http_downloader_example — HTTP/HTTPS downloader built on libnei facilities
// =============================================================================
//
// Usage:
//   http_downloader_example [-L] [-O <output_file>] <url>
//
//   <url>        URL to download (scheme must be http:// or https://).
//   -L           Follow HTTP redirects (3xx + Location header), up to 10 hops.
//   -O <file>    Write the response body to <file>.  Accepts both "-O <file>"
//                (space-separated) and "-O=<file>".  Without -O the body is
//                printed to stdout.
//
// Flow:
//   1. Parse the command line via CommandLine.
//   2. Resolve the URL host via HostResolver (c-ares, background worker).
//   3. Send an HTTP GET via HttpClient on the global IO thread.
//   4. On 3xx + Location with -L: re-resolve + re-send.  A fresh HttpClient is
//      used per hop because keep-alive reuse must never cross hosts.
//   5. Print the body to stdout or write it to the -O file.

#include <cstdint>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include <neixx/command_line/command_line.h>
#include <neixx/common/at_exit.h>
#include <neixx/io/io_thread.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/address_list.h>
#include <neixx/net/host_resolver.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_pool_instance.h>
#include <neixx/url/url.h>

using nei::scoped_refptr;

namespace {

// ===========================================================================
// Downloader — async state machine driven entirely on the IO thread.
// ===========================================================================
class Downloader : public nei::RefCountedThreadSafe<Downloader> {
public:
  Downloader(std::string url, bool follow_redirects, std::string output_path)
      : url_str(std::move(url)),
        follow_redirects(follow_redirects),
        output_path(std::move(output_path)) {}

  std::string url_str;
  bool follow_redirects = false;
  std::string output_path;
  int redirect_depth = 0;
  static constexpr int kMaxRedirects = 10;

  bool succeeded = false;
  bool failed = false;
  nei::WaitableEvent done{nei::WaitableEvent::ResetPolicy::kAutomatic, false};

  scoped_refptr<nei::net::http::HttpClient> client; // Current hop client.
  std::unique_ptr<nei::net::SSLContext> ssl_ctx;    // Active during an https hop.
  std::shared_ptr<nei::net::HostResolver> resolver; // Active during a resolve.
  nei::Url current_url;

  // Performs one fetch hop (parse → resolve → send).  Runs on |io_runner|.
  void FetchHop(scoped_refptr<nei::SingleThreadTaskRunner> io_runner) {
    nei::Url url(url_str);
    if (!url.is_valid()) {
      Fail("invalid URL: " + url_str);
      return;
    }
    if (url.scheme() != "http" && url.scheme() != "https") {
      Fail("unsupported scheme \"" + std::string(url.scheme()) + "\" (only http/https)");
      return;
    }
    current_url = url;

    const bool is_https = url.scheme() == "https";
    const uint16_t port = url.port(); // Default port when omitted.
    const std::string host(url.host());

    std::cerr << "[download] GET " << url.spec() << std::endl;

    auto self = scoped_refptr<Downloader>(this);
    resolver = std::make_shared<nei::net::HostResolver>();
    resolver->Resolve(
        host,
        [self, io_runner, url, host, is_https, port](const nei::net::AddressList &addresses) {
          if (addresses.empty()) {
            self->Fail("DNS resolution failed: " + host);
            return;
          }
          const nei::net::IPEndPoint &first = addresses.front();
          const nei::net::IPEndPoint endpoint(first.address(), port);
          std::cerr << "[download] resolved " << host << " -> " << first.ToString() << std::endl;

          if (is_https) {
            // Fresh SSLContext per hop (hostname drives SNI).  libnei bundles
            // no CA store, so peer verification is disabled in this example.
            auto ctx = std::make_unique<nei::net::SSLContext>(nei::net::SSLContext::Mode::Client);
            ctx->SetHostname(host);
            ctx->SetPeerVerify(nei::net::PeerVerify::kNone);
            self->ssl_ctx = std::move(ctx);
          } else {
            self->ssl_ctx.reset();
          }

          // HttpClient serializes the request line from url.path(); a URL
          // without an explicit path ("https://host" or "https://host?q=1")
          // yields an empty path, which would serialize as "GET  HTTP/1.1".
          // Normalize to "/", inserting it before any query/fragment.
          std::string request_spec = url.spec();
          if (url.path().empty()) {
            std::size_t insert_at = request_spec.size();
            std::size_t q = request_spec.find('?');
            std::size_t f = request_spec.find('#');
            if (q != std::string::npos) {
              insert_at = q;
            }
            if (f != std::string::npos && f < insert_at) {
              insert_at = f;
            }
            request_spec.insert(insert_at, "/");
          }

          nei::net::http::HttpRequest req;
          req.method = nei::net::http::HttpMethod::kGet;
          req.url = nei::Url(request_spec);
          req.http_version = nei::net::http::HttpVersion::kHttp11;

          // Host header: include the port only when it is non-default.
          std::string host_header = host;
          const uint16_t default_port = is_https ? 443U : 80U;
          if (port != default_port) {
            host_header += ":" + std::to_string(port);
          }
          req.headers.push_back({"Host", host_header});
          req.headers.push_back({"User-Agent", "libnei-http-downloader/1.0"});
          req.headers.push_back({"Accept", "*/*"});
          req.headers.push_back({"Connection", "close"});

          auto hop_client = scoped_refptr<nei::net::http::HttpClient>(new nei::net::http::HttpClient());
          self->client = hop_client;
          hop_client->Send(
              req, endpoint, self->ssl_ctx.get(), io_runner,
              [self, io_runner](std::unique_ptr<nei::net::http::HttpResponse> resp) {
                self->OnResponse(std::move(resp), io_runner);
              });
        },
        io_runner);
  }

private:
  void OnResponse(std::unique_ptr<nei::net::http::HttpResponse> resp,
                  scoped_refptr<nei::SingleThreadTaskRunner> io_runner) {
    if (!resp) {
      Fail("HTTP request failed (connect or parse error)");
      return;
    }

    const int code = resp->status.raw_code();
    const bool is_redirect = code >= 300 && code < 400;

    if (is_redirect && follow_redirects && redirect_depth < kMaxRedirects) {
      const std::string location(resp->GetHeaderValue("Location"));
      if (!location.empty()) {
        const nei::Url next = current_url.Resolve(location);
        ++redirect_depth;
        std::cerr << "[redirect] " << code << " -> " << next.spec() << std::endl;
        // Fresh client for the next hop: keep-alive reuse must never write to
        // the previous host's connection.  Releasing it closes the old socket.
        client.reset();
        ssl_ctx.reset();
        url_str = next.spec();
        FetchHop(io_runner);
        return;
      }
    }

    if (is_redirect) {
      std::cerr << "[download] " << code << " redirect (use -L to follow)" << std::endl;
    } else if (code < 200 || code >= 300) {
      std::cerr << "[download] HTTP " << code << std::endl;
    } else {
      std::cerr << "[download] HTTP " << code << ", " << resp->body.size() << " bytes" << std::endl;
    }

    succeeded = code >= 200 && code < 300;

    if (output_path.empty()) {
      // Print the body to stdout.
      std::cout.write(resp->body.data(), static_cast<std::streamsize>(resp->body.size()));
      std::cout.flush();
    } else {
      std::ofstream ofs(output_path, std::ios::binary | std::ios::trunc);
      if (ofs) {
        ofs.write(resp->body.data(), static_cast<std::streamsize>(resp->body.size()));
        std::cerr << "[download] saved " << resp->body.size() << " bytes to " << output_path << std::endl;
      } else {
        std::cerr << "ERROR: cannot open output file: " << output_path << std::endl;
        failed = true;
      }
    }

    if (client) {
      client->Close();
    }
    done.Signal();
  }

  void Fail(const std::string &message) {
    std::cerr << "ERROR: " << message << std::endl;
    failed = true;
    done.Signal();
  }
};

} // namespace

int main(int argc, char *argv[]) {
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  // Emit raw bytes to the console (no \n translation for binary output).
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  nei::AtExitManager at_exit;

  // Parse the command line via CommandLine.
#if defined(_WIN32)
  nei::CommandLine::Init(); // From GetCommandLineW().
#else
  nei::CommandLine::Init(argc, argv);
#endif
  nei::CommandLine &cl = nei::CommandLine::ForCurrentProcess();

  const bool follow_redirects = cl.HasSwitch("L");

  // -O output file.  Supports both "-O=<file>" (CommandLine-native) and
  // "-O <file>" (the value is left as a positional argument).
  const bool has_output_switch = cl.HasSwitch("O");
  std::string output_path = cl.GetSwitchValueASCII("O");

  // Identify the URL positionally (must start with http:// or https://); any
  // other positional argument is the bare "-O <file>" candidate.
  std::string url;
  std::string bare_output_file;
  for (const std::string &arg : cl.GetArgs()) {
    if (arg.rfind("http://", 0) == 0 || arg.rfind("https://", 0) == 0) {
      url = arg;
    } else if (bare_output_file.empty()) {
      bare_output_file = arg;
    }
  }
  if (has_output_switch && output_path.empty()) {
    output_path = std::move(bare_output_file);
  }

  if (url.empty()) {
    std::cerr << "Usage: " << argv[0] << " [-L] [-O <output_file>] <url>" << std::endl;
    return 2;
  }

  nei::IOThread::Start();
  // HostResolver needs a background thread pool for blocking DNS lookups.
  nei::ThreadPoolInstance::CreateAndStart(nei::ThreadPoolInstance::InitParams{});
  auto io_runner = nei::GetGlobalIOTaskRunner();

  auto downloader = nei::MakeRefCounted<Downloader>(url, follow_redirects, output_path);
  downloader->FetchHop(io_runner);

  if (!downloader->done.TimedWait(std::chrono::seconds(60))) {
    std::cerr << "ERROR: timed out after 60s" << std::endl;
    nei::ThreadPoolInstance::Shutdown();
    nei::IOThread::Shutdown();
    return 1;
  }

  const bool ok = downloader->succeeded && !downloader->failed;

  nei::ThreadPoolInstance::Shutdown();
  nei::IOThread::Shutdown();
  return ok ? 0 : 1;
}
