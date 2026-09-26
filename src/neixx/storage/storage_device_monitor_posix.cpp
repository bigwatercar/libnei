#include "storage_device_monitor_posix.h"

#if defined(_WIN32)
#error "POSIX implementation should not be compiled for Windows"
#endif

#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef __linux__
#error "StorageDeviceMonitor POSIX backend requires Linux netlink"
#endif
#include <linux/netlink.h>

#include <nei/debug/check.h>
#include <neixx/common/location.h>

namespace nei {
namespace {

std::string Basename(const std::string& path) {
  const size_t p = path.rfind('/');
  return p == std::string::npos ? path : path.substr(p + 1);
}

// The NETLINK_KOBJECT_UEVENT message payload is a sequence of NUL-terminated
// strings (kernel/netlink/uevent.c udev format):
//   <action>@<devpath>\0
//   KEY=VALUE\0
//   KEY=VALUE\0
//   ...
// Extract the fields the storage monitor needs into `message`.
void ParseUeventMsg(const char* buf, int n, UeventMsg& message) {
  const char* p = buf;
  const char* end = buf + n;
  bool leading = true;
  while (p < end) {
    const char* e = reinterpret_cast<const char*>(memchr(p, '\0',
                                                       static_cast<size_t>(end - p)));
    if (e == nullptr)
      break;
    const char* eq = reinterpret_cast<const char*>(memchr(p, '=',
                                                         static_cast<size_t>(e - p)));
    if (eq != nullptr) {
      // KEY=VALUE pair.
      const std::string key(p, eq - p);
      const std::string value(eq + 1, e - eq - 1);
      if (key == "ACTION")
        message.action = value;
      else if (key == "SUBSYSTEM")
        message.subsystem = value;
      else if (key == "DEVTYPE")
        message.devtype = value;
      else if (key == "DEVNAME")
        message.devname = value;
      else if (key == "DEVNODE")
        message.devnode = value;
      else if (key == "DEVPATH")
        message.devpath = value;
      leading = false;
    } else if (leading) {
      // Leading <action>@<devpath> string (fallback for action/devpath).
      const char* at = reinterpret_cast<const char*>(memchr(p, '@',
                                                           static_cast<size_t>(e - p)));
      if (at != nullptr) {
        if (message.action.empty())
          message.action = std::string(p, at - p);
        if (message.devpath.empty())
          message.devpath = std::string(at + 1, e - at - 1);
      }
      leading = false;
    }
    p = e + 1;
  }
}

}  // namespace

// Create a raw NETLINK_KOBJECT_UEVENT socket bound to receive kernel uevents.
//
// Must bind nl_pid=0 (all pids) AND nl_groups=1: the kernel only broadcasts
// NETLINK_KOBJECT_UEVENT messages to sockets that joined the udev netlink
// group. libudev's udev_monitor_new_from_netlink() binds with nl_groups=0
// and therefore never receives events on this kernel.
int StorageDeviceMonitor::Impl::CreateNetlinkSocket() {
  int sock = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
                      NETLINK_KOBJECT_UEVENT);
  if (sock < 0)
    return -1;
  // Best effort: not all glibc builds define/accept SO_RCVBUFFORCE.
  #if defined(SO_RCVBUFFORCE)
  int rcvbuf = 134217728;
  (void)setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof rcvbuf);
  #endif
  struct sockaddr_nl addr = {0};
  addr.nl_family = AF_NETLINK;
  addr.nl_pid = 0;
  addr.nl_groups = 1;
  if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) !=
      0) {
    ::close(sock);
    return -1;
  }
  return sock;
}

StorageDeviceMonitor::Impl::Impl(scoped_refptr<TaskRunner> task_runner,
                                  Callback callback)
    : task_runner_(std::move(task_runner))
    , callback_(std::move(callback))
    , weak_factory_(this, FROM_HERE) {
  DCHECK(task_runner_ != nullptr);
}

StorageDeviceMonitor::Impl::~Impl() {
  Stop();
}

