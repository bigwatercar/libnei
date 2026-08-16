// =============================================================================
// http_client_example — 配合 http_echo_server_example 的 HTTP/WS 客户端
// =============================================================================
//
// 用法:
//   http_client_example [--h1|--h2|--no-alpn|--plain] [--ws] [--port <n>] [--plain-port <n>]
//
//   - 缺省      : TLS + ALPN {"h2","http/1.1"}（与服务器协商 h2）→ GET /echo
//   - --h1      : 仅 http/1.1（ALPN {"http/1.1"}）
//   - --h2      : 仅 h2（ALPN {"h2"}）
//   - --no-alpn : 无 ALPN 扩展（服务器按 h1 兜底；仅-h2 服务器会拒绝）
//   - --plain   : 明文 TCP（h1 引擎）→ http://127.0.0.1:<plain-port>/echo
//   - --ws      : WebSocket /ws 发送 "hello"，打印服务器回发的长度
//   - --port    : 服务器 TLS 端口，默认 9443
//   - --plain-port : 服务器明文端口，默认 8080
//
// 示例:
//   http_client_example                  # h2 协商，GET /echo
//   http_client_example --h1             # h1 强制
//   http_client_example --plain --ws     # 明文 ws echo
//   http_client_example --ws             # wss echo
// =============================================================================

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nei/log/log.h>
#include <neixx/command_line/command_line.h>
#include <neixx/common/at_exit.h>
#include <neixx/common/time.h>
#include <neixx/io/io_thread.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/websocket/websocket_client.h>
#include <neixx/net/websocket/websocket_frame.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>

using namespace nei;
using namespace nei::net;
using namespace nei::net::http;

namespace {

// 与 http_echo_server_example 相同的自签名证书（作为信任锚，CN=localhost）。
constexpr char kServerCertPem[] = "-----BEGIN CERTIFICATE-----\n"
                                  "MIIDCTCCAfGgAwIBAgIULXGW9eR+F+dX/BfCNv20XaxQ0sIwDQYJKoZIhvcNAQEL\n"
                                  "BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDgxNjA0NDc1M1oXDTM2MDgx\n"
                                  "MzA0NDc1M1owFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF\n"
                                  "AAOCAQ8AMIIBCgKCAQEAqAYYm87apQS2YtZOAnj7Gb3GxZ1RXem3ncRiLq2DFEsU\n"
                                  "jLC3l/XsJkPcq2kEv5n9z2mw3+yemwXnBBA5hvxBTy3tXwV4YnQfJjE0QwBv+ATC\n"
                                  "BLG4eyrJNYHp0XbCD+3MZvugaMiUWk0YaCdS+E5gCDTOh+ZRWliP2azkPtqcSCxn\n"
                                  "8n8H7FxNinCIB+CS8JZ/igIEYH3bi9WdSTLw/xa6LOVhq9x9ciUTDAFMAo4vEAWr\n"
                                  "7sb37T/ifMTaioi4sOcODlZKmrkNNSDk+py2a8NpW2PQUn+GjLeCQkXTLZ6f/szh\n"
                                  "2TJT+kX3a7zLkSRHpRbcEUZTvN4jV6EPFQoHOMiebwIDAQABo1MwUTAdBgNVHQ4E\n"
                                  "FgQUr4XH53EnpEzqkU6T7nTvWjK2jzwwHwYDVR0jBBgwFoAUr4XH53EnpEzqkU6T\n"
                                  "7nTvWjK2jzwwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAd4On\n"
                                  "3JpES0Yj16WN6svJTVM6pkhx/ukAGUjGpAuKy6xcnL5fc8T+soK5mmuxAJBdTHYx\n"
                                  "TFl2AygeZdlpfY9kDTC/fpultxsy2bxhFxHTJsaNwyxrrdGOx+bpqPTNmksi8tEl\n"
                                  "jEOJgqMxI7rNjvS07XoLFIgIdgef1jL3tId69Orx+j7Pt4SCndIyEpjeH2lMT6Io\n"
                                  "SsNY3gXBvvHKMsOq4FKuAWqb5QqFYQF97kbcP95Dht8rT6RE7v30EwUzKVZPbxs3\n"
                                  "V4WFUuWiYVkm5KVT/9zdeH0n18o0aPN/vohAh8BAriwJ+4NLl5X1WFi1CZ1mFCLE\n"
                                  "fzP8POlbSn52tJR4Hw==\n"
                                  "-----END CERTIFICATE-----\n";

enum class ClientMode {
  kAuto,   // {"h2","http/1.1"}
  kH1Only, // {"http/1.1"}
  kH2Only, // {"h2"}
  kNoAlpn, // 无 ALPN
  kPlain,  // 明文 TCP（无 TLS）
};

HttpRequest MakeGet() {
  HttpRequest req;
  req.method = HttpMethod::kGet;
  req.url = Url("https://localhost/echo");
  req.http_version = HttpVersion::kHttp11;
  req.headers.push_back({"Host", "localhost"});
  return req;
}

} // namespace

