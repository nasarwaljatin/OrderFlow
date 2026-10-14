// Engine-level validation, rejection paths and bookkeeping.
#include "test_helpers.hpp"

using namespace orderflow;
using namespace orderflow::test;

using ValidationTest = EngineTest;

TEST_F(ValidationTest, ZeroQuantityIsRejected) {
    buy(1, 100, 0);
    ASSERT_EQ(events().size(), 1u);
    EXPECT_EQ(events()[0], make_event(EventType::Rejected, Reason::InvalidQty, 1, 0));
    EXPECT_FALSE(engine_.is_live(1));
}

TEST_F(ValidationTest, ZeroPriceLimitIsRejected) {
    buy(1, 0, 10);
    EXPECT_EQ(events()[0].reason, Reason::InvalidPrice);
}

TEST_F(ValidationTest, NegativePriceIsRejected) {
    sell(1, -5, 10);
    EXPECT_EQ(events()[0].reason, Reason::InvalidPrice);
}

TEST_F(ValidationTest, ZeroPriceIocIsRejected) {
    send(OrderType::IOC, Side::Buy, 1, 0, 10);
    EXPECT_EQ(events()[0].reason, Reason::InvalidPrice);
}

TEST_F(ValidationTest, ZeroPriceFokIsRejected) {
    send(OrderType::FOK, Side::Buy, 1, 0, 10);
    EXPECT_EQ(events()[0].reason, Reason::InvalidPrice);
}

TEST_F(ValidationTest, ZeroPriceMarketIsAccepted) {
    send(OrderType::Market, Side::Buy, 1, 0, 10);
    EXPECT_EQ(events()[0].type, EventType::Accepted);
}

TEST_F(ValidationTest, DuplicateLiveIdIsRejected) {
    buy(1, 100, 10);
    clear();
    sell(1, 200, 10);
    ASSERT_EQ(events().size(), 1u);
    EXPECT_EQ(events()[0].reason, Reason::DuplicateId);
    EXPECT_FALSE(book().best_ask());
}

TEST_F(ValidationTest, UnknownSymbolIsRejected) {
    buy(1, 100, 10, /*symbol=*/99);
    EXPECT_EQ(events()[0].reason, Reason::UnknownSymbol);
}

TEST_F(ValidationTest, CancelOnUnknownSymbolIsRejected) {
    cancel(1, 99);
    EXPECT_EQ(events()[0].reason, Reason::UnknownSymbol);
}

TEST_F(ValidationTest, CancelUnknownIdIsRejected) {
    cancel(42);
    EXPECT_EQ(events()[0], make_event(EventType::Rejected, Reason::UnknownOrder, 42, 0));
}

TEST_F(ValidationTest, RejectionDoesNotChangeBook) {
    buy(1, 100, 10);
    sell(2, 100, 0);
    sell(3, 0, 10);
    EXPECT_EQ(book().volume_at(Side::Buy, 100), 10u);
    EXPECT_EQ(count(EventType::Trade), 0u);
}

TEST_F(ValidationTest, CommandsProcessedCountsEverything) {
    buy(1, 100, 10);
    buy(2, 100, 0);  // rejected still counts
    cancel(1);
    EXPECT_EQ(engine_.commands_processed(), 3u);
}

TEST_F(ValidationTest, LiveOrdersAcrossSymbols) {
    buy(1, 100, 1, 0);
    buy(2, 100, 1, 1);
    EXPECT_EQ(engine_.live_orders(), 2u);
    EXPECT_EQ(engine_.num_symbols(), 2u);
}

// A tiny pool to exercise exhaustion.
class SmallPoolTest : public EngineTest {
protected:
    SmallPoolTest() : EngineTest(1, 2) {}
};

TEST_F(SmallPoolTest, RejectsLimitWhenPoolExhausted) {
    buy(1, 100, 1);
    buy(2, 100, 1);
    clear();
    buy(3, 100, 1);
    ASSERT_EQ(events().size(), 1u);
    EXPECT_EQ(events()[0].reason, Reason::PoolExhausted);
}

TEST_F(SmallPoolTest, IocStillWorksWhenPoolExhausted) {
    buy(1, 100, 1);
    buy(2, 100, 1);
    clear();
    send(OrderType::IOC, Side::Sell, 3, 100, 1);
    EXPECT_EQ(trades().size(), 1u);
}

TEST_F(SmallPoolTest, PoolSlotsAreRecycledAfterCancel) {
    buy(1, 100, 1);
    buy(2, 100, 1);
    cancel(1);
    clear();
    buy(3, 100, 1);
    EXPECT_EQ(events()[0].type, EventType::Accepted);
    EXPECT_TRUE(engine_.is_live(3));
}

TEST_F(SmallPoolTest, PoolSlotsAreRecycledAfterFill) {
    buy(1, 100, 1);
    buy(2, 100, 1);
    send(OrderType::IOC, Side::Sell, 3, 100, 2);  // full match releases both slots
    clear();
    buy(4, 99, 1);
    buy(5, 99, 1);
    EXPECT_EQ(count(EventType::Rejected), 0u);
}
