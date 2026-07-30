#ifndef NEIXX_MEMORY_UNRETAINED_WRAPPER_H_
#define NEIXX_MEMORY_UNRETAINED_WRAPPER_H_

#include <nei/macros/nei_export.h>

namespace nei {

template <typename T>
class NEI_API UnretainedWrapper final {
public:
  explicit UnretainedWrapper(T *ptr)
      : ptr_(ptr) {
  }

  T *get() const {
    return ptr_;
  }

private:
  T *ptr_ = nullptr;
};

template <typename T>
inline UnretainedWrapper<T> Unretained(T *ptr) {
  return UnretainedWrapper<T>(ptr);
}

} // namespace nei

#endif // NEIXX_MEMORY_UNRETAINED_WRAPPER_H_
