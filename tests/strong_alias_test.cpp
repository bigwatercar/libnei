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
  // The real proof is that swapping DogID/CatID in a function call
  // produces a compile error.
  DogID dog(1);
  CatID cat(1);
  EXPECT_EQ(dog.value(), cat.value()); // same int value, different types
}

} // namespace
} // namespace nei
