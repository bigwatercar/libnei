// =============================================================================
// http_echo_server — h1/h2 ALPN 分流 HTTP 服务器 + ws/wss echo 示例
// =============================================================================
//
// 用法:
//   http_echo_server [--h1|--h2] [--port <n>] [--plain-port <n>]
//
//   - 缺省      : 同时支持 h1+h2（TLS ALPN {"h2","http/1.1"}）
//   - --h1      : 仅 http/1.1（ALPN {"http/1.1"}）
//   - --h2      : 仅 h2（ALPN {"h2"}）
//   - --port    : TLS 端口（https + wss），默认 9443
//   - --plain-port : 明文 TCP 端口（http + ws，h1 引擎），默认 8080
//
// 路由:
//   GET /echo : h1 引擎返回 "http 1.1, hello"；h2 引擎返回 "http 2, hello"
//   WS  /ws   : 收到 text 帧后回发该帧的长度（十进制字符串 text 帧）
//
// 验证（另开终端）:
//   curl -k https://127.0.0.1:9443/echo               → http 2, hello（缺省 ALPN）
//   curl -k --http1.1 https://127.0.0.1:9443/echo     → http 1.1, hello
//   curl http://127.0.0.1:8080/echo                   → http 1.1, hello（明文 h1）
//   浏览器控制台（wss://127.0.0.1:9443/ws）: 发 "hello" → 收到 "5"
// =============================================================================

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <nei/log/log.h>
#include <neixx/command_line/command_line.h>
#include <neixx/common/at_exit.h>
#include <neixx/io/io_thread.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/ssl_context.h>
#include <neixx/net/websocket/websocket_connection.h>
#include <neixx/net/websocket/websocket_frame.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>

using namespace nei;
using namespace nei::net;
using namespace nei::net::http;

