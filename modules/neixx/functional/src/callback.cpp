// callback.cpp  --  explicit template instantiations for common signatures.
//
// OnceCallback / RepeatingCallback are now templates with all lifecycle
// methods inline in callback.h.  This file provides explicit instantiations
// for the most frequently used signatures to reduce code bloat across
// translation units.
//
// Add new signatures here as they become needed by the codebase.

#include <neixx/functional/callback.h>

namespace nei {
namespace detail {

// Single definition of the OnceCallback::Run() diagnostic counter.
// An inline variable (C++17) would have separate copies in the DLL and
// the benchmark EXE, making cross-module reads useless.
std::atomic<std::uint64_t> g_once_callback_run_count{0};

} // namespace detail

// void()  --  the universal PostTask / timer / general-purpose signature.
template class OnceCallback<void()>;
template class RepeatingCallback<void()>;

} // namespace nei
