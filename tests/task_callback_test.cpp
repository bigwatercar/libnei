#include <gtest/gtest.h>

#include <memory>

#include <neixx/task/callback.h>

TEST(TaskCallbackTest, OnceCallbackRunsAtMostOnce) {
    int count = 0;
    nei::OnceCallback cb = nei::BindOnce(
        [&](int delta) {
            count += delta;
        },
        3);

    std::move(cb).Run();
    std::move(cb).Run();

    EXPECT_EQ(count, 3);
}

TEST(TaskCallbackTest, OnceCallbackSupportsMoveOnlyArguments) {
    int out = 0;
    nei::OnceCallback cb = nei::BindOnce(
        [](std::unique_ptr<int> value, int& out_ref) {
            out_ref = *value;
        },
        std::make_unique<int>(42),
        std::ref(out));

    std::move(cb).Run();

    EXPECT_EQ(out, 42);
}

TEST(TaskCallbackTest, RepeatingCallbackRunsMultipleTimes) {
    int count = 0;
    nei::RepeatingCallback cb = nei::BindRepeating(
        [&](int delta) {
            count += delta;
        },
        2);

    cb.Run();
    cb.Run();
    cb.Run();

    EXPECT_EQ(count, 6);
}

TEST(TaskCallbackTest, RepeatingCallbackIsCopyable) {
    int count = 0;
    nei::RepeatingCallback cb = nei::BindRepeating(
        [&](int delta) {
            count += delta;
        },
        1);

    nei::RepeatingCallback copied = cb;

    cb.Run();
    copied.Run();

    EXPECT_EQ(count, 2);
}

TEST(TaskCallbackTest, RepeatingCallbackSupportsReferenceBinding) {
    int value = 5;
    nei::RepeatingCallback cb = nei::BindRepeating(
        [](int& target, int delta) {
            target += delta;
        },
        std::ref(value),
        4);

    cb.Run();
    cb.Run();

    EXPECT_EQ(value, 13);
}

TEST(TaskCallbackTest, RepeatingCallbackCanHoldMoveOnlyState) {
    int sum = 0;
    nei::RepeatingCallback cb = nei::BindRepeating(
        [](std::unique_ptr<int>& value, int& sum_ref) {
            sum_ref += *value;
        },
        std::make_unique<int>(7),
        std::ref(sum));

    cb.Run();
    cb.Run();

    EXPECT_EQ(sum, 14);
}