namespace {

// 开发用静态自签名证书（CN=localhost，有效期 2026-08-16 ~ 2036-08-13）。
// 仅示例用途；生产环境请通过外部证书管理注入真实证书。
constexpr char kCertPem[] = "-----BEGIN CERTIFICATE-----\n"
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

constexpr char kKeyPem[] = "-----BEGIN RSA PRIVATE KEY-----\n"
                           "MIIEogIBAAKCAQEAqAYYm87apQS2YtZOAnj7Gb3GxZ1RXem3ncRiLq2DFEsUjLC3\n"
                           "l/XsJkPcq2kEv5n9z2mw3+yemwXnBBA5hvxBTy3tXwV4YnQfJjE0QwBv+ATCBLG4\n"
                           "eyrJNYHp0XbCD+3MZvugaMiUWk0YaCdS+E5gCDTOh+ZRWliP2azkPtqcSCxn8n8H\n"
                           "7FxNinCIB+CS8JZ/igIEYH3bi9WdSTLw/xa6LOVhq9x9ciUTDAFMAo4vEAWr7sb3\n"
                           "7T/ifMTaioi4sOcODlZKmrkNNSDk+py2a8NpW2PQUn+GjLeCQkXTLZ6f/szh2TJT\n"
                           "+kX3a7zLkSRHpRbcEUZTvN4jV6EPFQoHOMiebwIDAQABAoIBADQruGZggxkr7mmf\n"
                           "+xbfc7AZceeYIlSTpjSxbn7p10Js0ZOhp0/ixxFWjuUWNag8a0eEnKvar6CY45Mq\n"
                           "aoJUPv8T1pljfG6teaKFMqH/N8T9zTRh7lMoBNO9Y9lrE3SYtJfhV3xRk2a6e3KT\n"
                           "izqYM084/kvKIsZ3qyq+eRxfCpmJ1s1ZUWbcUYX6DMZ1Lub+wKaRbWP5N5RVWo7o\n"
                           "onqaCVZrNODYGz2VVHMlIpwiNOoN6ADowhjNkeSvz07dBqL8aQeyKm8n/7z826bg\n"
                           "IUpg/M0Sfhs9thH/kmZZ/R6TE4mfeYpijONUc5bZsSwZaljD8iqEQFZXDQNmfSqB\n"
                           "1nyZxSkCgYEA5NsOfMuvoF7O8J4VrnK9RpKpgp1zOFnxrRzeoPSh+sBtZtvF6/9q\n"
                           "mVRy7QBEbmb5wcVk0+hIU1YaXgzq0fEHyXtRwAVkKyMccRciI+oFha2OR/94fsmw\n"
                           "7LcHSKdMKfuXwTTPNY37JPT3XCCYEtQ8qDC7vk7YdV9on9RpGbyVXQMCgYEAu/Py\n"
                           "17CskitFkYTqpCCukQz1P1tr0I2aVMnY1JkplIf5rj9Zm8FmRiGQfNvoSoOL4ex7\n"
                           "+xz8wjyytse2Ka1tt2HgPrtLQqacRAksMaF6kaqMAo89aWpDLeT2nNWz0eEARl9r\n"
                           "fna+DDRa+qUqFep5n4wr5HC1o18FkIpZolyVDyUCgYBGIGHWF8wfRi3/SVG3fO1G\n"
                           "3NYYcgrGb7lApKILjCq+XYyoghup70BI77mvqe9OLTvHBqeYz4qqDq5Rt3+VCVir\n"
                           "gqBQSNai6UVj2gTaIHHEvqPkqAHSSBdw0bznpGwQSUn9KCN+c51Le8z4a/xteJ+F\n"
                           "ojlFXX+yp6O1pi72dfUG5QKBgB5nxn9SG3jB+00hPXwztUnN2NbZCUYBwle5F5S8\n"
                           "+lcG8ENaCDsEPHFX+LHaOWfkg/qWcTAcbl9Vxmt/P17aqYcjFE3Rqskrftgay8Vz\n"
                           "pApwlpnLZlnpUNjZ03NntuFbDtpTkWYx+2iqB5XIplhJSEehO3CHMzssog/R8dIs\n"
                           "PAjpAoGAdbq5k25Kchi+HI9ZzPtSii6za1vz3v/qxDOLTdS2zWjX4GoiOpMh8zjS\n"
                           "l7BxMmoNutKrXNIRCTlHB8LyHtbuuT0qXd3EAeYvHDY6OGPriW/mRcD3B2NOZpbU\n"
                           "0xUzikmvAUEqmU68UhdRa2qgEt+EeQpbldz3UqUAhSYaP2l141M=\n"
                           "-----END RSA PRIVATE KEY-----\n";

enum class AlpnMode {
  kBoth,   // {"h2","http/1.1"}
  kH1Only, // {"http/1.1"}
  kH2Only, // {"h2"}
};

void RegisterRoutes(HttpServer &server) {
  // /echo：按请求到达的引擎返回问候。h2 引擎的 HttpRequest.http_version
  // 保持 kUnknown；h1 引擎填 kHttp11/kHttp10。
  server.AddRoute(HttpMethod::kGet, "/echo", [](const HttpRequest &req) {
    HttpResponse resp;
    resp.SetStatus(HttpStatusCode::kOk);
    resp.headers.push_back({"Content-Type", "text/plain"});
    resp.body = (req.http_version == HttpVersion::kUnknown) ? "http 2, hello" : "http 1.1, hello";
    return resp;
  });

  // /ws：回发收到的 text 帧长度（十进制字符串）。h1 引擎的升级路径处理
  // ws（明文 TCP）与 wss（TLS 协商 h1 后升级）。
  server.AddWebSocketRoute("/ws",
                           [](net::websocket::WebSocketConnection &conn, const net::websocket::WebSocketFrame &frame) {
                             if (!frame.is_control())
                               conn.SendText(std::to_string(frame.text_payload().size()));
                           });
}

} // namespace

