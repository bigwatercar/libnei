// libnei HTTP/1.1 loopback throughput benchmark
//
// Measures HTTP request/response throughput over a single TCP connection.
// Tests both keep-alive reuse and new-connection modes.
//
// Build: cmake --build build/windows-vs2022-shared-release --target http_throughput_bench
// Run:   .\http_throughput_bench [requests] [mode: keepalive|newconn]
//        default: 10000 requests, keep-alive

#if defined(_WIN32)
#include <winsock2.h>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <neixx/common/at_exit.h>
#include <neixx/common/location.h>
#include <neixx/net/http/http_client.h>
#include <neixx/net/http/http_common.h>
#include <neixx/net/http/http_request.h>
#include <neixx/net/http/http_response.h>
#include <neixx/net/http/http_server.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

using namespace nei;
using namespace nei::net;
using namespace nei::net::http;

namespace {

using Clock = std::chrono::high_resolution_clock;

constexpr uint16_t kServerPort = 18092;
constexpr int kDefaultRequests = 10000;

// ===========================================================================
// Stats
// ===========================================================================
struct Stats {
    std::atomic<int64_t> completed{0};
    std::atomic<int64_t> total_ns{0};
    std::atomic<int64_t> min_ns{INT64_MAX};
    std::atomic<int64_t> max_ns{0};
};

// ===========================================================================
// Benchmark runner
// ===========================================================================
void RunBenchmark(int total_requests, bool keepalive) {
    AtExitManager at_exit;

    // ---- Server on dedicated IO thread ----
    Thread server_thread;
    Thread::Options server_opts;
    server_opts.message_pump_type = MessagePumpType::IO;
    server_thread.StartWithOptions(server_opts);
    auto server_runner = server_thread.GetTaskRunner();

    auto server = std::make_shared<HttpServer>();
    auto server_ready =
        std::make_shared<WaitableEvent>(
            WaitableEvent::ResetPolicy::kAutomatic, false);

    server_runner->PostTask(FROM_HERE, [=]() {
        server->AddRoute(HttpMethod::kGet, "/ping",
                         [keepalive](const HttpRequest&) {
                             HttpResponse resp;
                             resp.SetStatus(HttpStatusCode::kOk);
                             resp.body = "pong";
                             resp.headers.push_back(
                                 {"Content-Type", "text/plain"});
                             if (!keepalive) {
                                 resp.headers.push_back(
                                     {"Connection", "close"});
                             }
                             return resp;
                         });
        IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), kServerPort);
        server->Listen(addr, server_runner);
        server_ready->Signal();
    });
    server_ready->Wait();

    // ---- Client on separate IO thread ----
    Thread client_thread;
    Thread::Options client_opts;
    client_opts.message_pump_type = MessagePumpType::IO;
    client_thread.StartWithOptions(client_opts);
    auto client_runner = client_thread.GetTaskRunner();

    auto done =
        std::make_shared<WaitableEvent>(
            WaitableEvent::ResetPolicy::kAutomatic, false);
    auto stats = std::make_shared<Stats>();

    IPEndPoint addr(IPAddress::FromIPv4(127, 0, 0, 1), kServerPort);

    client_runner->PostTask(FROM_HERE, [=]() {
        auto client = scoped_refptr<HttpClient>(new HttpClient());
        auto remaining = std::make_shared<std::atomic<int>>(total_requests);

        struct ReqState {
            std::shared_ptr<std::atomic<int>> remaining;
            std::shared_ptr<Stats> stats;
            std::shared_ptr<WaitableEvent> done;
            scoped_refptr<HttpClient> client;
            IPEndPoint addr;
            scoped_refptr<SingleThreadTaskRunner> runner;
            bool keepalive;
            Clock::time_point batch_start;
        };

        auto state = std::make_shared<ReqState>();
        state->remaining = remaining;
        state->stats = stats;
        state->done = done;
        state->client = client;
        state->addr = addr;
        state->runner = client_runner;
        state->keepalive = keepalive;
        state->batch_start = Clock::now();

        auto send_next = std::make_shared<std::function<void()>>();
        *send_next = [state, send_next]() {
            if (state->remaining->fetch_sub(1) <= 0) {
                // All done.
                auto elapsed = Clock::now() - state->batch_start;
                double sec =
                    std::chrono::duration<double>(elapsed).count();
                int64_t completed = state->stats->completed.load();
                double rps = sec > 0 ? completed / sec : 0;
                double avg_us =
                    completed > 0
                        ? state->stats->total_ns.load() / completed / 1000.0
                        : 0;

                std::cout << "=== HTTP Throughput Benchmark ===\n";
                std::cout << "Mode:        "
                          << (state->keepalive ? "keep-alive"
                                               : "new-connection")
                          << "\n";
                std::cout << "Requests:    " << completed << "\n";
                std::cout << "Elapsed:     "
                          << std::fixed << std::setprecision(3) << sec
                          << " s\n";
                std::cout << "Throughput:  "
                          << std::fixed << std::setprecision(1) << rps
                          << " req/s\n";
                std::cout << "Avg latency: "
                          << std::fixed << std::setprecision(1) << avg_us
                          << " us\n";
                if (completed > 0) {
                    std::cout << "Min latency: "
                              << state->stats->min_ns.load() / 1000.0
                              << " us\n";
                    std::cout << "Max latency: "
                              << state->stats->max_ns.load() / 1000.0
                              << " us\n";
                }

                state->done->Signal();
                return;
            }

            HttpRequest req;
            req.method = HttpMethod::kGet;
            req.url = Url("/ping");
            req.http_version = HttpVersion::kHttp11;
            req.headers.push_back({"Host", "127.0.0.1"});

            auto t0 = Clock::now();

            if (!state->keepalive) {
                // New connection per request.
                auto fresh_client =
                    scoped_refptr<HttpClient>(new HttpClient());
                fresh_client->Send(
                    req, state->addr, nullptr, state->runner,
                    [state, send_next, t0, fresh_client](
                        std::unique_ptr<HttpResponse> resp) {
                        if (resp &&
                            resp->status.code() == HttpStatusCode::kOk) {
                            auto t1 = Clock::now();
                            auto ns =
                                std::chrono::duration_cast<
                                    std::chrono::nanoseconds>(t1 - t0)
                                    .count();
                            state->stats->completed.fetch_add(1);
                            state->stats->total_ns.fetch_add(ns);
                            // Update min/max.
                            int64_t prev_min =
                                state->stats->min_ns.load();
                            while (ns < prev_min &&
                                   !state->stats->min_ns.compare_exchange_weak(
                                       prev_min, ns)) {
                            }
                            int64_t prev_max =
                                state->stats->max_ns.load();
                            while (ns > prev_max &&
                                   !state->stats->max_ns.compare_exchange_weak(
                                       prev_max, ns)) {
                            }
                        }
                        (*send_next)();
                    });
            } else {
                // Keep-alive: reuse same client.
                state->client->Send(
                    req, state->addr, nullptr, state->runner,
                    [state, send_next, t0](
                        std::unique_ptr<HttpResponse> resp) {
                        if (resp &&
                            resp->status.code() == HttpStatusCode::kOk) {
                            auto t1 = Clock::now();
                            auto ns =
                                std::chrono::duration_cast<
                                    std::chrono::nanoseconds>(t1 - t0)
                                    .count();
                            state->stats->completed.fetch_add(1);
                            state->stats->total_ns.fetch_add(ns);
                            int64_t prev_min =
                                state->stats->min_ns.load();
                            while (ns < prev_min &&
                                   !state->stats->min_ns.compare_exchange_weak(
                                       prev_min, ns)) {
                            }
                            int64_t prev_max =
                                state->stats->max_ns.load();
                            while (ns > prev_max &&
                                   !state->stats->max_ns.compare_exchange_weak(
                                       prev_max, ns)) {
                            }
                        }
                        (*send_next)();
                    });
            }
        };

        // Kick off the pipeline.
        (*send_next)();
    });

    done->Wait();

    // Shutdown.
    auto server_stopped =
        std::make_shared<WaitableEvent>(
            WaitableEvent::ResetPolicy::kAutomatic, false);
    server_runner->PostTask(FROM_HERE, [=]() {
        server->Shutdown();
        server_stopped->Signal();
    });
    server_stopped->Wait();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client_thread.Stop();
    server_thread.Stop();
}

}  // namespace

int main(int argc, char* argv[]) {
    int requests = kDefaultRequests;
    bool keepalive = true;

    if (argc > 1) {
        requests = std::atoi(argv[1]);
        if (requests <= 0)
            requests = kDefaultRequests;
    }
    if (argc > 2) {
        std::string_view mode(argv[2]);
        if (mode == "newconn")
            keepalive = false;
    }

    std::cout << "HTTP Throughput Benchmark\n";
    std::cout << "Requests: " << requests << "\n";
    std::cout << "Mode: " << (keepalive ? "keep-alive" : "new-connection")
              << "\n\n";

    RunBenchmark(requests, keepalive);
    return 0;
}
