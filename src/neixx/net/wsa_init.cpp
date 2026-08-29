#include <neixx/net/wsa_init.h>

#if defined(_WIN32)

#include <windows.h>
#include <winsock2.h>

#include <neixx/common/no_destructor.h>

namespace nei::net {

namespace {

struct WsaInit {
  WsaInit() {
    WSADATA data = {};
    WSAStartup(MAKEWORD(2, 2), &data);
  }

  // NoDestructor skips the destructor  --  WSACleanup is unnecessary at process
  // exit and may race with other Winsock-using static destructors.
  ~WsaInit() {
    WSACleanup();
  }
};

} // namespace

void EnsureWsa() {
  static NoDestructor<WsaInit> init;
  (void)init;
}

} // namespace nei::net

#endif // _WIN32