int main(int argc, char **argv) {
  AtExitManager at_exit;
  // 退出时由 AtExitManager 回收日志子系统（内部完成排空与 sink 释放），
  // 无需在 return 前手动 nei_log_flush。
  AtExitManager::RegisterCallback(&nei_log_shutdown);
  // 日志：stdout sink，经 nei_log_add_sink 注册到默认配置并 update_config 发布。
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

  AlpnMode mode = AlpnMode::kBoth;
  if (cl.HasSwitch("h1"))
    mode = AlpnMode::kH1Only;
  else if (cl.HasSwitch("h2"))
    mode = AlpnMode::kH2Only;

  int tls_port = 9443;
  int plain_port = 8080;
  if (!cl.GetSwitchValueASCII("port").empty())
    tls_port = std::atoi(cl.GetSwitchValueASCII("port").c_str());
  if (!cl.GetSwitchValueASCII("plain-port").empty())
    plain_port = std::atoi(cl.GetSwitchValueASCII("plain-port").c_str());

  auto ssl_ctx = std::make_shared<net::SSLContext>(net::SSLContext::Mode::Server);
  if (!ssl_ctx->SetCertificate(kCertPem, kKeyPem)) {
    NEI_LOG_ERROR("FAIL: SetCertificate");
    return 1;
  }
  switch (mode) {
  case AlpnMode::kH1Only:
    ssl_ctx->SetAlpnProtocols({"http/1.1"});
    break;
  case AlpnMode::kH2Only:
    ssl_ctx->SetAlpnProtocols({"h2"});
    break;
  default:
    ssl_ctx->SetAlpnProtocols({"h2", "http/1.1"});
    break;
  }

  IOThread::Start();
  auto io_runner = GetGlobalIOTaskRunner();

  auto ready = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto listen_ok = std::make_shared<std::atomic<bool>>(false);
  auto tls_server = std::make_shared<HttpServer>();
  auto plain_server = std::make_shared<HttpServer>();

  io_runner->PostTask(FROM_HERE, [=]() {
    RegisterRoutes(*tls_server);
    RegisterRoutes(*plain_server);

    IPEndPoint tls_addr(IPAddress::FromIPv4(127, 0, 0, 1), static_cast<uint16_t>(tls_port));
    IPEndPoint plain_addr(IPAddress::FromIPv4(127, 0, 0, 1), static_cast<uint16_t>(plain_port));

    if (!tls_server->Listen(tls_addr, ssl_ctx.get(), io_runner)) {
      NEI_LOG_ERROR("FAIL: TLS Listen on port %d", tls_port);
      ready->Signal();
      return;
    }
    if (!plain_server->Listen(plain_addr, io_runner)) {
      NEI_LOG_ERROR("FAIL: plain Listen on port %d", plain_port);
      ready->Signal();
      return;
    }
    listen_ok->store(true);
    ready->Signal();
  });

  ready->Wait();
  if (!listen_ok->load()) {
    IOThread::Shutdown();
    return 1;
  }

  const char *mode_str =
      mode == AlpnMode::kH1Only ? "http/1.1 only" : (mode == AlpnMode::kH2Only ? "h2 only" : "h1+h2");
  NEI_LOG_INFO("[server] ALPN mode: %s", mode_str);
  NEI_LOG_INFO("[server] https/wss on 127.0.0.1:%d", tls_port);
  NEI_LOG_INFO("[server] http/ws   on 127.0.0.1:%d", plain_port);
  NEI_LOG_INFO("[server] GET /echo | WS /ws | press Enter to exit");

  // 控制台回车优雅退出；后台运行（stdin 已关闭/管道）时保持服务直到被 kill。
  if (std::cin.get() != EOF) {
    WaitableEvent drained(WaitableEvent::ResetPolicy::kAutomatic, false);
    io_runner->PostTask(FROM_HERE, [=, &drained]() {
      tls_server->Shutdown();
      plain_server->Shutdown();
      drained.Signal();
    });
    drained.Wait();
    // 排空异步 teardown（Close/注销跨多个 I/O 跳）。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    IOThread::Shutdown();
    NEI_LOG_INFO("[server] bye");
    return 0;
  }

  NEI_LOG_INFO("[server] stdin closed — running until killed");
  WaitableEvent never(WaitableEvent::ResetPolicy::kAutomatic, false);
  never.Wait();
  return 0;
}
