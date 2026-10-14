#if defined(_WIN32)

#include <windows.h>

#include <cfgmgr32.h>
#include <dbt.h>
#include <winioctl.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/strings/utf_string_conversions.h>

#include "storage_monitor_impl.h"

namespace nei {
namespace {

// GUID_DEVINTERFACE_VOLUME, spelled out to avoid needing INITGUID.
constexpr GUID kVolumeInterfaceGuid = {0x53f5630d, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

// A volume interface can appear before its drive letter is assigned, so an
// event is followed by a second scan after this delay.
constexpr int kSettleDelayMs = 1000;

using CmRegisterNotificationFn = CONFIGRET(WINAPI *)(PCM_NOTIFY_FILTER, PVOID, PCM_NOTIFY_CALLBACK, PHCMNOTIFICATION);
using CmUnregisterNotificationFn = CONFIGRET(WINAPI *)(HCMNOTIFICATION);

std::string WideToUtf8(const wchar_t *s) {
  if (s == nullptr || *s == L'\0')
    return {};
  return UTF16ToUTF8(std::u16string_view(reinterpret_cast<const char16_t *>(s), std::wcslen(s)));
}

std::string TrimAscii(const char *s) {
  std::string out(s);
  const auto b = out.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return {};
  return out.substr(b, out.find_last_not_of(" \t\r\n") - b + 1);
}

// Keeps a missing floppy/optical medium from raising a modal "no disk" dialog.
class ScopedFailCriticalErrors {
public:
  ScopedFailCriticalErrors() {
    ::SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode_);
  }

  ~ScopedFailCriticalErrors() {
    ::SetThreadErrorMode(old_mode_, nullptr);
  }

private:
  DWORD old_mode_ = 0;
};

struct DeviceProperties {
  STORAGE_BUS_TYPE bus_type = BusTypeUnknown;
  std::string vendor;
  std::string model;
};

DeviceProperties QueryDeviceProperties(wchar_t drive_letter) {
  DeviceProperties props;
  const wchar_t path[] = {L'\\', L'\\', L'.', L'\\', drive_letter, L':', L'\0'};
  HANDLE h = ::CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return props;

  STORAGE_PROPERTY_QUERY query = {};
  query.PropertyId = StorageDeviceProperty;
  query.QueryType = PropertyStandardQuery;
  alignas(STORAGE_DEVICE_DESCRIPTOR) char buf[1024] = {};
  DWORD returned = 0;
  if (::DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buf, sizeof(buf), &returned, nullptr)
      && returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
    const auto *desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buf);
    props.bus_type = desc->BusType;
    // Offsets are from the start of the buffer; 0 means absent.
    if (desc->VendorIdOffset != 0 && desc->VendorIdOffset < sizeof(buf))
      props.vendor = TrimAscii(buf + desc->VendorIdOffset);
    if (desc->ProductIdOffset != 0 && desc->ProductIdOffset < sizeof(buf))
      props.model = TrimAscii(buf + desc->ProductIdOffset);
  }
  ::CloseHandle(h);
  return props;
}

bool QueryDrive(wchar_t letter, StorageInfo *out) {
  const wchar_t root[] = {letter, L':', L'\\', L'\0'};
  const UINT drive_type = ::GetDriveTypeW(root);
  if (drive_type != DRIVE_REMOVABLE && drive_type != DRIVE_FIXED && drive_type != DRIVE_CDROM)
    return false;

  wchar_t label[MAX_PATH + 1] = {};
  DWORD serial = 0;
  // Fails for drives without media; those are not reported.
  if (!::GetVolumeInformationW(root, label, MAX_PATH + 1, &serial, nullptr, nullptr, nullptr, 0))
    return false;

  const DeviceProperties props = QueryDeviceProperties(letter);
  const bool removable = drive_type != DRIVE_FIXED || props.bus_type == BusTypeUsb || props.bus_type == BusType1394
                         || props.bus_type == BusTypeSd || props.bus_type == BusTypeMmc;

  wchar_t volume_name[64] = {};
  if (::GetVolumeNameForVolumeMountPointW(root, volume_name, 64)) {
    // "\\?\Volume{GUID}\" -> "{GUID}"
    const std::wstring v(volume_name);
    const auto open = v.find(L'{');
    const auto close = v.find(L'}');
    if (open != std::wstring::npos && close != std::wstring::npos)
      out->device_id = "volume:" + WideToUtf8(v.substr(open, close - open + 1).c_str());
  }
  if (out->device_id.empty()) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "serial:%08lX", static_cast<unsigned long>(serial));
    out->device_id = buf;
  }

  ULARGE_INTEGER total = {};
  ::GetDiskFreeSpaceExW(root, nullptr, &total, nullptr);

  out->type = removable ? StorageInfo::Type::kRemovableMassStorage : StorageInfo::Type::kFixedMassStorage;
  out->location = WideToUtf8(root);
  out->label = WideToUtf8(label);
  out->vendor = props.vendor;
  out->model = props.model;
  out->total_size_bytes = total.QuadPart;
  return true;
}

} // namespace

// Owns the OS change-notification registration.  Events arrive on foreign
// threads and are bounced to the monitor sequence; |impl| is only touched on
// that sequence and cleared in Stop().
struct StorageMonitor::Impl::WinNotifier : std::enable_shared_from_this<WinNotifier> {
  WinNotifier(scoped_refptr<SingleThreadTaskRunner> task_runner, Impl *owner)
      : runner(std::move(task_runner))
      , impl(owner) {
  }

