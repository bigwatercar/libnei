#pragma once
#ifndef NEIXX_FUNCTIONAL_CALLBACK_INTERNAL_H_
#define NEIXX_FUNCTIONAL_CALLBACK_INTERNAL_H_
#include <cstddef>
#include <functional>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#include <nei/build/compiler_specific.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/memory/small_object_allocator.h>
#include <neixx/memory/unretained_wrapper.h>

namespace nei {
template <typename T>
class WeakPtr;
template <typename T>
class PassedWrapper;

template <typename Signature>
class OnceCallback;
template <typename Signature>
class RepeatingCallback;

namespace detail {

// -----------------------------------------------------------------------------
// Callback allocation policy switch.
//
//   NEI_CALLBACK_ALLOCATOR_USE_PARTITION = 1  -> small-object pool (nei::memory).
//   NEI_CALLBACK_ALLOCATOR_USE_PARTITION = 0  -> legacy plain operator new.
//
// The legacy path is intentionally kept so the two can be compared for
// stability/performance (flip the macro, rebuild, rerun the bench).
// NOTE: the macro must be identical across the library and all consumers.
// -----------------------------------------------------------------------------
#ifndef NEI_CALLBACK_ALLOCATOR_USE_PARTITION
#define NEI_CALLBACK_ALLOCATOR_USE_PARTITION 1
#endif

#if NEI_CALLBACK_ALLOCATOR_USE_PARTITION
inline void *callback_alloc(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
  return SmallObjectAlloc(bytes, alignment);
}

inline void callback_free(void *ptr, size_t alignment = alignof(std::max_align_t)) noexcept {
  (void)alignment;
  SmallObjectFree(ptr);
}
#else
inline void *callback_alloc(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
  if (alignment <= alignof(std::max_align_t))
    return ::operator new(bytes);
  return ::operator new(bytes, std::align_val_t(alignment));
}

inline void callback_free(void *ptr, size_t alignment = alignof(std::max_align_t)) noexcept {
  if (alignment <= alignof(std::max_align_t)) {
    ::operator delete(ptr);
    return;
  }
  ::operator delete(ptr, std::align_val_t(alignment));
}
#endif

// Ref-counted storage base, mirroring Chromium's base/callback_internal.h
// BindStateBase.  The ref count is thread-safe (atomic) so a BindState can be
// shared across threads by RepeatingCallback copies, and released by any thread
// (e.g. the worker thread that runs the task).
//
// operator new/delete are overloaded to route through the same partition
// allocator used by callback_alloc, so a BindState can also be created with a
// plain `new BindState<...>(...)` and freed with `delete` symmetrically.
class BindStateBase : public RefCountedThreadSafe<BindStateBase> {
public:
  virtual ~BindStateBase() = default;

#if NEI_CALLBACK_ALLOCATOR_USE_PARTITION
  static void *operator new(size_t size) {
    return SmallObjectAlloc(size, alignof(std::max_align_t));
  }

  static void operator delete(void *ptr) noexcept {
    SmallObjectFree(ptr);
  }

  static void operator delete(void *ptr, std::size_t) noexcept {
    SmallObjectFree(ptr);
  }

  // Over-aligned functors (alignof > max_align) are allocated via the aligned
  // operator new; route it through SmallObjectAlloc so the block header is
  // always present (the direct path stores it for align > 16).  The delete
  // side is covered by SmallObjectFree in the plain forms above.
  static void *operator new(std::size_t size, std::align_val_t align) {
    return SmallObjectAlloc(size, static_cast<std::size_t>(align));
  }

  static void operator delete(void *ptr, std::align_val_t) noexcept {
    SmallObjectFree(ptr);
  }

  static void operator delete(void *ptr, std::size_t, std::align_val_t) noexcept {
    SmallObjectFree(ptr);
  }

  // Declaring class-scope operator new hides the global placement forms, so
  // restore them for `new (ptr) BindState<...>(...)` placement construction.
  static void *operator new(std::size_t, void *ptr) noexcept {
    return ptr;
  }

  static void operator delete(void *, void *) noexcept {
  }
#endif
};

template <typename Fn, typename... BArgs>
class BindState : public BindStateBase {
public:
  template <typename F, typename... A>
  explicit BindState(F &&f, A &&...a)
      : fn_(std::forward<F>(f))
      , args_(std::forward<A>(a)...) {
  }

