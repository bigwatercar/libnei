#ifndef NEIXX_IO_IO_CONTEXT_INTERNAL_H_
#define NEIXX_IO_IO_CONTEXT_INTERNAL_H_

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace nei {

#if defined(_WIN32)
struct IOOverlappedBase {
  OVERLAPPED overlapped;
  void (*on_complete)(IOOverlappedBase *self, DWORD bytes_transferred, DWORD error_code);
};
#endif

} // namespace nei

#endif // NEIXX_IO_IO_CONTEXT_INTERNAL_H_
