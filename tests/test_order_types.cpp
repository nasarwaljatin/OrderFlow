// IOC, FOK and Market order semantics.
#include "test_helpers.hpp"

using namespace orderflow;
using namespace orderflow::test;

using OrderTypeTest = EngineTest;

// ---------------------------------- IOC ------------------------------------

TEST_F(OrderTypeTest, IocFullFillLeavesNothing) {
    sell(1, 100, 10);
    clear();
    send(OrderType::IOC, Side::Buy, 2, 100, 10);
    EXPECT_EQ(trades().size(), 1u);
    EXPECT_EQ(count(EventType::Cancelled), 0u);
    EXPECT_FALSE(engine_.is_live(2));
}

TEST_F(OrderTypeTest, IocPartialFillCancelsRemainder) {
    sell(1, 100, 4);
    clear();
    send(OrderType::IOC, Side::Buy, 2, 100, 10);
    ASSERT_EQ(events().size(), 3u);
    EXPECT_EQ(events()[1], make_trade(2, 1, 100, 4));
    EXPECT_EQ(events()[2], make_event(EventType::Cancelled, Reason::IocRemainder, 2, 6));
    EXPECT_FALSE(book().best_bid());
}

TEST_F(OrderTypeTest, IocWithNoLiquidityIsCancelled) {
    send(OrderType::IOC, Side::Sell, 1, 100, 10);
    ASSERT_EQ(events().size(), 2u);
    EXPECT_EQ(events()[1].reason, Reason::IocRemainder);
    EXPECT_EQ(events()[1].qty, 10u);
}

TEST_F(OrderTypeTest, IocNeverRests) {
    buy(1, 99, 10);
    send(OrderType::IOC, Side::Sell, 2, 100, 10);  // doesn't cross
    EXPECT_FALSE(book().best_ask());
    EXPECT_FALSE(engine_.is_live(2));
}

TEST_F(OrderTypeTest, IocRespectsLimitPrice) {
    sell(1, 100, 5);
    sell(2, 102, 5);
    clear();
    send(OrderType::IOC, Side::Buy, 3, 101, 10);
    ASSERT_EQ(trades().size(), 1u);
    EXPECT_EQ(events().back().qty, 5u);
    EXPECT_EQ(book().volume_at(Side::Sell, 102), 5u);
}

TEST_F(OrderTypeTest, IocSweepsMultipleLevels) {
    buy(1, 101, 3);
    buy(2, 100, 3);
    clear();
    send(OrderType::IOC, Side::Sell, 3, 100, 6);
    EXPECT_EQ(trades().size(), 2u);
    EXPECT_EQ(count(EventType::Cancelled), 0u);
}

// --------------------------------- Market ----------------------------------

TEST_F(OrderTypeTest, MarketBuySweepsAnyPrice) {
    sell(1, 100, 5);
    sell(2, 500, 5);
    clear();
    send(OrderType::Market, Side::Buy, 3, 0, 10);
    const auto t = trades();
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[1].price, 500);
    EXPECT_FALSE(book().best_ask());
}

TEST_F(OrderTypeTest, MarketSellSweepsAnyPrice) {
    buy(1, 100, 5);
    buy(2, 1, 5);
    clear();
    send(OrderType::Market, Side::Sell, 3, 0, 10);
    EXPECT_EQ(trades().size(), 2u);
    EXPECT_FALSE(book().best_bid());
}

TEST_F(OrderTypeTest, MarketWithNoLiquidityIsCancelled) {
    send(OrderType::Market, Side::Buy, 1, 0, 10);
    ASSERT_EQ(events().size(), 2u);
    EXPECT_EQ(events()[1], make_event(EventType::Cancelled, Reason::MarketNoLiquidity, 1, 10));
}

TEST_F(OrderTypeTest, MarketPartialFillCancelsRemainder) {
    sell(1, 100, 3);
    clear();
    send(OrderType::Market, Side::Buy, 2, 0, 10);
    EXPECT_EQ(events().back(), make_event(EventType::Cancelled, Reason::MarketNoLiquidity, 2, 7));
}

TEST_F(OrderTypeTest, MarketNeverRests) {
    send(OrderType::Market, Side::Sell, 1, 0, 10);
    EXPECT_FALSE(book().best_ask());
    EXPECT_EQ(book().live_orders(), 0u);
}

TEST_F(OrderTypeTest, MarketIgnoresPriceField) {
    sell(1, 100, 5);
    clear();
    send(OrderType::Market, Side::Buy, 2, 1, 5);  // price 1 would never cross as a limit
    EXPECT_EQ(trades().size(), 1u);
}

// ---------------------------------- FOK ------------------------------------

TEST_F(OrderTypeTest, FokFillsCompletelyWhenLiquidityExists) {
    sell(1, 100, 10);
    clear();
    send(OrderType::FOK, Side::Buy, 2, 100, 10);
    EXPECT_EQ(trades().size(), 1u);
    EXPECT_EQ(count(EventType::Cancelled), 0u);
}

TEST_F(OrderTypeTest, FokInsufficientLiquidityTradesNothing) {
    sell(1, 100, 9);
    clear();
    send(OrderType::FOK, Side::Buy, 2, 100, 10);
    ASSERT_EQ(events().size(), 2u);
    EXPECT_EQ(events()[1], make_event(EventType::Cancelled, Reason::FokUnfilled, 2, 10));
    EXPECT_EQ(book().volume_at(Side::Sell, 100), 9u);  // book untouched
}

TEST_F(OrderTypeTest, FokAcrossMultipleLevels) {
    sell(1, 100, 4);
    sell(2, 101, 6);
    clear();
    send(OrderType::FOK, Side::Buy, 3, 101, 10);
    EXPECT_EQ(trades().size(), 2u);
    EXPECT_FALSE(book().best_ask());
}

TEST_F(OrderTypeTest, FokIgnoresLiquidityBeyondLimit) {
    sell(1, 100, 4);
    sell(2, 105, 100);
    clear();
    send(OrderType::FOK, Side::Buy, 3, 101, 10);
    EXPECT_EQ(trades().size(), 0u);
    EXPECT_EQ(events().back().reason, Reason::FokUnfilled);
}

TEST_F(OrderTypeTest, FokSellSide) {
    buy(1, 100, 5);
    buy(2, 99, 5);
    clear();
    send(OrderType::FOK, Side::Sell, 3, 99, 10);
    EXPECT_EQ(trades().size(), 2u);
    EXPECT_FALSE(book().best_bid());
}

TEST_F(OrderTypeTest, FokNeverRests) {
    send(OrderType::FOK, Side::Buy, 1, 100, 10);
    EXPECT_EQ(book().live_orders(), 0u);
    EXPECT_FALSE(engine_.is_live(1));
}

TEST_F(OrderTypeTest, FokExactLiquidityAcrossManyMakers) {
    for (OrderId i = 1; i <= 10; ++i) sell(i, 100, 1);
    clear();
    send(OrderType::FOK, Side::Buy, 11, 100, 10);
    EXPECT_EQ(trades().size(), 10u);
    EXPECT_EQ(book().live_orders(), 0u);
}

TEST_F(OrderTypeTest, FokCanFillPartOfAMaker) {
    sell(1, 100, 50);
    clear();
    send(OrderType::FOK, Side::Buy, 2, 100, 20);
    EXPECT_EQ(trades().size(), 1u);
    EXPECT_EQ(book().volume_at(Side::Sell, 100), 30u);
}
