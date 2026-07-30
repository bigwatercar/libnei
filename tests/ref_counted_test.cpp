#include <gtest/gtest.h>

#include <memory>

#include <neixx/memory/ref_counted.h>

namespace nei {
namespace {

class SequenceBoundObject final : public RefCounted<SequenceBoundObject> {
public:
  explicit SequenceBoundObject(int *destroyed_count)
      : destroyed_count_(destroyed_count) {
  }

  int value() const {
    return value_;
  }

  void set_value(int value) {
    value_ = value;
  }

private:
  friend class RefCounted<SequenceBoundObject>;

  ~SequenceBoundObject() {
    if (destroyed_count_ != nullptr) {
      ++(*destroyed_count_);
    }
  }

  int *destroyed_count_ = nullptr;
  int value_ = 0;
};

TEST(RefCountedTest, ScopedRefptrWorksWithNonThreadSafeBase) {
  int destroyed_count = 0;

  {
    scoped_refptr<SequenceBoundObject> object(new SequenceBoundObject(&destroyed_count));
    EXPECT_TRUE(object);
    object->set_value(7);

    scoped_refptr<SequenceBoundObject> copy = object;
    EXPECT_EQ(copy->value(), 7);

    object.reset();
    EXPECT_EQ(destroyed_count, 0);

    copy.reset();
    EXPECT_EQ(destroyed_count, 1);
  }

  EXPECT_EQ(destroyed_count, 1);
}

TEST(RefCountedTest, MakeRefCountedWorksWithNonThreadSafeBase) {
  int destroyed_count = 0;

  {
    scoped_refptr<SequenceBoundObject> object = MakeRefCounted<SequenceBoundObject>(&destroyed_count);
    EXPECT_TRUE(object);

    scoped_refptr<SequenceBoundObject> copy = object;
    copy->set_value(42);
    EXPECT_EQ(object->value(), 42);
  }

  EXPECT_EQ(destroyed_count, 1);
}

} // namespace
} // namespace nei