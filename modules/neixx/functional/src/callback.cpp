#include <neixx/functional/callback.h>

namespace nei {
namespace detail {
std::atomic<std::uint64_t> g_once_callback_run_count{0};
std::atomic<std::uint64_t> g_once_callback_heap_count{0};
} // namespace detail
template class OnceCallback<void()>;
template class RepeatingCallback<void()>;
} // namespace nei