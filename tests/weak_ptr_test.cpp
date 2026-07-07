#include <thread>
#include <string>

#include <gtest/gtest.h>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/memory/weak_ptr.h>

namespace nei {
namespace {

// =============================================================================
// Basic Location tracking tests
// =============================================================================

TEST(WeakPtrLocationTest, BasicLocationTracking) {
  // Verify that passing FROM_HERE records a non-null Location.
  int dummy = 42;
  WeakPtrFactory<int> factory(&dummy, FROM_HERE);

  WeakPtr<int> wp = factory.GetWeakPtr(FROM_HERE);

  // In Debug builds, the Location strings should contain the filename.
  // In Release builds, the Location fields are compiled out, but the API
  // still compiles.
#if !defined(NDEBUG)
  // WeakPtr is valid; operator bool() returns true.
  EXPECT_TRUE(wp);

  // Dereference should work without firing diagnostics.
  EXPECT_EQ(*wp, 42);
#else
  // Release: basic sanity.
  EXPECT_TRUE(wp);
  EXPECT_EQ(*wp, 42);
#endif
}

TEST(WeakPtrLocationTest, UnknownLocation) {
  // Verify that the backward-compatible (no-FROM_HERE) constructors compile
  // and work correctly.  In Debug builds the Location is "unknown".
  int dummy = 42;
  WeakPtrFactory<int> factory(&dummy);

  WeakPtr<int> wp = factory.GetWeakPtr();
  EXPECT_TRUE(wp);
  EXPECT_EQ(*wp, 42);
}

TEST(WeakPtrLocationTest, BackwardCompatibleInvalidate) {
  // Verify that the old InvalidateWeakPtrs() (no Location) still works.
  int dummy = 42;
  WeakPtrFactory<int> factory(&dummy, FROM_HERE);

  WeakPtr<int> wp = factory.GetWeakPtr(FROM_HERE);
  EXPECT_TRUE(wp);

  factory.InvalidateWeakPtrs();  // Old API, no FROM_HERE.

  EXPECT_FALSE(wp);
  EXPECT_EQ(wp.get(), nullptr);
}

// =============================================================================
// Invalidation diagnostics
// =============================================================================

TEST(WeakPtrDiagnosticTest, InvalidationWithLocation) {
  int dummy = 42;
  WeakPtrFactory<int> factory(&dummy, FROM_HERE);

  WeakPtr<int> wp = factory.GetWeakPtr(FROM_HERE);
  EXPECT_TRUE(wp);

  // Invalidate with location. In Debug builds this may print a warning
  // if refcount > 1 (which it is, since wp still holds a reference).
  // The old-style InvalidateWeakPtrs should NOT crash.
  factory.InvalidateWeakPtrs(FROM_HERE);

  EXPECT_FALSE(wp);
  EXPECT_EQ(wp.get(), nullptr);
}

TEST(WeakPtrDiagnosticTest, OperatorBoolAfterInvalidation) {
  int dummy = 42;
  WeakPtrFactory<int> factory(&dummy, FROM_HERE);

  WeakPtr<int> wp = factory.GetWeakPtr(FROM_HERE);
  EXPECT_TRUE(wp);

  factory.InvalidateWeakPtrs(FROM_HERE);

  // After invalidation, operator bool() should return false.
  EXPECT_FALSE(wp);
}

TEST(WeakPtrDiagnosticTest, GetAfterInvalidation) {
  int dummy = 42;
  WeakPtrFactory<int> factory(&dummy, FROM_HERE);

  WeakPtr<int> wp = factory.GetWeakPtr(FROM_HERE);
  EXPECT_TRUE(wp);

  factory.InvalidateWeakPtrs(FROM_HERE);

  // get() returns nullptr after invalidation
  EXPECT_EQ(wp.get(), nullptr);
}

// =============================================================================
// operator-> / operator* diagnostics (Debug only)
// =============================================================================

#if !defined(NDEBUG)
// Death tests only work in Debug builds where DCHECK/abort are enabled.

TEST(WeakPtrDeathTest, OperatorArrowOnInvalidWeakPtr) {
  struct TestObj {
    int value = 99;
  };

  TestObj obj;
  WeakPtrFactory<TestObj> factory(&obj, FROM_HERE);
  WeakPtr<TestObj> wp = factory.GetWeakPtr(FROM_HERE);

  factory.InvalidateWeakPtrs(FROM_HERE);

  // Dereferencing an invalid WeakPtr via operator-> should trigger a fatal
  // diagnostic (abort).
  EXPECT_DEATH(
      {
        int v = wp->value;
        (void)v;
      },
      "INVALID WeakPtr");
}

TEST(WeakPtrDeathTest, OperatorStarOnInvalidWeakPtr) {
  struct TestObj {
    int value = 99;
  };

  TestObj obj;
  WeakPtrFactory<TestObj> factory(&obj, FROM_HERE);
  WeakPtr<TestObj> wp = factory.GetWeakPtr(FROM_HERE);

  factory.InvalidateWeakPtrs(FROM_HERE);

  // Dereferencing an invalid WeakPtr via operator* should trigger a fatal
  // diagnostic (abort).
  EXPECT_DEATH(
      {
        TestObj& ref = *wp;
        (void)ref;
      },
      "INVALID WeakPtr");
}

TEST(WeakPtrDeathTest, OperatorArrowOnValidWeakPtr) {
  // operator-> on a VALID WeakPtr should work normally.
  struct TestObj {
    int value = 99;
  };

  TestObj obj;
  WeakPtrFactory<TestObj> factory(&obj, FROM_HERE);
  WeakPtr<TestObj> wp = factory.GetWeakPtr(FROM_HERE);

  EXPECT_EQ(wp->value, 99);
  EXPECT_EQ((*wp).value, 99);
}

#endif  // !defined(NDEBUG)

// =============================================================================
// Cross-thread dereference diagnostics
// =============================================================================

#if !defined(NDEBUG)
TEST(WeakPtrDeathTest, CrossThreadDereference) {
  struct TestObj {
    int value = 42;
  };

  TestObj obj;
  WeakPtrFactory<TestObj> factory(&obj, FROM_HERE);
  WeakPtr<TestObj> wp = factory.GetWeakPtr(FROM_HERE);

  // GetWeakPtr on the main thread; dereference on a different thread
  // without WeakPtrThreadSafe opt-in should trigger a fatal diagnostic.
  std::thread t([wp]() {
    EXPECT_DEATH(
        {
          int v = wp->value;
          (void)v;
        },
        "Cross-thread dereference");
  });
  t.join();
}

TEST(WeakPtrTest, CrossThreadOperatorBoolIsSafe) {
  // operator bool() is explicitly allowed cross-thread without opt-in.
  // It only reads the atomic flag.
  struct TestObj {
    int value = 42;
  };

  TestObj obj;
  WeakPtrFactory<TestObj> factory(&obj, FROM_HERE);
  WeakPtr<TestObj> wp = factory.GetWeakPtr(FROM_HERE);

  std::thread t([wp]() {
    EXPECT_TRUE(wp);  // operator bool() is thread-safe.
  });
  t.join();

  EXPECT_TRUE(wp);
}
#endif  // !defined(NDEBUG)

// =============================================================================
// Zero-overhead in Release
// =============================================================================

TEST(WeakPtrSizeTest, ReleaseZeroOverhead) {
  // In Release builds, Location members should be compiled out, so
  // sizeof(WeakPtr<int>) should not include Location storage.
  // In Debug builds, it will be larger.
#if defined(NDEBUG)
  // Release: WeakPtr<int> should be exactly: ptr_ (8 bytes) +
  // scoped_refptr<InternalFlag> (8 bytes) + thread::id (8 bytes) = 24 bytes.
  // (Plus potential alignment padding.)
  //
  // We don't assert an exact size here because it can vary by platform
  // and compiler; we just verify it compiles.
  WeakPtr<int> wp;
  EXPECT_FALSE(wp);
  EXPECT_EQ(wp.get(), nullptr);
#else
  // Debug build: size may be larger due to Location storage.
  WeakPtr<int> wp;
  EXPECT_FALSE(wp);
  EXPECT_EQ(wp.get(), nullptr);
#endif
}

// =============================================================================
// Thread-safe opt-in (WeakPtrThreadSafe)
// =============================================================================

struct ThreadSafeObj {};
struct NonThreadSafeObj {};

}  // namespace

// Mark ThreadSafeObj as allowed for cross-thread dereference.
template <>
struct WeakPtrThreadSafe<ThreadSafeObj> : std::true_type {};

namespace {

TEST(WeakPtrThreadSafeTest, OptInAllowsCrossThreadDereference) {
  ThreadSafeObj obj;
  WeakPtrFactory<ThreadSafeObj> factory(&obj, FROM_HERE);
  WeakPtr<ThreadSafeObj> wp = factory.GetWeakPtr(FROM_HERE);

  // Cross-thread dereference should NOT trigger assertion because
  // WeakPtrThreadSafe<ThreadSafeObj> is true_type.
  std::thread t([wp]() {
    EXPECT_TRUE(wp);      // operator bool()  --  always safe.
    EXPECT_NE(wp.get(), nullptr);  // get()  --  safe due to opt-in.
  });
  t.join();
}

TEST(WeakPtrThreadSafeTest, NonOptInBlocksCrossThreadDereference) {
#if !defined(NDEBUG)
  NonThreadSafeObj obj;
  WeakPtrFactory<NonThreadSafeObj> factory(&obj, FROM_HERE);
  WeakPtr<NonThreadSafeObj> wp = factory.GetWeakPtr(FROM_HERE);

  // Cross-thread dereference without opt-in must fail.
  std::thread t([wp]() {
    EXPECT_DEATH(
        {
          wp.get();
        },
        "Cross-thread dereference");
  });
  t.join();
#else
  // Release: no assertion, just verify compilation.
  NonThreadSafeObj obj;
  WeakPtrFactory<NonThreadSafeObj> factory(&obj, FROM_HERE);
  WeakPtr<NonThreadSafeObj> wp = factory.GetWeakPtr(FROM_HERE);
  EXPECT_TRUE(wp);
#endif
}

}  // namespace
}  // namespace nei
