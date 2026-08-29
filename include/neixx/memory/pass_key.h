#ifndef NEIXX_MEMORY_PASS_KEY_H_
#define NEIXX_MEMORY_PASS_KEY_H_

#include <nei/build/nei_export.h>

namespace nei {

template <typename T>
class NEI_API PassKey final {
private:
  friend T;
  PassKey() = default;
};

} // namespace nei

#endif // NEIXX_MEMORY_PASS_KEY_H_