  NEI_SUPPRESS_MSC_WARNING_BEGIN(4324)
  Fn fn_;
  std::tuple<BArgs...> args_;
  NEI_SUPPRESS_MSC_WARNING_END()
};

// --- BindState construction / destruction helpers ---------------------------
// Route through the allocation policy:
//   Partition path -> plain `new`/`delete` (uses BindStateBase::operator
//   new/delete -> SmallObjectAlloc/Free).
//   Legacy path    -> callback_alloc + placement new + explicit dtor + free.
template <typename State, typename... A>
State *BindStateNew(A &&...a) {
#if NEI_CALLBACK_ALLOCATOR_USE_PARTITION
  return new State(std::forward<A>(a)...);
#else
  auto *s = static_cast<State *>(callback_alloc(sizeof(State), alignof(State)));
  new (s) State(std::forward<A>(a)...);
  return s;
#endif
}

template <typename State>
void BindStateDelete(State *s) {
  if (s == nullptr) {
    return;
  }
#if NEI_CALLBACK_ALLOCATOR_USE_PARTITION
  delete s;
#else
  s->~State();
  callback_free(s, alignof(State));
#endif
}

// UnwrapOnce: moves bound args (OnceCallback — args consumed once).
template <typename T>
struct UnwindOnce {
  using type = T &&;

  static type Unwind(T &v) noexcept {
    return std::move(v);
  }
};

template <typename T>
struct UnwindOnce<std::reference_wrapper<T>> {
  using type = T &;

  static type Unwind(std::reference_wrapper<T> &v) noexcept {
    return v.get();
  }
};

template <typename T>
using unwrap_once_t = typename UnwindOnce<std::remove_reference_t<T>>::type;

template <typename T>
unwrap_once_t<T> UnwrapOnce(T &&arg) noexcept {
  return UnwindOnce<std::remove_reference_t<T>>::Unwind(arg);
}

// UnwrapRepeat: passes bound args by const lvalue reference (RepeatingCallback
// — args persist across calls). reference_wrapper returns T& as the user
// explicitly opted into mutable referencing.
template <typename T>
struct UnwindRepeat {
  using type = const T &;

  static type Unwind(T &v) noexcept {
    return v;
  }
};

template <typename T>
struct UnwindRepeat<std::reference_wrapper<T>> {
  using type = T &;

  static type Unwind(std::reference_wrapper<T> &v) noexcept {
    return v.get();
  }
};

template <typename T>
using unwrap_repeat_t = typename UnwindRepeat<std::remove_reference_t<T>>::type;

template <typename T>
unwrap_repeat_t<T> UnwrapRepeat(T &&arg) noexcept {
  return UnwindRepeat<std::remove_reference_t<T>>::Unwind(arg);
}

// -----------------------------------------------------------------------------
// WeakPtrCheck — mirrors Chromium's base/bind_internal.h WeakPtrCheck.
//
// A bound WeakPtr whose target has been destroyed must cancel the whole
// callback at Run() time, NOT be dereferenced (dereferencing an invalidated
// WeakPtr yields a null target and an access violation).  Without this, a
// delayed task that was posted while the target was alive but runs after the
// target died (e.g. a RepeatingTimer tick whose owning timer was destroyed by
// another queued task) crashes with 0xc0000005.
// -----------------------------------------------------------------------------
template <typename T>
bool WeakPtrCheck(const T &) {
  return true;
}

template <typename T>
bool WeakPtrCheck(const nei::WeakPtr<T> &weak) {
  return static_cast<bool>(weak); // WeakPtr::operator bool == IsValid()
}

template <typename Tuple>
bool AllBoundArgsValid(const Tuple &args) {
  return std::apply([](const auto &...a) { return (WeakPtrCheck(a) && ...); }, args);
}

// is_once_callback / is_repeating_callback：判断 functor 是否为回调类型。
// （定义在 Invoker 之前：Invoker::RunImpl 通过依赖名查找使用它们。）
template <typename F>
struct is_once_callback : std::false_type {};

template <typename R, typename... Args>
struct is_once_callback<OnceCallback<R(Args...)>> : std::true_type {};

template <typename F>
struct is_repeating_callback : std::false_type {};

template <typename R, typename... Args>
struct is_repeating_callback<RepeatingCallback<R(Args...)>> : std::true_type {};

template <typename F>
constexpr bool is_once_callback_v = is_once_callback<F>::value;

template <typename F>
constexpr bool is_repeating_callback_v = is_repeating_callback<F>::value;

// Invoker<Storage, Sig, IsOnce> — dispatches to the correct unwind policy.
template <typename Storage, typename Sig, bool IsOnce>
struct Invoker;

template <typename Storage, typename R, typename... UA>
struct Invoker<Storage, R(UA...), true> {
  template <typename Fn, typename Tuple, size_t... I>
  static R RunImpl(Fn &fn, Tuple &args, std::index_sequence<I...>, UA... ua) {
    using FnT = std::remove_reference_t<Fn>;
    if constexpr (is_once_callback_v<FnT>) {
      // A callback used as the bound functor (BindOnce(cb, args...)) is
      // dispatched through Run(); the callback consumes itself on Run.
      return std::move(fn).Run(UnwrapOnce(std::get<I>(args))..., std::forward<UA>(ua)...);
    } else if constexpr (is_repeating_callback_v<FnT>) {
      return fn.Run(UnwrapOnce(std::get<I>(args))..., std::forward<UA>(ua)...);
    } else if constexpr (std::is_member_function_pointer_v<FnT>) {
      return std::invoke(fn, UnwrapOnce(std::get<I>(args))..., std::forward<UA>(ua)...);
    } else {
      return fn(UnwrapOnce(std::get<I>(args))..., std::forward<UA>(ua)...);
    }
  }

