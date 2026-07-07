#pragma once

#ifndef NEIXX_NET_WSA_INIT_H_
#define NEIXX_NET_WSA_INIT_H_

// =============================================================================
// EnsureWsa  --  进程级 Winsock 一次性初始化
// =============================================================================
//
// Windows：使用 NoDestructor 保证 WSAStartup 仅调用一次，跳过 WSACleanup
//          （进程退出时 Windows 自动回收 Winsock 资源）。
// POSIX：空操作，无网络栈初始化需求。
//
// 线程安全：C++11 保证函数局部 static 初始化互斥。

#include <nei/macros/nei_export.h>

namespace nei::net {

#if defined(_WIN32)

// Call once on any thread before Winsock usage. Subsequent calls are no-ops.
NEI_API void EnsureWsa();

#else  // !_WIN32

inline void EnsureWsa() {}

#endif  // _WIN32

}  // namespace nei::net

#endif  // NEIXX_NET_WSA_INIT_H_
