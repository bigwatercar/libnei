#include <neixx/memory/ref_counted.h>

namespace {

struct NotRefCounted {
  int value = 0;
};

} // namespace

int main() {
  // Intentional compile-fail case: NotRefCounted lacks AddRef/Release.
  // Build target `ref_counted_compile_fail` manually to inspect diagnostics.
  nei::scoped_refptr<NotRefCounted> ptr;
  return static_cast<int>(ptr.get() != nullptr);
}