  static R Run(BindStateBase *base, UA... ua) {
    auto *s = static_cast<Storage *>(base);

    // The OnceCallback holds the only reference to the BindState.  Release it
    // after invoking — even if the functor throws — so the storage is freed.
    struct ScopedRelease {
      BindStateBase *state;

      ~ScopedRelease() {
        if (state) {
          state->Release();
        }
      }
    } scoped_release{s};

    // If any bound WeakPtr target has died, skip the call entirely rather than
    // dereferencing the invalidated WeakPtr (Chromium semantics).  The state
    // is still released by scoped_release.
    if (!AllBoundArgsValid(s->args_)) {
      return R();
    }
    return RunImpl(
        s->fn_, s->args_, std::make_index_sequence<std::tuple_size_v<decltype(s->args_)>>{}, std::forward<UA>(ua)...);
  }
};

template <typename Storage, typename R, typename... UA>
struct Invoker<Storage, R(UA...), false> {
  template <typename Fn, typename Tuple, size_t... I>
  static R RunImpl(Fn &fn, Tuple &args, std::index_sequence<I...>, UA... ua) {
    using FnT = std::remove_reference_t<Fn>;
    if constexpr (is_repeating_callback_v<FnT>) {
      return fn.Run(UnwrapRepeat(std::get<I>(args))..., std::forward<UA>(ua)...);
    } else if constexpr (std::is_member_function_pointer_v<FnT>) {
      return std::invoke(fn, UnwrapRepeat(std::get<I>(args))..., std::forward<UA>(ua)...);
    } else {
      return fn(UnwrapRepeat(std::get<I>(args))..., std::forward<UA>(ua)...);
    }
  }

  static R Run(BindStateBase *base, UA... ua) {
    auto *s = static_cast<Storage *>(base);
    // Skip the call if any bound WeakPtr target has died (Chromium semantics).
    if (!AllBoundArgsValid(s->args_)) {
      return R();
    }
    return RunImpl(
        s->fn_, s->args_, std::make_index_sequence<std::tuple_size_v<decltype(s->args_)>>{}, std::forward<UA>(ua)...);
  }
};

template <typename T>
struct is_weak_ptr : std::false_type {};

template <typename T>
struct is_weak_ptr<nei::WeakPtr<T>> : std::true_type {};

template <typename T>
constexpr bool is_weak_ptr_v = is_weak_ptr<T>::value;

template <typename T>
struct is_unretained_wrapper : std::false_type {};

template <typename T>
struct is_unretained_wrapper<nei::UnretainedWrapper<T>> : std::true_type {};

template <typename T>
constexpr bool is_unretained_wrapper_v = is_unretained_wrapper<T>::value;

template <typename T>
struct is_passed_wrapper : std::false_type {};

template <typename T>
struct is_passed_wrapper<nei::PassedWrapper<T>> : std::true_type {};

template <typename T>
constexpr bool is_passed_wrapper_v = is_passed_wrapper<T>::value;

template <typename T>
struct bind_arg_storage_impl {
  using type = std::decay_t<T>;

