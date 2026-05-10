#pragma once

#ifndef NEIXX_MEMORY_INTERNAL_FLAG_H_
#define NEIXX_MEMORY_INTERNAL_FLAG_H_

#include <atomic>

#include <nei/macros/nei_export.h>
#include <neixx/memory/ref_counted.h>

namespace nei {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

// Shared validity flag used by WeakPtrFactory/WeakPtr.
// Invalidated (once) when the factory is destroyed.
class NEI_API InternalFlag final : public RefCountedThreadSafe<InternalFlag> {
public:
  InternalFlag();
  ~InternalFlag();

  InternalFlag(const InternalFlag &) = delete;
  InternalFlag &operator=(const InternalFlag &) = delete;

  bool IsValid() const;
  void Invalidate();

private:
  std::atomic<bool> valid_{true};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

#endif // NEIXX_MEMORY_INTERNAL_FLAG_H_