  ~WinNotifier() {
    DCHECK(cm_handle == nullptr && !window_thread.joinable());
  }

  bool StartCm() {
    cfgmgr = ::LoadLibraryExW(L"cfgmgr32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (cfgmgr == nullptr)
      return false;
    // CM_Register_Notification is Win8+; resolving it at run time keeps the
    // library loadable on Windows 7.
    register_fn = reinterpret_cast<CmRegisterNotificationFn>(::GetProcAddress(cfgmgr, "CM_Register_Notification"));
    unregister_fn =
        reinterpret_cast<CmUnregisterNotificationFn>(::GetProcAddress(cfgmgr, "CM_Unregister_Notification"));
    if (register_fn == nullptr || unregister_fn == nullptr) {
      ReleaseCm();
      return false;
    }

    CM_NOTIFY_FILTER filter = {};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = kVolumeInterfaceGuid;
    if (register_fn(&filter, this, &WinNotifier::CmCallback, &cm_handle) != CR_SUCCESS) {
      cm_handle = nullptr;
      ReleaseCm();
      return false;
    }
    return true;
  }

  bool StartWindow() {
    std::promise<bool> ready;
    std::future<bool> ready_future = ready.get_future();
    window_thread = std::thread([this, &ready]() { WindowThreadMain(&ready); });
    if (!ready_future.get()) {
      window_thread.join();
      return false;
    }
    return true;
  }

  void Stop() {
    impl = nullptr;
    if (cm_handle != nullptr) {
      // Blocks until in-flight callbacks return; they never wait on us.
      unregister_fn(cm_handle);
      cm_handle = nullptr;
      ReleaseCm();
    }
    if (window_thread.joinable()) {
      ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
      window_thread.join();
      hwnd = nullptr;
    }
  }

  // Any thread.
  void NotifyFromAnyThread() {
    auto self = shared_from_this();
    auto rescan = [self]() {
      if (self->impl != nullptr)
        self->impl->OnPlatformEvent();
    };
    runner->PostTask(FROM_HERE, rescan);
    runner->PostDelayedTask(FROM_HERE, rescan, TimeDelta::FromMilliseconds(kSettleDelayMs));
  }

  scoped_refptr<SingleThreadTaskRunner> runner;
  Impl *impl;

private:
  void ReleaseCm() {
    register_fn = nullptr;
    unregister_fn = nullptr;
    if (cfgmgr != nullptr) {
      ::FreeLibrary(cfgmgr);
      cfgmgr = nullptr;
    }
  }

  static DWORD CALLBACK
  CmCallback(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD) {
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
      static_cast<WinNotifier *>(context)->NotifyFromAnyThread();
    return ERROR_SUCCESS;
  }

  static LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_DEVICECHANGE:
      if (wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE || wparam == DBT_DEVNODES_CHANGED) {
        auto *self = reinterpret_cast<WinNotifier *>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self != nullptr)
          self->NotifyFromAnyThread();
      }
      return TRUE;
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
    default:
      return ::DefWindowProcW(window, msg, wparam, lparam);
    }
  }

  // Win7 path.  Volume broadcasts are delivered to top-level windows only, so
  // this is deliberately not a message-only window.
  void WindowThreadMain(std::promise<bool> *ready) {
    wchar_t class_name[64];
    std::swprintf(class_name, 64, L"NeiStorageMonitor_%p", static_cast<void *>(this));

    HINSTANCE module = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&WinNotifier::WindowProc),
                         &module);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WinNotifier::WindowProc;
    wc.hInstance = module;
    wc.lpszClassName = class_name;
    if (::RegisterClassExW(&wc) == 0) {
      ready->set_value(false);
      return;
    }

    HWND window = ::CreateWindowExW(0, class_name, L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, module, nullptr);
    if (window == nullptr) {
      ::UnregisterClassW(class_name, module);
      ready->set_value(false);
      return;
    }
    ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    hwnd = window;
    ready->set_value(true);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
    ::UnregisterClassW(class_name, module);
  }

  HMODULE cfgmgr = nullptr;
  CmRegisterNotificationFn register_fn = nullptr;
  CmUnregisterNotificationFn unregister_fn = nullptr;
  HCMNOTIFICATION cm_handle = nullptr;

  std::thread window_thread;
  HWND hwnd = nullptr;
};

bool StorageMonitor::Impl::PlatformStart() {
  if (!options_.watch_changes)
    return true;

  auto notifier = std::make_shared<WinNotifier>(task_runner_, this);
  const bool ok = (!options_.use_legacy_window_notifications && notifier->StartCm()) || notifier->StartWindow();
  if (!ok)
    return false;
  win_notifier_ = std::move(notifier);
  return true;
}

void StorageMonitor::Impl::PlatformStop() {
  if (win_notifier_ == nullptr)
    return;
  win_notifier_->Stop();
  win_notifier_.reset();
}

std::vector<StorageInfo> StorageMonitor::Impl::PlatformScan() const {
  std::vector<StorageInfo> result;
  ScopedFailCriticalErrors no_dialogs;
  const DWORD drives = ::GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if ((drives & (1u << i)) == 0)
      continue;
    StorageInfo info;
    if (QueryDrive(static_cast<wchar_t>(L'A' + i), &info))
      result.push_back(std::move(info));
  }
  return result;
}

} // namespace nei

#endif // _WIN32