  template <typename U>
  static type Store(U &&v) {
    return std::forward<U>(v);
  }
};

template <typename T>
struct bind_arg_storage_impl<nei::UnretainedWrapper<T>> {
  using type = T *;

  static type Store(nei::UnretainedWrapper<T> v) {
    return v.get();
  }
};

template <typename T>
struct bind_arg_storage_impl<nei::PassedWrapper<T>> {
  using type = std::decay_t<T>;

  static type Store(nei::PassedWrapper<T> v) {
    return v.Take();
  }
};

template <typename T>
struct bind_arg_storage : bind_arg_storage_impl<std::remove_cv_t<std::remove_reference_t<T>>> {};

template <typename T>
using bind_arg_storage_t = typename bind_arg_storage<std::remove_cv_t<std::remove_reference_t<T>>>::type;

template <typename T>
bind_arg_storage_t<T> StoreBoundArg(T &&v) {
  return bind_arg_storage<T>::Store(std::forward<T>(v));
}

// -----------------------------------------------------------------------------
// FunctorRunType — 提取 functor 的完整调用签名
// (Chromium base/bind_internal.h FunctorTraits::RunType 的 libnei 等价物)。
//
// 输出统一为函数类型 `R(Args...)`。functor 的几种形态：
//   * 自由函数指针 / 引用                 -> R(Args...)
//   * 成员函数指针                        -> R(C*, Args...)（receiver 位于参数
//                                          首位，与 BindOnce 把 receiver 作为
//                                          第一个绑定参数、Invoker 用 std::invoke
//                                          调用的语义一致）
//   * lambda / std::function / 函数对象   -> R(Args...)（经 operator() 提取，
//                                          不含隐式 receiver）
//   * OnceCallback / RepeatingCallback    -> R(Args...)，运行时用 .Run() 派发
// -----------------------------------------------------------------------------

// 从 operator() 提取纯调用签名（用于 lambda / 函数对象）。覆盖 const / ref /
// noexcept 限定的全部组合。
template <typename Callable, typename Signature = decltype(&std::decay_t<Callable>::operator())>
struct CallableRunType;

// MSVC 不接受空实参宏调用（C4003），用 NEIXX_EMPTY 占位并在展开时置空。
#define NEIXX_EMPTY
#define NEIXX_DEFINE_CALLABLE_RUN_TYPE(QUALIFIERS)                                                                     \
  template <typename Callable, typename R, typename C, typename... Args>                                               \
  struct CallableRunType<Callable, R (C::*)(Args...) QUALIFIERS> {                                                     \
    using type = R(Args...);                                                                                           \
  };

NEIXX_DEFINE_CALLABLE_RUN_TYPE(NEIXX_EMPTY)
NEIXX_DEFINE_CALLABLE_RUN_TYPE(const)
NEIXX_DEFINE_CALLABLE_RUN_TYPE(&)
NEIXX_DEFINE_CALLABLE_RUN_TYPE(const &)
NEIXX_DEFINE_CALLABLE_RUN_TYPE(&&)
NEIXX_DEFINE_CALLABLE_RUN_TYPE(const &&)
#if defined(__cpp_noexcept_function_type)
NEIXX_DEFINE_CALLABLE_RUN_TYPE(noexcept)
NEIXX_DEFINE_CALLABLE_RUN_TYPE(const noexcept)
NEIXX_DEFINE_CALLABLE_RUN_TYPE(& noexcept)
NEIXX_DEFINE_CALLABLE_RUN_TYPE(const & noexcept)
NEIXX_DEFINE_CALLABLE_RUN_TYPE(&& noexcept)
NEIXX_DEFINE_CALLABLE_RUN_TYPE(const && noexcept)
#endif

#undef NEIXX_DEFINE_CALLABLE_RUN_TYPE

// 泛型 functor（lambda / std::function / 函数对象）：经 operator() 提取。
template <typename F>
struct FunctorRunType {
  using type = typename CallableRunType<std::decay_t<F>>::type;
};

// 自由函数指针 / 引用（含 noexcept 变体）。
template <typename R, typename... Args>
struct FunctorRunType<R (*)(Args...)> {
  using type = R(Args...);
};
#if defined(__cpp_noexcept_function_type)
template <typename R, typename... Args>
struct FunctorRunType<R (*)(Args...) noexcept> {
  using type = R(Args...);
};
#endif
template <typename R, typename... Args>
struct FunctorRunType<R (&)(Args...)> {
  using type = R(Args...);
};
#if defined(__cpp_noexcept_function_type)
template <typename R, typename... Args>
struct FunctorRunType<R (&)(Args...) noexcept> {
  using type = R(Args...);
};
#endif

// 成员函数指针：receiver 成为第一个调用参数。覆盖 const / ref / noexcept。
#define NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(CV, REF, NOEX)                                                            \
  template <typename R, typename C, typename... Args>                                                                  \
  struct FunctorRunType<R (C::*)(Args...) CV REF NOEX> {                                                               \
    using type = R(CV C *, Args...);                                                                                   \
  };

NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(NEIXX_EMPTY, NEIXX_EMPTY, NEIXX_EMPTY)
NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(const, NEIXX_EMPTY, NEIXX_EMPTY)
NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(NEIXX_EMPTY, &, NEIXX_EMPTY)
NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(const, &, NEIXX_EMPTY)
NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(NEIXX_EMPTY, &&, NEIXX_EMPTY)
NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(const, &&, NEIXX_EMPTY)
#if defined(__cpp_noexcept_function_type)
NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(NEIXX_EMPTY, NEIXX_EMPTY, noexcept)
NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(const, NEIXX_EMPTY, noexcept)
NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(NEIXX_EMPTY, &, noexcept)
NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(const, &, noexcept)
NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(NEIXX_EMPTY, &&, noexcept)
NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE(const, &&, noexcept)
#endif

#undef NEIXX_DEFINE_MEMBER_FUNCTOR_RUN_TYPE
#undef NEIXX_EMPTY

// 回调类型作为 functor。
template <typename R, typename... Args>
struct FunctorRunType<OnceCallback<R(Args...)>> {
  using type = R(Args...);
};

template <typename R, typename... Args>
struct FunctorRunType<RepeatingCallback<R(Args...)>> {
  using type = R(Args...);
};

// has_functor_run_type：判断能否静态提取完整调用签名（路径 1 可用），从而决定
// MakeUnboundRunTypeImpl 走“签名裁剪”还是“bound args 推断”。
//
// 注意：不能通过探测“typename FunctorRunType<F>::type 是否存在”实现——MSVC 对
// 类模板实例化内部（默认模板实参中的 decltype(&F::operator())）的 SFINAE 支持
// 不完整，会在 void_t 探测里把泛型 lambda 的 operator() 提取错误硬报出来。
// 因此改用：
//   * has_extractable_operator：直接 decltype(&F::operator())（泛型 lambda /
//     重载 operator() 在此失败）；
//   * 对无 operator() 但有 FunctorRunType 特化的类型（函数指针/引用、成员
//     函数指针、回调）显式判为 true。
template <typename F, typename = void>
struct has_extractable_operator : std::false_type {};

template <typename F>
struct has_extractable_operator<F, std::void_t<decltype(&std::decay_t<F>::operator())>> : std::true_type {};

template <typename F>
struct has_functor_run_type
    : std::bool_constant<
          has_extractable_operator<F>::value || std::is_function_v<std::remove_reference_t<std::remove_pointer_t<F>>>
          || std::is_member_function_pointer_v<F> || is_once_callback_v<F> || is_repeating_callback_v<F>> {};

template <typename F>
constexpr bool has_functor_run_type_v = has_functor_run_type<F>::value;

// -----------------------------------------------------------------------------
// BindTypeHelper / MakeUnboundRunType — 计算未绑定签名。
//
// 从完整签名 R(FullArgs...) 丢弃前 sizeof...(BoundArgs) 个参数得到
// R(UnboundArgs...)。对成员函数指针，receiver 占据 FullArgs 首位，因此自然被
// 计入“已绑定参数”——与 BindOnce(&C::Method, receiver, args...) 的调用语义
// 一致。返回类型 R 原样保留。
//   BindOnce([](int a, int b) { return a + b; }, 10)  -> OnceCallback<int(int)>
//   BindOnce(&C::Method, &obj)                        -> OnceCallback<R()>
// -----------------------------------------------------------------------------
// MakeFunctionType<R, tuple<Args...>> -> R(Args...)
template <typename R, typename Tuple>
struct MakeFunctionType;

template <typename R, typename... Args>
struct MakeFunctionType<R, std::tuple<Args...>> {
  using type = R(Args...);
};

template <typename Signature, typename... BoundArgs>
struct BindTypeHelper;

template <typename R, typename... FullArgs, typename... BoundArgs>
struct BindTypeHelper<R(FullArgs...), BoundArgs...> {
  static constexpr size_t kBoundCount = sizeof...(BoundArgs);
  static_assert(sizeof...(FullArgs) >= sizeof...(BoundArgs),
                "BindOnce/BindRepeating: 绑定的参数多于 functor 接受的参数");

