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

// void()  --  the universal PostTask / timer / general-purpose signature.
template class OnceCallback<>;
template class RepeatingCallback<>;

}  // namespace nei
