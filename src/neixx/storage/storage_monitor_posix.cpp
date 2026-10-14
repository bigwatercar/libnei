#if !defined(_WIN32)

#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "storage_monitor_impl.h"

namespace nei {
namespace {

namespace fs = std::filesystem;

struct MountEntry {
  std::string major_minor;
  std::string mount_point;
  std::string fs_type;
  std::string source;
};

// mountinfo escapes space, tab, newline and backslash as \NNN (octal).
std::string UnescapeMountField(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '\\' && i + 3 < in.size() && in[i + 1] >= '0' && in[i + 1] <= '3' && in[i + 2] >= '0'
        && in[i + 2] <= '7' && in[i + 3] >= '0' && in[i + 3] <= '7') {
      out.push_back(static_cast<char>(((in[i + 1] - '0') << 6) | ((in[i + 2] - '0') << 3) | (in[i + 3] - '0')));
      i += 3;
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

// /dev/disk/by-label names escape unsafe bytes as \xNN.
std::string UnescapeHexName(const std::string &in) {
  std::string out;
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '\\' && i + 3 < in.size() && in[i + 1] == 'x' && std::isxdigit(static_cast<unsigned char>(in[i + 2]))
        && std::isxdigit(static_cast<unsigned char>(in[i + 3]))) {
      out.push_back(static_cast<char>(std::strtol(in.substr(i + 2, 2).c_str(), nullptr, 16)));
      i += 3;
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

// Format: id parent maj:min root mountpoint opts [optional...] - fstype source superopts
std::vector<MountEntry> ParseMountInfo(const std::string &path) {
  std::vector<MountEntry> entries;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    std::vector<std::string> tok;
    for (std::string t; ss >> t;)
      tok.push_back(std::move(t));
    std::size_t dash = 6;
    while (dash < tok.size() && tok[dash] != "-")
      ++dash;
    if (dash + 2 >= tok.size())
      continue;
    entries.push_back({tok[2], UnescapeMountField(tok[4]), tok[dash + 1], UnescapeMountField(tok[dash + 2])});
  }
  return entries;
}

std::string ReadTrimmed(const fs::path &p) {
  std::ifstream in(p);
  std::string s;
  std::getline(in, s);
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Finds the by-uuid / by-label entry whose symlink target is |dev_name|.
std::string FindDiskAlias(const fs::path &dir, const std::string &dev_name) {
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code lec;
    const fs::path target = fs::read_symlink(it->path(), lec);
    if (!lec && target.filename() == dev_name)
      return it->path().filename().string();
  }
  return {};
}

// Stable-per-volume identifier, most stable source first.  The device node
// name is the last resort: it can change when a device is re-plugged.
std::string MakeDeviceId(const fs::path &disk,
                         const std::string &dev_name,
                         const std::string &partition_index,
                         const StorageMonitor::Options &opts,
                         const std::string &source) {
  const fs::path alias_root = fs::path(opts.dev_disk_root);
  if (std::string uuid = FindDiskAlias(alias_root / "by-uuid", dev_name); !uuid.empty())
    return "uuid:" + uuid;
  if (std::string partuuid = FindDiskAlias(alias_root / "by-partuuid", dev_name); !partuuid.empty())
    return "partuuid:" + partuuid;

  // Hardware identity belongs to the whole disk, so a partition appends its
  // index to stay distinct from its siblings.
  std::string hardware = ReadTrimmed(disk / "device" / "wwid");
  const char *prefix = "wwid:";
  if (hardware.empty()) {
    hardware = ReadTrimmed(disk / "device" / "serial");
    prefix = "serial:";
  }
  if (!hardware.empty())
    return prefix + hardware + (partition_index.empty() ? std::string() : "p" + partition_index);

  return "path:" + source;
}

bool BuildStorageInfo(const MountEntry &m, const StorageMonitor::Options &opts, StorageInfo *out) {
  if (m.source.compare(0, 5, "/dev/") != 0)
    return false;

  std::error_code ec;
  const fs::path sys_root = fs::canonical(opts.sysfs_root, ec);
  if (ec)
    return false;
  const fs::path dev = fs::canonical(fs::path(opts.sysfs_root) / "dev" / "block" / m.major_minor, ec);
  if (ec)
    return false;

  const std::string name = dev.filename().string();
  // Snap packages mount squashfs images through loop devices.
  if (name.compare(0, 4, "loop") == 0 && m.fs_type == "squashfs")
    return false;

  // The "partition" attribute is non-empty only for partitions; a partition's
  // removable/vendor/model attributes live on its parent disk.
  const std::string partition_index = ReadTrimmed(dev / "partition");
  const fs::path disk = partition_index.empty() ? dev : dev.parent_path();
  const bool on_usb = ("/" + dev.lexically_relative(sys_root).generic_string()).find("/usb") != std::string::npos;
  const bool removable = ReadTrimmed(disk / "removable") == "1" || on_usb;

  out->type = removable ? StorageInfo::Type::kRemovableMassStorage : StorageInfo::Type::kFixedMassStorage;
  out->device_id = MakeDeviceId(disk, name, partition_index, opts, m.source);
  out->location = m.mount_point;
  out->label = UnescapeHexName(FindDiskAlias(fs::path(opts.dev_disk_root) / "by-label", name));
  out->vendor = ReadTrimmed(disk / "device" / "vendor");
  out->model = ReadTrimmed(disk / "device" / "model");
  // sysfs "size" counts 512-byte sectors regardless of the device block size.
  const std::string sectors = ReadTrimmed(dev / "size");
  out->total_size_bytes = sectors.empty() ? 0 : std::strtoull(sectors.c_str(), nullptr, 10) * 512ULL;
  return true;
}

} // namespace

bool StorageMonitor::Impl::PlatformStart() {
  if (!options_.watch_changes)
    return true;

  MessagePumpForIO *pump = MessagePumpForIO::Current();
  if (pump == nullptr)
    return false;

  mount_fd_ = ::open(options_.mountinfo_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (mount_fd_ < 0)
    return false;

  // procfs mountinfo is always "readable" (EPOLLIN), so READ mode would spin.
  // On a mount table change the kernel reports EPOLLERR|EPOLLPRI, and the pump
  // already dispatches EPOLLERR to the write handler of a WRITE-mode watch.
  if (!controller_.StartWatching(pump, mount_fd_, MessagePumpForIO::FdWatchController::Mode::WRITE, this)) {
    ::close(mount_fd_);
    mount_fd_ = -1;
    return false;
  }
  return true;
}

void StorageMonitor::Impl::PlatformStop() {
  controller_.StopWatching();
  if (mount_fd_ >= 0) {
    ::close(mount_fd_);
    mount_fd_ = -1;
  }
}

std::vector<StorageInfo> StorageMonitor::Impl::PlatformScan() const {
  std::vector<StorageInfo> result;
  for (const MountEntry &m : ParseMountInfo(options_.mountinfo_path)) {
    StorageInfo info;
    if (BuildStorageInfo(m, options_, &info))
      result.push_back(std::move(info));
  }
  return result;
}

} // namespace nei

#endif // !_WIN32
