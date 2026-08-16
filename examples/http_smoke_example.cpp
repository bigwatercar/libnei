// =============================================================================
// http_smoke — HTTP server/client lifecycle test (ASAN/TSan target)
// =============================================================================

#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <neixx/common/at_exit.h>
#include <neixx/common/location.h>
#include <neixx/io/io_thread.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>

using namespace nei;
using namespace nei::net;
using namespace nei::net::http;

constexpr uint16_t kPort = 18081;

int main() {
    AtExitManager at_exit;
    IOThread::Start();

    auto io_runner = GetGlobalIOTaskRunner();
    auto done = std::make_shared<WaitableEvent>(
        WaitableEvent::ResetPolicy::kAutomatic, false);
    auto passed = std::make_shared<std::atomic<bool>>(false);

    // Post server setup + client request to IO thread.
    auto server_ptr = std::make_shared<HttpServer>();
    io_runner->PostTask(FROM_HERE, [=]() {
        server_ptr->AddRoute(HttpMethod::kGet, "/hello",
            [](const HttpRequest&) {
                HttpResponse resp;
                resp.SetStatus(HttpStatusCode::kOk);
                resp.body = "world";
                resp.headers.push_back({"Content-Type", "text/plain"});
                return resp;
            });

        IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), kPort);
        if (!server_ptr->Listen(addr)) {
            std::cerr << "FAIL: Listen" << std::endl;
            done->Signal();
            return;
        }
        std::cout << "[server] listening" << std::endl;

        auto client = scoped_refptr<HttpClient>(new HttpClient());
        HttpRequest req;
        req.method = HttpMethod::kGet;
        req.url = Url("/hello");
        req.http_version = HttpVersion::kHttp11;
        req.headers.push_back({"Host", "127.0.0.1:18081"});

        // Capture by value to keep client/server alive in callback.
        client->Send(req, addr, nullptr, io_runner,
            [=](std::unique_ptr<HttpResponse> resp) {
                if (resp && resp->status.code() == HttpStatusCode::kOk &&
                    resp->body == "world") {
                    passed->store(true);
                }
                server_ptr->Shutdown();
                std::cout << "[server] shutdown" << std::endl;
                done->Signal();
            });
    });

    done->Wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    IOThread::Shutdown();

    if (!passed->load()) {
        std::cerr << "FAIL: no response" << std::endl;
        return 1;
    }

    std::cout << "PASS" << std::endl;
    return 0;
}
