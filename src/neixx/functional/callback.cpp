#include <neixx/functional/callback.h>

namespace nei {
namespace detail {
std::atomic<std::uint64_t> g_once_callback_run_count{0};
std::atomic<std::uint64_t> g_once_callback_heap_count{0};
} // namespace detail
// Explicit instantiations exported from the DLL.  Consumers see the matching
// `extern template` declarations in callback.h, so they link against these
// rather than instantiating their own inline copies.
template class NEI_API OnceCallback<void()>;
template class NEI_API RepeatingCallback<void()>;
} // namespace nei