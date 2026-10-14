#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <thread>

#include "orderflow/spsc_ring.hpp"
#include "orderflow/types.hpp"

using orderflow::SpscRing;

TEST(SpscRing, RejectsNonPowerOfTwoCapacity) {
    EXPECT_THROW(SpscRing<int>(3), std::invalid_argument);
    EXPECT_THROW(SpscRing<int>(0), std::invalid_argument);
    EXPECT_NO_THROW(SpscRing<int>(8));
}

TEST(SpscRing, PopFromEmptyFails) {
    SpscRing<int> r(4);
    int v = 0;
    EXPECT_FALSE(r.try_pop(v));
}

TEST(SpscRing, FifoOrder) {
    SpscRing<int> r(8);
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(r.try_push(i));
    for (int i = 0; i < 5; ++i) {
        int v = -1;
        ASSERT_TRUE(r.try_pop(v));
        EXPECT_EQ(v, i);
    }
}

TEST(SpscRing, PushToFullFails) {
    SpscRing<int> r(4);
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(r.try_push(i));
    EXPECT_FALSE(r.try_push(99));
    int v;
    ASSERT_TRUE(r.try_pop(v));
    EXPECT_TRUE(r.try_push(99));  // space freed
}

TEST(SpscRing, WrapsAroundManyTimes) {
    SpscRing<int> r(4);
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(r.try_push(i));
        int v;
        ASSERT_TRUE(r.try_pop(v));
        ASSERT_EQ(v, i);
    }
    EXPECT_EQ(r.size_approx(), 0u);
}

TEST(SpscRing, SizeApproxTracksContents) {
    SpscRing<int> r(8);
    r.try_push(1);
    r.try_push(2);
    EXPECT_EQ(r.size_approx(), 2u);
}

TEST(SpscRing, CarriesCommandsByValue) {
    SpscRing<orderflow::Command> r(4);
    const auto c = orderflow::Command::limit(3, 77, orderflow::Side::Sell, 1234, 9);
    ASSERT_TRUE(r.try_push(c));
    orderflow::Command out;
    ASSERT_TRUE(r.try_pop(out));
    EXPECT_EQ(out.id, 77u);
    EXPECT_EQ(out.price, 1234);
    EXPECT_EQ(out.symbol, 3u);
}

// The real test: two threads, millions of items, nothing lost or reordered.
TEST(SpscRing, TwoThreadStressPreservesOrderAndCount) {
    constexpr std::uint64_t kItems = 2'000'000;
    SpscRing<std::uint64_t> r(1024);
    std::uint64_t sum = 0;
    bool in_order = true;

    std::thread consumer([&] {
        std::uint64_t expected = 0;
        while (expected < kItems) {
            std::uint64_t v;
            if (r.try_pop(v)) {
                if (v != expected) in_order = false;
                sum += v;
                ++expected;
            }
        }
    });
    for (std::uint64_t i = 0; i < kItems; ++i)
        while (!r.try_push(i)) std::this_thread::yield();
    consumer.join();

    EXPECT_TRUE(in_order);
    EXPECT_EQ(sum, kItems * (kItems - 1) / 2);
}
