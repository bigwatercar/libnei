// =============================================================================
// resolve_connect_demo  --  DNS resolve + TCP connect to port 80
// =============================================================================
//
// Demonstrates the end-to-end async networking flow:
//   1. Resolve a hostname via HostResolver (background worker thread)
//   2. Connect to the first resolved address via TCPClientSocket (IO thread)
//   3. Send HTTP GET /generate_204 request
//   4. Read and print the HTTP response
//
// Build: cmake --build . --target resolve_connect_demo
// Run:   ./resolve_connect_demo [hostname] [port]
//        (default: www.gstatic.com:80)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include <neixx/common/at_exit.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/net/address_list.h>
#include <neixx/net/host_resolver.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/net/tcp_client_socket.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_pool_instance.h>
#include <neixx/threading/thread.h>

namespace {

class IoThread {
public:
  explicit IoThread(const std::string &name) {
    nei::Thread::Options opts;
    opts.message_pump_type = nei::MessagePumpType::IO;
    thread_ = std::make_unique<nei::Thread>(name);
    thread_->StartWithOptions(opts);
    runner_ = thread_->GetTaskRunner();
  }

  ~IoThread() {
    thread_->Stop();
  }

  nei::scoped_refptr<nei::TaskRunner> runner() const {
    return runner_;
  }

private:
  std::unique_ptr<nei::Thread> thread_;
  nei::scoped_refptr<nei::TaskRunner> runner_;
};

struct State {
  nei::WaitableEvent done{nei::WaitableEvent::ResetPolicy::kAutomatic, false};
  std::atomic<bool> connected{false};
  std::atomic<bool> response_received{false};
};

void RunDemo(const std::string &host, uint16_t port) {
  std::cout << "=== Resolve & Connect Demo ===" << std::endl;
  std::cout << "Host: " << host << "  Port: " << port << std::endl;
  std::cout << std::endl;

  IoThread io_thread("demo-io");
  nei::net::HostResolver resolver;
  auto state = std::make_shared<State>();
  auto io_runner = io_thread.runner();

  std::cout << "[1] Resolving " << host << " ..." << std::endl;

  resolver.Resolve(
      host,
      [host, port, io_runner, state](const nei::net::AddressList &addresses) {
        if (addresses.empty()) {
          std::cerr << "ERROR: no addresses resolved" << std::endl;
          state->done.Signal();
          return;
        }
        const nei::net::IPEndPoint &ep = addresses.front();
        std::cout << "[1] Resolved: " << ep.ToString() << std::endl;

        nei::net::IPEndPoint target(ep.address(), port);
        std::cout << "[2] Connecting to " << target.ToString() << " ..." << std::endl;

        auto client = std::make_shared<nei::net::TCPClientSocket>();
        client->Connect(
            target,
            [client, host, state](bool success) {
              if (!success) {
                std::cerr << "ERROR: connect failed" << std::endl;
                state->done.Signal();
                return;
              }
              state->connected.store(true);
              std::cout << "[2] Connected!" << std::endl;

              // ---- Step 3: Send HTTP GET request ----
              std::string request = "GET /generate_204 HTTP/1.1\r\n"
                                    "Host: "
                                    + host
                                    + "\r\n"
                                      "Connection: close\r\n"
                                      "\r\n";

              auto write_buf = nei::MakeRefCounted<nei::IOBufferWithSize>(request.size());
              std::memcpy(write_buf->data(), request.data(), request.size());

              std::cout << "[3] Sending HTTP GET /generate_204 ..." << std::endl;

              client->WriteAsync(write_buf, request.size(), [client, state](bool ws, std::size_t /*n*/) {
                if (!ws) {
                  std::cerr << "ERROR: write failed" << std::endl;
                  state->done.Signal();
                  return;
                }
                std::cout << "[3] Request sent." << std::endl;

                // ---- Step 4: Read HTTP response ----
                auto read_buf = nei::MakeRefCounted<nei::IOBufferWithSize>(4096);
                std::cout << "[4] Reading response ..." << std::endl;

                client->ReadAsync(read_buf, 4096, [client, read_buf, state](bool rs, std::size_t n) {
                  if (!rs && n == 0) {
                    std::cout << "[4] Server closed connection." << std::endl;
                    state->done.Signal();
                    return;
                  }
                  if (!rs) {
                    std::cerr << "ERROR: read failed" << std::endl;
                    state->done.Signal();
                    return;
                  }

                  state->response_received.store(true);
                  std::cout << "[4] Received " << n << " bytes:" << std::endl;
                  std::cout.write(reinterpret_cast<const char *>(read_buf->data()), static_cast<std::streamsize>(n));
                  std::cout << std::endl;

                  client->Close();
                  state->done.Signal();
                });
              });
            },
            io_runner);
      },
      io_runner);

  std::cout << "[*] Waiting ..." << std::endl;
  if (!state->done.TimedWait(std::chrono::milliseconds(10000))) {
    std::cerr << "ERROR: timed out after 10s" << std::endl;
    return;
  }

  std::cout << std::endl;
  std::cout << "=== Result ===" << std::endl;
  std::cout << "  Connected:  " << (state->connected.load() ? "YES" : "NO") << std::endl;
  std::cout << "  Response:   " << (state->response_received.load() ? "YES" : "NO") << std::endl;
  if (state->connected.load())
    std::cout << std::endl << "Demo completed successfully." << std::endl;
}

} // namespace

int main(int argc, char *argv[]) {
  // AtExitManager must be the first stack object in main()  --  it ensures
  // cleanup callbacks run in reverse order of registration.
  nei::AtExitManager at_exit;

  std::string host = "www.gstatic.com";
  uint16_t port = 80;
  if (argc > 1)
    host = argv[1];
  if (argc > 2)
    port = static_cast<uint16_t>(std::atoi(argv[2]));

  // HostResolver needs a background thread pool for blocking DNS lookups.
  nei::ThreadPoolInstance::CreateAndStart(nei::ThreadPoolInstance::InitParams{});

  RunDemo(host, port);

  nei::ThreadPoolInstance::Shutdown();
  return 0;
}
