// Implementation of the deprecated ThreadLocalStorage API.
// Suppress deprecation warnings since this is the implementation itself.
#include <nei/macros/suppress_compiler_warnings.h>
NEI_SUPPRESS_MSC_WARNING_BEGIN(4996)

#include <neixx/threading/thread_local_storage.h>
#include "internal/tls_slot.h"

namespace nei {

class ThreadLocalStorage::Slot::Impl {
public:
  Impl()
      : index_(-1) {
  }

  explicit Impl(TLSDestructorFunc d)
      : index_(-1) {
    index_ = internal::InternalAllocateSlot(d);
    if (index_ >= 0)
      initialized_ = true;
  }

  ~Impl() {
    if (index_ >= 0)
      internal::InternalFreeSlot(index_);
  }

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  bool Initialize(TLSDestructorFunc d) {
    if (initialized_)
      return false;
    index_ = internal::InternalAllocateSlot(d);
    if (index_ >= 0)
      initialized_ = true;
    return index_ >= 0;
  }

  bool InitializeAsLongLived(TLSDestructorFunc d) {
    if (initialized_)
      return false;
    index_ = internal::InternalAllocateLongLivedSlot(d);
    if (index_ >= 0)
      initialized_ = true;
    return index_ >= 0;
  }

  bool initialized() const {
    return initialized_;
  }

  void *Get() const {
    return internal::InternalGetSlotValue(index_);
  }

  void Set(void *value) {
    internal::InternalSetSlotValue(index_, value);
  }

private:
  int index_;
  bool initialized_ = false;
};

ThreadLocalStorage::Slot::Slot()
    : impl_(std::make_unique<Impl>()) {
}

ThreadLocalStorage::Slot::Slot(TLSDestructorFunc destructor)
    : impl_(std::make_unique<Impl>(destructor)) {
}

ThreadLocalStorage::Slot::~Slot() = default;

ThreadLocalStorage::Slot::Slot(Slot &&) noexcept = default;
ThreadLocalStorage::Slot &ThreadLocalStorage::Slot::operator=(Slot &&) noexcept = default;

bool ThreadLocalStorage::Slot::Initialize(TLSDestructorFunc destructor) {
  return impl_->Initialize(destructor);
}

bool ThreadLocalStorage::Slot::InitializeAsLongLived(TLSDestructorFunc destructor) {
  return impl_->InitializeAsLongLived(destructor);
}

bool ThreadLocalStorage::Slot::initialized() const {
  return impl_->initialized();
}

void *ThreadLocalStorage::Slot::Get() const {
  return impl_->Get();
}

void ThreadLocalStorage::Slot::Set(void *value) {
  impl_->Set(value);
}

// ---- Iterator --------------------------------------------------------------

class ThreadLocalStorage::Iterator::Impl {
public:
  Impl()
      : slot_index_(-1) {
    AdvanceToNext();
  }

  bool IsAtEnd() const {
    return slot_index_ >= internal::kMaxSlots;
  }

  void Advance() {
    ++slot_index_;
    AdvanceToNext();
  }

  void *Get() const {
    if (IsAtEnd())
      return nullptr;
    return internal::InternalGetSlotValue(slot_index_);
  }

private:
  void AdvanceToNext() {
    while (slot_index_ < internal::kMaxSlots) {
      if (internal::InternalIsSlotActive(slot_index_))
        return;
      ++slot_index_;
    }
  }

  int slot_index_;
};

ThreadLocalStorage::Iterator::Iterator()
    : impl_(std::make_unique<Impl>()) {
}

ThreadLocalStorage::Iterator::~Iterator() = default;

bool ThreadLocalStorage::Iterator::IsAtEnd() const {
  return impl_->IsAtEnd();
}

void ThreadLocalStorage::Iterator::Advance() {
  impl_->Advance();
}

void *ThreadLocalStorage::Iterator::Get() const {
  return impl_->Get();
}

} // namespace nei

NEI_SUPPRESS_MSC_WARNING_END()
