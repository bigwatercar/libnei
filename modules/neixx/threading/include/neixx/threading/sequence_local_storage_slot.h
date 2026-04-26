#pragma once

#ifndef NEI_THREADING_SEQUENCE_LOCAL_STORAGE_SLOT_H
#define NEI_THREADING_SEQUENCE_LOCAL_STORAGE_SLOT_H

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include <nei/macros/nei_export.h>

namespace nei {

class NEI_API SequenceLocalStorageMap final {
public:
  class Impl;

  SequenceLocalStorageMap();
  ~SequenceLocalStorageMap();

  SequenceLocalStorageMap(const SequenceLocalStorageMap &) = delete;
  SequenceLocalStorageMap &operator=(const SequenceLocalStorageMap &) = delete;
  SequenceLocalStorageMap(SequenceLocalStorageMap &&) noexcept;
  SequenceLocalStorageMap &operator=(SequenceLocalStorageMap &&) noexcept;

  bool HasValue(std::size_t key) const;
  std::shared_ptr<void> GetValue(std::size_t key) const;
  void SetValue(std::size_t key, std::shared_ptr<void> value);
  void ClearValue(std::size_t key);

  static SequenceLocalStorageMap *GetForCurrentThread();
  static SequenceLocalStorageMap *SwapCurrentForThread(SequenceLocalStorageMap *map);

private:
  static std::size_t AllocateSlotKey();

  template <typename T>
  friend class SequenceLocalStorageSlot;

  std::unique_ptr<Impl> impl_;
};

template <typename T>
class SequenceLocalStorageSlot final {
public:
  SequenceLocalStorageSlot()
      : key_(SequenceLocalStorageMap::AllocateSlotKey()) {
    static_assert(!std::is_reference<T>::value, "T must not be a reference type");
  }

  SequenceLocalStorageSlot(const SequenceLocalStorageSlot &) = delete;
  SequenceLocalStorageSlot &operator=(const SequenceLocalStorageSlot &) = delete;

  bool has_value() const {
    SequenceLocalStorageMap *map = SequenceLocalStorageMap::GetForCurrentThread();
    if (map == nullptr) {
      return false;
    }
    return map->HasValue(key_);
  }

  T *Get() const {
    std::shared_ptr<T> value = GetShared();
    return value.get();
  }

  std::shared_ptr<T> GetShared() const {
    SequenceLocalStorageMap *map = SequenceLocalStorageMap::GetForCurrentThread();
    if (map == nullptr) {
      return nullptr;
    }
    std::shared_ptr<void> value = map->GetValue(key_);
    return std::static_pointer_cast<T>(value);
  }

  void Set(std::shared_ptr<T> value) {
    SequenceLocalStorageMap *map = SequenceLocalStorageMap::GetForCurrentThread();
    if (map == nullptr) {
      return;
    }
    map->SetValue(key_, std::static_pointer_cast<void>(std::move(value)));
  }

  void Set(std::unique_ptr<T> value) {
    Set(std::shared_ptr<T>(std::move(value)));
  }

  template <typename... Args>
  T *Emplace(Args &&...args) {
    std::shared_ptr<T> value = std::make_shared<T>(std::forward<Args>(args)...);
    T *raw = value.get();
    Set(std::move(value));
    return raw;
  }

  void Reset() {
    SequenceLocalStorageMap *map = SequenceLocalStorageMap::GetForCurrentThread();
    if (map == nullptr) {
      return;
    }
    map->ClearValue(key_);
  }

private:
  const std::size_t key_;
};

} // namespace nei

#endif // NEI_THREADING_SEQUENCE_LOCAL_STORAGE_SLOT_H
