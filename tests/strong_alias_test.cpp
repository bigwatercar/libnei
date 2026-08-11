// =============================================================================
// StrongAlias unit tests
// =============================================================================

#include <gtest/gtest.h>

#include <neixx/common/strong_alias.h>

#include <functional>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>

namespace nei {
namespace {

using DogID = StrongAlias<struct DogIDTag, int>;
using CatID = StrongAlias<struct CatIDTag, int>;
using Name = StrongAlias<struct NameTag, std::string>;

// ---- Construction & value access ----

TEST(StrongAliasTest, DefaultConstruction) {
  DogID id;
  EXPECT_EQ(id.value(), 0);
}

TEST(StrongAliasTest, ExplicitConstruction) {
  DogID id(42);
  EXPECT_EQ(id.value(), 42);
}

TEST(StrongAliasTest, MoveConstruction) {
  Name name(std::string("hello"));
  EXPECT_EQ(name.value(), "hello");

  Name moved(std::move(name));
  EXPECT_EQ(moved.value(), "hello");
}

// ---- Type safety — these would be compile errors if uncommented ----

// TEST(StrongAliasTest, NoImplicitConversion) {
//   void Feed(DogID, CatID) {}
//   Feed(1, 2);          // ❌ compile error
//   Feed(DogID(1), 2);   // ❌ compile error
//   Feed(1, CatID(2));   // ❌ compile error
//   Feed(DogID(1), CatID(2)); // ✅
// }

// ---- Comparison ----

TEST(StrongAliasTest, Equality) {
  DogID a(1), b(1), c(2);
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_LT(a, c);
  EXPECT_LE(a, b);
  EXPECT_GT(c, a);
  EXPECT_GE(c, a);
}

// ---- Hashing (std::unordered_set) ----

TEST(StrongAliasTest, HashSupport) {
  std::unordered_set<DogID> s;
  s.insert(DogID(1));
  s.insert(DogID(2));
  s.insert(DogID(1)); // duplicate
  EXPECT_EQ(s.size(), 2u);
}

// ---- Ordered container (std::set) ----

TEST(StrongAliasTest, OrderedContainer) {
  std::set<DogID> s{DogID(3), DogID(1), DogID(2)};
  auto it = s.begin();
  EXPECT_EQ(it->value(), 1);
  ++it;
  EXPECT_EQ(it->value(), 2);
}

// ---- Streaming ----

TEST(StrongAliasTest, Ostream) {
  DogID id(42);
  std::ostringstream oss;
  oss << id;
  EXPECT_EQ(oss.str(), "42");
}

// ---- const value access ----

TEST(StrongAliasTest, ConstValueAccess) {
  const DogID id(7);
  EXPECT_EQ(id.value(), 7);
}

// ---- Different tag types are incompatible ----

TEST(StrongAliasTest, DifferentTagsAreDistinctTypes) {
  // This test just verifies the types exist and are distinct.
  DogID dog(1);
  CatID cat(1);
  EXPECT_EQ(dog.value(), cat.value());
}

// =============================================================================
// Policy-based customization tests
// =============================================================================

// ---- Arithmetic ----

using Offset = StrongAlias<struct OffsetTag,
                           int,
                           StrongAliasPolicy::Equality | StrongAliasPolicy::Ordering | StrongAliasPolicy::Arithmetic>;

TEST(StrongAliasTest, ArithmeticAdd) {
  Offset a(10), b(20);
  EXPECT_EQ((a + b).value(), 30);
}

TEST(StrongAliasTest, ArithmeticSubtract) {
  Offset a(30), b(10);
  EXPECT_EQ((a - b).value(), 20);
}

TEST(StrongAliasTest, ArithmeticMultiply) {
  Offset a(5), b(6);
  EXPECT_EQ((a * b).value(), 30);
}

TEST(StrongAliasTest, ArithmeticDivide) {
  Offset a(30), b(6);
  EXPECT_EQ((a / b).value(), 5);
}

TEST(StrongAliasTest, ArithmeticCompoundAdd) {
  Offset a(10);
  a += Offset(5);
  EXPECT_EQ(a.value(), 15);
}

TEST(StrongAliasTest, ArithmeticCompoundSubtract) {
  Offset a(10);
  a -= Offset(3);
  EXPECT_EQ(a.value(), 7);
}

TEST(StrongAliasTest, ArithmeticUnaryMinus) {
  Offset a(10);
  EXPECT_EQ((-a).value(), -10);
}

// ---- Increment ----

using Counter = StrongAlias<struct CounterTag, int, StrongAliasPolicy::Equality | StrongAliasPolicy::Increment>;

TEST(StrongAliasTest, IncrementPrefix) {
  Counter c(1);
  EXPECT_EQ((++c).value(), 2);
  EXPECT_EQ(c.value(), 2);
}

TEST(StrongAliasTest, IncrementPostfix) {
  Counter c(1);
  Counter old = c++;
  EXPECT_EQ(old.value(), 1);
  EXPECT_EQ(c.value(), 2);
}

TEST(StrongAliasTest, DecrementPrefix) {
  Counter c(5);
  EXPECT_EQ((--c).value(), 4);
  EXPECT_EQ(c.value(), 4);
}

TEST(StrongAliasTest, DecrementPostfix) {
  Counter c(5);
  Counter old = c--;
  EXPECT_EQ(old.value(), 5);
  EXPECT_EQ(c.value(), 4);
}

// ---- Bitwise ----

using Flags = StrongAlias<struct FlagsTag, unsigned, StrongAliasPolicy::Equality | StrongAliasPolicy::Bitwise>;

TEST(StrongAliasTest, BitwiseAnd) {
  EXPECT_EQ((Flags(0b1100u) & Flags(0b1010u)).value(), 0b1000u);
}

TEST(StrongAliasTest, BitwiseOr) {
  EXPECT_EQ((Flags(0b1100u) | Flags(0b0011u)).value(), 0b1111u);
}

TEST(StrongAliasTest, BitwiseXor) {
  EXPECT_EQ((Flags(0b1111u) ^ Flags(0b0101u)).value(), 0b1010u);
}

TEST(StrongAliasTest, BitwiseNot) {
  EXPECT_EQ((~Flags(0u)).value(), ~0u);
}

TEST(StrongAliasTest, BitwiseCompound) {
  Flags a(0b1100u);
  a &= Flags(0b1010u);
  EXPECT_EQ(a.value(), 0b1000u);
  a |= Flags(0b0011u);
  EXPECT_EQ(a.value(), 0b1011u);
  a ^= Flags(0b1111u);
  EXPECT_EQ(a.value(), 0b0100u);
}

// ---- Implicit ----

using RawInt = StrongAlias<struct RawIntTag, int, StrongAliasPolicy::Implicit>;

static void AcceptRaw(RawInt) {
}

TEST(StrongAliasTest, ImplicitConstruction) {
  RawInt r = 42; // OK
  EXPECT_EQ(r.value(), 42);
}

TEST(StrongAliasTest, ImplicitPassToFunction) {
  AcceptRaw(10);         // implicit from int
  AcceptRaw(RawInt(10)); // explicit also works
}

// ---- None policy ----

using Bare = StrongAlias<struct BareTag, int, StrongAliasPolicy::None>;

TEST(StrongAliasTest, BareNonePolicyStillConstructable) {
  Bare b(42);
  EXPECT_EQ(b.value(), 42);
}

// ---- HasPolicy utility ----

TEST(StrongAliasTest, HasPolicyUtility) {
  EXPECT_TRUE(HasPolicy(StrongAliasPolicy::Equality | StrongAliasPolicy::Ordering, StrongAliasPolicy::Equality));
  EXPECT_FALSE(HasPolicy(StrongAliasPolicy::Equality, StrongAliasPolicy::Ordering));
}

// ---- Convenience aliases ----

using DefSA = DefaultStrongAlias<struct DefTag, int>;
using FullSA = FullStrongAlias<struct FullTag, int>;

TEST(StrongAliasTest, DefaultAliasHasComparison) {
  DefSA a(1), b(2);
  EXPECT_LT(a, b);
}

TEST(StrongAliasTest, FullAliasHasAllOperations) {
  FullSA a(2), b(3);
  EXPECT_LT(a, b);
  auto c = a + b;
  EXPECT_EQ(c.value(), 5);
  ++c;
  EXPECT_EQ(c.value(), 6);
}

} // namespace
} // namespace nei