  // 丢弃 tuple 前 k 个元素，保留尾部（未绑定参数）。
  template <size_t k, typename Tuple>
  struct Tail;

  template <size_t k, typename... Ts>
  struct Tail<k, std::tuple<Ts...>> {
    static_assert(k <= sizeof...(Ts), "Tail: k out of range");
    template <size_t... I>
    static auto Take(std::index_sequence<I...>) -> std::tuple<std::tuple_element_t<k + I, std::tuple<Ts...>>...>;
    using type = decltype(Take(std::make_index_sequence<sizeof...(Ts) - k>{}));
  };

  using UnboundRunType = typename MakeFunctionType<R, typename Tail<kBoundCount, std::tuple<FullArgs...>>::type>::type;
};

// 泛型 lambda / 重载 operator() 的 fallback：用 bound args 实例化调用推断返回
// 类型，未绑定签名为 R()（丢弃全部 bound args）。仅当 bound args 能实例化
// functor 的调用时可用（无绑定参数、或完全绑定）。
template <typename F, typename... BA>
struct BoundArgsInferredRunType {
  using ReturnType = decltype(std::declval<F>()(std::declval<BA>()...));
  using UnboundRunType = typename MakeFunctionType<ReturnType, std::tuple<>>::type;
};

// 按 has_functor_run_type_v 分派：路径 1（operator() 可提取）用完整签名裁剪出
// 未绑定参数；路径 2（泛型 lambda / 重载 operator()）退回 bound args 推断。
// 用类模板部分特化而非函数重载：UnboundRunType 是函数类型，不能作为函数返回
// 类型，只能放在 using 别名里。分派 bool 置于参数列表最前（模板参数包不能
// 后跟非包参数，MSVC 严格要求）。
template <bool Extractable, typename F, typename... BA>
struct MakeUnboundRunTypeImpl;

template <typename F, typename... BA>
struct MakeUnboundRunTypeImpl<true, F, BA...> {
  using type = typename BindTypeHelper<typename FunctorRunType<F>::type, std::decay_t<BA>...>::UnboundRunType;
};

template <typename F, typename... BA>
struct MakeUnboundRunTypeImpl<false, F, BA...> {
  using type = typename BoundArgsInferredRunType<F, std::decay_t<BA>...>::UnboundRunType;
};

template <typename F, typename... BA>
using MakeUnboundRunType = typename MakeUnboundRunTypeImpl<has_functor_run_type_v<F>, F, BA...>::type;
} // namespace detail

template <typename T>
class PassedWrapper {
public:
  explicit PassedWrapper(T &&t)
      : value_(std::move(t)) {
  }

  PassedWrapper(const PassedWrapper &) = delete;
  PassedWrapper &operator=(const PassedWrapper &) = delete;
  PassedWrapper(PassedWrapper &&) = default;
  PassedWrapper &operator=(PassedWrapper &&) = default;

  T Take() {
    return std::move(value_);
  }

private:
  T value_;
};

template <typename T>
PassedWrapper<T> Passed(T &&v) {
  return PassedWrapper<T>(std::forward<T>(v));
}
} // namespace nei
#endif