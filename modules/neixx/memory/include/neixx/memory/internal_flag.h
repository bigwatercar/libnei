#pragma once

#ifndef NEIXX_MEMORY_INTERNAL_FLAG_H_
#define NEIXX_MEMORY_INTERNAL_FLAG_H_

#include <memory>

#include <nei/macros/nei_export.h>

namespace nei {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

// Shared validity flag used by WeakPtrFactory/WeakPtr.
// Invalidated (once) when the factory is destroyed.
class NEI_API InternalFlag final {
public:
  class Impl;

  InternalFlag();
  ~InternalFlag();

  InternalFlag(const InternalFlag &) = delete;
  InternalFlag &operator=(const InternalFlag &) = delete;

  bool IsValid() const;
  void Invalidate();

private:
  std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

#endif // NEIXX_MEMORY_INTERNAL_FLAG_H_