int main(int argc, char **argv) {
  AtExitManager at_exit;
  AtExitManager::RegisterCallback(&nei_log_shutdown);

  // 日志：stdout sink，经 nei_log_add_sink 注册并 update_config 发布。
  nei_log_sink_st *sink = nei_log_create_stdout_sink();
  if (sink) {
    nei_log_config_st *cfg = nei_log_default_config();
    cfg->level_flags.all = 0xffffffffU;
    cfg->verbose_threshold = 2;
    if (nei_log_add_sink(cfg, sink) != 0)
      nei_log_release_sink(sink);
    nei_log_update_config();
  }

#if defined(_WIN32)
  (void)argc;
  (void)argv;
  CommandLine::Init();
#else
  CommandLine::Init(argc, argv);
#endif
  CommandLine &cl = CommandLine::ForCurrentProcess();

  ClientMode mode = ClientMode::kAuto;
  if (cl.HasSwitch("h1"))
    mode = ClientMode::kH1Only;
  else if (cl.HasSwitch("h2"))
    mode = ClientMode::kH2Only;
  else if (cl.HasSwitch("no-alpn"))
    mode = ClientMode::kNoAlpn;
  else if (cl.HasSwitch("plain"))
    mode = ClientMode::kPlain;
  const bool use_ws = cl.HasSwitch("ws");

  int tls_port = 9443;
  int plain_port = 8080;
  if (!cl.GetSwitchValueASCII("port").empty())
    tls_port = std::atoi(cl.GetSwitchValueASCII("port").c_str());
  if (!cl.GetSwitchValueASCII("plain-port").empty())
    plain_port = std::atoi(cl.GetSwitchValueASCII("plain-port").c_str());

  // 客户端 TLS 上下文：信任示例服务器自签名证书（作为 CA），校验可选。
  auto ssl_ctx = std::make_shared<net::SSLContext>(net::SSLContext::Mode::Client);
  if (mode != ClientMode::kPlain) {
    ssl_ctx->SetPeerVerify(nei::net::PeerVerify::kOptional);
    if (!ssl_ctx->SetCAChain(kServerCertPem)) {
      NEI_LOG_ERROR("FAIL: SetCAChain");
      return 1;
    }
    switch (mode) {
    case ClientMode::kH1Only:
      ssl_ctx->SetAlpnProtocols({"http/1.1"});
      break;
    case ClientMode::kH2Only:
      ssl_ctx->SetAlpnProtocols({"h2"});
      break;
    case ClientMode::kNoAlpn:
      break; // 不设置 ALPN
    default:
      // WS 升级仅 h1 引擎支持：--ws 且未显式指定协议时以 http/1.1 连接。
      ssl_ctx->SetAlpnProtocols(use_ws ? std::vector<std::string>{"http/1.1"}
                                       : std::vector<std::string>{"h2", "http/1.1"});
      break;
    }
  }

  IOThread::Start();
  auto io_runner = GetGlobalIOTaskRunner();

  const uint16_t port = static_cast<uint16_t>(mode == ClientMode::kPlain ? plain_port : tls_port);
  net::SSLContext *ctx = (mode == ClientMode::kPlain) ? nullptr : ssl_ctx.get();
  IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), port);

  auto done = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto client = scoped_refptr<HttpClient>();
  auto ws = scoped_refptr<net::websocket::WebSocketClient>();

  if (use_ws) {
    // WebSocket：连接 /ws，升级完成后发送 "hello"，服务器回发帧长度。
    ws = scoped_refptr<net::websocket::WebSocketClient>(new net::websocket::WebSocketClient());
    auto echoed = std::make_shared<std::atomic<bool>>(false);

    io_runner->PostTask(FROM_HERE, [=]() {
      ws->Connect(
          addr,
          "localhost",
          "/ws",
          ctx,
          io_runner,
          HttpHeaders{},
          [done, echoed](const net::websocket::WebSocketFrame &frame) {
            if (!frame.is_control()) {
              echoed->store(true);
              std::string payload(frame.text_payload());
              NEI_LOG_INFO("[ws] server echoed frame length: %s", payload.c_str());
              done->Signal();
            }
          },
          [done, echoed]() {
            if (!echoed->load())
              NEI_LOG_ERROR("[ws] connection closed before echo (handshake failed?)");
            done->Signal();
          });
    });

    // 升级完成前 SendText 静默丢弃（kConnected 检查），用延迟任务轮询发送。
    for (int i = 1; i <= 8; ++i) {
      io_runner->PostDelayedTask(FROM_HERE, [ws]() { ws->SendText("hello"); }, TimeDelta::FromMilliseconds(50 * i));
    }
  } else {
    // HTTP：融合 HttpClient，协议由 ALPN 结果决定。
    client = scoped_refptr<HttpClient>(new HttpClient());
    io_runner->PostTask(FROM_HERE, [=]() {
      client->Send(MakeGet(), addr, ctx, io_runner, [done](std::unique_ptr<HttpResponse> resp) {
        if (!resp) {
          NEI_LOG_ERROR("[http] request failed (null response)");
        } else {
          // h2 聚合响应不填 http_version（kUnknown）；h1 引擎填 kHttp11/kHttp10。
          const char *engine = (resp->http_version == HttpVersion::kUnknown) ? "h2" : "h1";
          NEI_LOG_INFO("[http] via %s: %s", engine, resp->body.c_str());
        }
        done->Signal();
      });
    });
  }

  const bool completed = done->TimedWait(std::chrono::seconds(15));
  if (!completed)
    NEI_LOG_ERROR("[client] timed out waiting for response");

  // 关闭客户端（任意线程投递）并排空 I/O 线程上的异步 teardown 后再退出。
  if (client)
    client->Close();
  if (ws)
    ws->Close();
  WaitableEvent fence(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner->PostTask(FROM_HERE, [&fence]() { fence.Signal(); });
  fence.Wait();
  IOThread::Shutdown();
  return completed ? 0 : 1;
}