bool StorageDeviceMonitor::Impl::Start() {
  Stop();

  if (!callback_)
    return false;

  udev_fd_ = CreateNetlinkSocket();
  if (udev_fd_ < 0)
    return false;

  MessagePumpForIO *pump = MessagePumpForIO::Current();
  if (pump == nullptr) {
    CleanupUdev();
    return false;
  }
  if (!controller_.StartWatching(
      pump, static_cast<NativeIOHandle>(udev_fd_),
      MessagePumpForIO::FdWatchController::Mode::READ, this)) {
    CleanupUdev();
    return false;
  }

  watching_ = true;
  return true;
}

void StorageDeviceMonitor::Impl::Stop() {
  watching_ = false;
  // The callback persists for the lifetime of the monitor (it is set once at
  // construction and re-used on every Start()).  Watching is gated by watching_
  // and by the (closed) netlink fd, so an outstanding callback cannot fire
  // after stop.
  controller_.StopWatching();
  CleanupUdev();
}

void StorageDeviceMonitor::Impl::OnFileCanReadWithoutBlocking(
    NativeIOHandle handle) {
  if (!watching_ || udev_fd_ < 0)
    return;
  if (handle != static_cast<NativeIOHandle>(udev_fd_))
    return;
  DrainUdevEvents();
}

void StorageDeviceMonitor::Impl::DrainUdevEvents() {
  std::vector<char> buf(128 * 1024);
  while (watching_ && udev_fd_ >= 0) {
    struct msghdr msg = {0};
    struct sockaddr_nl from = {0};
    from.nl_family = AF_NETLINK;
    msg.msg_name = &from;
    msg.msg_namelen = sizeof from;
    struct iovec iov = {buf.data(), buf.size()};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    const int n = ::recvmsg(udev_fd_, &msg, 0);
    if (n <= 0)
      return;
    UeventMsg message;
    ParseUeventMsg(buf.data(), n, message);
    HandleUdevMessage(message);
  }
}

void StorageDeviceMonitor::Impl::HandleUdevMessage(const UeventMsg& message) {
  std::fprintf(stderr,
               "[SDM-dbg] action=%-7s subsystem=%-10s devtype=%-10s "
               "name=%-12s devnode=%-12s devpath=%s\n",
               message.action.c_str(),
               message.subsystem.c_str(),
               message.devtype.c_str(),
               message.devname.c_str(),
               message.devnode.c_str(),
               message.devpath.c_str());

  // Only whole disks: block subsystem, DEVTYPE == "disk".  Partitions, roms,
  // and other block entities are ignored.
  if (message.subsystem != "block")
    return;
  if (message.devtype != "disk")
    return;

  DeviceEventType type;
  if (message.action == "add")
    type = DeviceEventType::kAdded;
  else if (message.action == "remove")
    type = DeviceEventType::kRemoved;
  else
    type = DeviceEventType::kChanged;

  DeviceInfo info;
  info.device_path =
      !message.devnode.empty() ? message.devnode : message.devpath;
  info.name = !message.devname.empty()
      ? message.devname
      : Basename(message.devpath);
  info.subsystem = message.subsystem;

  EmitEvent(type, info);
}

void StorageDeviceMonitor::Impl::EmitEvent(DeviceEventType type,
                                           const DeviceInfo& info) {
  if (!watching_ || !callback_)
    return;

  if (task_runner_->RunsTasksInCurrentSequence()) {
    callback_.Run(type, info);
  } else {
    Callback cb = callback_;
    DeviceInfo info_copy = info;
    auto weak = weak_factory_.GetWeakPtr(FROM_HERE);
    task_runner_->PostTask(FROM_HERE,
        [weak = std::move(weak), cb = std::move(cb), info_copy = std::move(info_copy),
         type]() {
          if (!weak || !weak->watching_)
            return;
          cb.Run(type, info_copy);
        });
  }
}

void StorageDeviceMonitor::Impl::CleanupUdev() {
  if (udev_fd_ >= 0) {
    ::close(udev_fd_);
    udev_fd_ = -1;
  }
}

}  // namespace nei
