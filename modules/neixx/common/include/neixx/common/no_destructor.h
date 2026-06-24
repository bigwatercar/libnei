#pragma once

#ifndef NEIXX_COMMON_NO_DESTRUCTOR_H_
#define NEIXX_COMMON_NO_DESTRUCTOR_H_

#include <new>
#include <type_traits>
#include <utility>

namespace nei {

// ===========================================================================
// NoDestructor<T> — 永不析构的存储包装器
// ===========================================================================
//
// 对标 Chromium `base::NoDestructor<T>`，提供一种轻量的包装，保证被包装的
// T 实例在进程整个生命周期内**永不析构**。这是比 LeakySingletonTraits 更
// 底层、更直接的防关机崩溃方案。
//
// ## 定位
//
//   Singleton<T, Traits>  = 单例容器 (管理访问控制 + 懒初始化 + 退出清理)
//   NoDestructor<T>       = 存储策略 (管理内存布局 + 永不析构)
//
// 两者互补而非互斥：NoDestructor 可以替代 Singleton 中不需要 AtExit 集成的
// 场景，也可以作为函数局部 static 变量实现带参构造的懒单例。
//
// ## 为什么永不析构是安全的
//
//   析构函数不运行意味着：
//     - T 持有的 OS 资源（文件句柄、socket 等）由 OS 在进程退出时回收
//     - T 自身的栈/数据段内存在进程虚拟地址空间释放时整体回收
//     - 残存后台线程在退出期间访问 T 时不会触发 use-after-free
//
//   这正是 Chromium Leaky 模式的核心思想：宁可泄露进程生命周期对象，
//   也不在退出期冒 use-after-free 崩溃的风险。
//
// ## 线程安全
//
//   NoDestructor **本身不提供线程安全保证**——构造在声明点完成。如果你需要
//   多线程安全的懒初始化，使用 C++11 函数局部 static：
//
//     MyClass& Get() {
//       static NoDestructor<MyClass> s(arg1, arg2);
//       return *s;
//     }
//
//   C++11 标准保证函数局部 static 的初始化是线程安全的（magic statics）。
//
// ## 使用示例
//
//   1) 文件作用域全局变量：
//        namespace { NoDestructor<MyService> g_svc("host", 8080); }
//        MyService& GetMyService() { return *g_svc; }
//
//   2) 函数局部 static + 带参构造（最常用）：
//        MyClass& GetInstance() {
//          static NoDestructor<MyClass> s(config_path);
//          return *s;
//        }
//
//   3) 容器类型：
//        static NoDestructor<std::vector<std::string>> g_origins({"a", "b"});
//        g_origins->size();  // 2
//
// ## 与 Singleton<T> 的选择
//
//   | 场景                                    | 推荐                         |
//   |-----------------------------------------|-----------------------------|
//   | 需要 AtExit 受控销毁                      | Singleton<T, Default...>    |
//   | 需要退出后依然可安全访问（防关机 UAF）       | Singleton<T, Leaky...>      |
//   | 需要带参构造                              | NoDestructor<T>             |
//   | 需要带参构造 + AtExit 清理                | NoDestructor + 手动注册回调   |
//
template <typename T>
class NoDestructor {
 public:
  // -----------------------------------------------------------------------
  // Construction — 完美转发任意构造参数
  // -----------------------------------------------------------------------
  //
  // 支持 T 的任意构造函数签名，包括：
  //   - 默认构造:    NoDestructor<T> obj;
  //   - 单参构造:    NoDestructor<T> obj(42);
  //   - 多参构造:    NoDestructor<T> obj("host", 8080, true);
  //   - 初始化列表:   NoDestructor<T> obj({1, 2, 3});
  //   - 移动语义:    NoDestructor<T> obj(std::move(existing));
  //
  template <typename... Args>
  explicit NoDestructor(Args&&... args) {
    // Placement-new onto the aligned raw storage.
    // Uses parenthesized syntax () rather than braced {} to match Chromium
    // behaviour and the majority of construction scenarios.
    new (storage_) T(std::forward<Args>(args)...);
  }

  // -----------------------------------------------------------------------
  // Destruction — 故意留空（核心语义）
  // -----------------------------------------------------------------------
  //
  // 析构函数**不调用 ~T()**。这是 NoDestructor 区别于任何 RAII 包装器的
  // 根本差异：T 的析构函数永远不会运行，所有资源由 OS 在进程退出时回收。
  ~NoDestructor() = default;

  // -----------------------------------------------------------------------
  // 禁止拷贝和移动
  // -----------------------------------------------------------------------
  //
  // NoDestructor 持有原地构造的对象，拷贝/移动在语义上无意义。如果确实需要
  // 转移所有权，说明不应该使用 NoDestructor——考虑 std::unique_ptr 或
  // Singleton 容器。
  NoDestructor(const NoDestructor&) = delete;
  NoDestructor& operator=(const NoDestructor&) = delete;
  NoDestructor(NoDestructor&&) = delete;
  NoDestructor& operator=(NoDestructor&&) = delete;

  // -----------------------------------------------------------------------
  // 访问器
  // -----------------------------------------------------------------------

  // 指针语法访问，类似智能指针。
  T* operator->() { return ptr(); }
  const T* operator->() const { return ptr(); }

  // 引用语法访问。
  T& operator*() { return *ptr(); }
  const T& operator*() const { return *ptr(); }

  // 显式 getter（偏好显式 API 时使用）。
  T* get() { return ptr(); }
  const T* get() const { return ptr(); }

 private:
  // 返回指向已构造 T 的指针。
  //
  // 使用 std::launder 满足 C++17 严格别名规则：
  //   storage_ 的声明类型是 unsigned char[]，但其中存活的对象是 T。
  //   不 launder 的情况下编译器可能基于"unsigned char 不能别名 T"
  //   做出错误优化（实践中极少触发但标准要求）。Chromium 实现同样
  //   使用 std::launder。
  T* ptr() {
    return std::launder(reinterpret_cast<T*>(storage_));
  }
  const T* ptr() const {
    return std::launder(reinterpret_cast<const T*>(storage_));
  }

  // 对齐到 T 的原始字节存储。
  // 对象在此存储上通过 placement new 原地构造，不涉及堆分配。
  alignas(T) unsigned char storage_[sizeof(T)];
};

}  // namespace nei

#endif  // NEIXX_COMMON_NO_DESTRUCTOR_H_
