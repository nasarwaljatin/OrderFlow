#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "orderflow/object_pool.hpp"

using orderflow::ObjectPool;

namespace {
struct Widget {
    int a;
    double b;
};
}  // namespace

TEST(ObjectPool, StartsEmptyOfUsedSlots) {
    ObjectPool<Widget> pool(4);
    EXPECT_EQ(pool.capacity(), 4u);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_FALSE(pool.empty());
}

TEST(ObjectPool, AcquireConstructsObject) {
    ObjectPool<Widget> pool(2);
    Widget* w = pool.acquire(Widget{7, 1.5});
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->a, 7);
    EXPECT_DOUBLE_EQ(w->b, 1.5);
    EXPECT_EQ(pool.in_use(), 1u);
}

TEST(ObjectPool, ReturnsNullWhenExhausted) {
    ObjectPool<Widget> pool(2);
    EXPECT_NE(pool.acquire(), nullptr);
    EXPECT_NE(pool.acquire(), nullptr);
    EXPECT_TRUE(pool.empty());
    EXPECT_EQ(pool.acquire(), nullptr);
}

TEST(ObjectPool, ReleasedSlotIsReused) {
    ObjectPool<Widget> pool(1);
    Widget* a = pool.acquire();
    pool.release(a);
    Widget* b = pool.acquire();
    EXPECT_EQ(a, b);  // LIFO free list -> cache-hot slot comes back first
}

TEST(ObjectPool, AllSlotsAreDistinct) {
    ObjectPool<Widget> pool(64);
    std::set<Widget*> seen;
    for (int i = 0; i < 64; ++i) seen.insert(pool.acquire());
    EXPECT_EQ(seen.size(), 64u);
}

TEST(ObjectPool, ChurnKeepsAccountingCorrect) {
    ObjectPool<Widget> pool(16);
    std::vector<Widget*> live;
    for (int round = 0; round < 1000; ++round) {
        if (live.size() < 16 && (round % 3 != 0 || live.empty())) {
            live.push_back(pool.acquire(Widget{round, 0}));
        } else {
            pool.release(live.back());
            live.pop_back();
        }
        ASSERT_EQ(pool.in_use(), live.size());
    }
}

TEST(ObjectPool, ZeroCapacityIsAlwaysEmpty) {
    ObjectPool<Widget> pool(0);
    EXPECT_TRUE(pool.empty());
    EXPECT_EQ(pool.acquire(), nullptr);
}
