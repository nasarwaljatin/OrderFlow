// Core limit-order-book behaviour: resting, price-time priority, matching, cancels.
#include "test_helpers.hpp"

using namespace orderflow;
using namespace orderflow::test;

using BookTest = EngineTest;

// ----------------------------- Resting orders ------------------------------

TEST_F(BookTest, EmptyBookHasNoBestPrices) {
    EXPECT_FALSE(book().best_bid().has_value());
    EXPECT_FALSE(book().best_ask().has_value());
    EXPECT_EQ(book().live_orders(), 0u);
}

TEST_F(BookTest, RestingBidSetsBestBid) {
    buy(1, 100, 10);
    ASSERT_TRUE(book().best_bid());
    EXPECT_EQ(*book().best_bid(), 100);
    EXPECT_FALSE(book().best_ask());
}

TEST_F(BookTest, RestingAskSetsBestAsk) {
    sell(1, 105, 10);
    ASSERT_TRUE(book().best_ask());
    EXPECT_EQ(*book().best_ask(), 105);
    EXPECT_FALSE(book().best_bid());
}

TEST_F(BookTest, BestBidIsHighestPrice) {
    buy(1, 100, 1);
    buy(2, 102, 1);
    buy(3, 101, 1);
    EXPECT_EQ(*book().best_bid(), 102);
}

TEST_F(BookTest, BestAskIsLowestPrice) {
    sell(1, 110, 1);
    sell(2, 108, 1);
    sell(3, 109, 1);
    EXPECT_EQ(*book().best_ask(), 108);
}

TEST_F(BookTest, RestingOrderEmitsOnlyAccepted) {
    buy(1, 100, 10);
    ASSERT_EQ(events().size(), 1u);
    EXPECT_EQ(events()[0], make_event(EventType::Accepted, Reason::None, 1, 10));
}

TEST_F(BookTest, VolumeAggregatesAtSamePrice) {
    buy(1, 100, 10);
    buy(2, 100, 15);
    buy(3, 99, 5);
    EXPECT_EQ(book().volume_at(Side::Buy, 100), 25u);
    EXPECT_EQ(book().volume_at(Side::Buy, 99), 5u);
    EXPECT_EQ(book().volume_at(Side::Buy, 98), 0u);
}

TEST_F(BookTest, LevelCountTracksDistinctPrices) {
    sell(1, 101, 1);
    sell(2, 101, 1);
    sell(3, 102, 1);
    EXPECT_EQ(book().level_count(Side::Sell), 2u);
    EXPECT_EQ(book().level_count(Side::Buy), 0u);
}

TEST_F(BookTest, DepthIsOrderedBestFirst) {
    buy(1, 98, 1);
    buy(2, 100, 2);
    buy(3, 99, 3);
    buy(4, 100, 4);
    const auto d = book().depth(Side::Buy, 10);
    ASSERT_EQ(d.size(), 3u);
    EXPECT_EQ(d[0], (LevelView{100, 6, 2}));
    EXPECT_EQ(d[1], (LevelView{99, 3, 1}));
    EXPECT_EQ(d[2], (LevelView{98, 1, 1}));
}

TEST_F(BookTest, DepthRespectsMaxLevels) {
    for (OrderId i = 1; i <= 5; ++i) sell(i, 100 + static_cast<Price>(i), 1);
    EXPECT_EQ(book().depth(Side::Sell, 2).size(), 2u);
    EXPECT_EQ(book().depth(Side::Sell, 2)[0].price, 101);
}

TEST_F(BookTest, NonCrossingOrdersBothRest) {
    buy(1, 99, 10);
    sell(2, 100, 10);
    EXPECT_EQ(count(EventType::Trade), 0u);
    EXPECT_EQ(book().live_orders(), 2u);
}

TEST_F(BookTest, LiveOrderCountTracksAddsFillsAndCancels) {
    buy(1, 100, 10);
    buy(2, 100, 10);
    EXPECT_EQ(book().live_orders(), 2u);
    sell(3, 100, 10);  // fills order 1
    EXPECT_EQ(book().live_orders(), 1u);
    cancel(2);
    EXPECT_EQ(book().live_orders(), 0u);
}

// ------------------------------- Matching ----------------------------------

TEST_F(BookTest, ExactCrossFillsBothSides) {
    buy(1, 100, 10);
    clear();
    sell(2, 100, 10);
    ASSERT_EQ(trades().size(), 1u);
    EXPECT_EQ(trades()[0], make_trade(2, 1, 100, 10));
    EXPECT_FALSE(book().best_bid());
    EXPECT_FALSE(book().best_ask());
    EXPECT_EQ(book().live_orders(), 0u);
}

TEST_F(BookTest, EventOrderIsAcceptedThenTrades) {
    sell(1, 100, 5);
    clear();
    buy(2, 100, 5);
    ASSERT_EQ(events().size(), 2u);
    EXPECT_EQ(events()[0].type, EventType::Accepted);
    EXPECT_EQ(events()[1].type, EventType::Trade);
}

TEST_F(BookTest, TradeExecutesAtMakerPrice) {
    sell(1, 100, 10);
    buy(2, 105, 10);  // willing to pay 105, gets price improvement to 100
    ASSERT_EQ(trades().size(), 1u);
    EXPECT_EQ(trades()[0].price, 100);
}

TEST_F(BookTest, SellTakerTradesAtBidPrice) {
    buy(1, 103, 10);
    sell(2, 100, 10);
    ASSERT_EQ(trades().size(), 1u);
    EXPECT_EQ(trades()[0].price, 103);
}

TEST_F(BookTest, PartialFillTakerRemainderRests) {
    sell(1, 100, 4);
    buy(2, 100, 10);
    EXPECT_EQ(trades()[0].qty, 4u);
    EXPECT_EQ(*book().best_bid(), 100);
    EXPECT_EQ(book().volume_at(Side::Buy, 100), 6u);
    EXPECT_FALSE(book().best_ask());
}

TEST_F(BookTest, PartialFillMakerRemainderStays) {
    sell(1, 100, 10);
    buy(2, 100, 3);
    EXPECT_EQ(book().volume_at(Side::Sell, 100), 7u);
    EXPECT_TRUE(engine_.is_live(1));
    EXPECT_FALSE(engine_.is_live(2));
}

TEST_F(BookTest, TimePriorityWithinPriceLevel) {
    sell(1, 100, 5);
    sell(2, 100, 5);
    sell(3, 100, 5);
    clear();
    buy(4, 100, 7);
    const auto t = trades();
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[0], make_trade(4, 1, 100, 5));  // oldest first
    EXPECT_EQ(t[1], make_trade(4, 2, 100, 2));
    EXPECT_EQ(book().volume_at(Side::Sell, 100), 8u);
}

TEST_F(BookTest, PricePriorityBeatsTimePriority) {
    sell(1, 101, 5);  // older but worse price
    sell(2, 100, 5);  // newer but better price
    buy(3, 101, 5);
    ASSERT_EQ(trades().size(), 1u);
    EXPECT_EQ(trades()[0].maker_id, 2u);
}

TEST_F(BookTest, SweepsMultipleLevels) {
    sell(1, 100, 5);
    sell(2, 101, 5);
    sell(3, 102, 5);
    clear();
    buy(4, 102, 12);
    const auto t = trades();
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0], make_trade(4, 1, 100, 5));
    EXPECT_EQ(t[1], make_trade(4, 2, 101, 5));
    EXPECT_EQ(t[2], make_trade(4, 3, 102, 2));
    EXPECT_EQ(book().level_count(Side::Sell), 1u);
    EXPECT_EQ(book().volume_at(Side::Sell, 102), 3u);
}

TEST_F(BookTest, SweepStopsAtLimitPriceAndRests) {
    sell(1, 100, 5);
    sell(2, 102, 5);
    buy(3, 101, 10);
    ASSERT_EQ(trades().size(), 1u);
    EXPECT_EQ(*book().best_bid(), 101);
    EXPECT_EQ(book().volume_at(Side::Buy, 101), 5u);
    EXPECT_EQ(*book().best_ask(), 102);
}

TEST_F(BookTest, SellSweepsBidsHighestFirst) {
    buy(1, 98, 5);
    buy(2, 100, 5);
    buy(3, 99, 5);
    clear();
    sell(4, 98, 15);
    const auto t = trades();
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0].price, 100);
    EXPECT_EQ(t[1].price, 99);
    EXPECT_EQ(t[2].price, 98);
    EXPECT_FALSE(book().best_bid());
}

TEST_F(BookTest, FullyFilledMakerIdCanBeReused) {
    sell(1, 100, 5);
    buy(2, 100, 5);
    clear();
    sell(1, 101, 5);  // id 1 is no longer live
    EXPECT_EQ(count(EventType::Rejected), 0u);
    EXPECT_TRUE(engine_.is_live(1));
}

TEST_F(BookTest, LargeQuantitiesAggregateWithoutOverflow) {
    const Qty big = 4'000'000'000u;
    buy(1, 100, big);
    buy(2, 100, big);
    EXPECT_EQ(book().volume_at(Side::Buy, 100), 8'000'000'000ull);
}

// -------------------------------- Cancels ----------------------------------

TEST_F(BookTest, CancelRemovesOrderAndEmitsEvent) {
    buy(1, 100, 10);
    clear();
    cancel(1);
    ASSERT_EQ(events().size(), 1u);
    EXPECT_EQ(events()[0], make_event(EventType::Cancelled, Reason::UserCancel, 1, 10));
    EXPECT_FALSE(book().best_bid());
    EXPECT_FALSE(engine_.is_live(1));
}

TEST_F(BookTest, CancelReportsRemainingQtyAfterPartialFill) {
    sell(1, 100, 10);
    buy(2, 100, 4);
    clear();
    cancel(1);
    EXPECT_EQ(events()[0].qty, 6u);
}

TEST_F(BookTest, CancelLastOrderRemovesLevel) {
    buy(1, 100, 10);
    buy(2, 99, 10);
    cancel(1);
    EXPECT_EQ(book().level_count(Side::Buy), 1u);
    EXPECT_EQ(*book().best_bid(), 99);
}

TEST_F(BookTest, CancelHeadKeepsFifoOfRest) {
    sell(1, 100, 1);
    sell(2, 100, 1);
    sell(3, 100, 1);
    cancel(1);
    clear();
    buy(4, 100, 2);
    const auto t = trades();
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[0].maker_id, 2u);
    EXPECT_EQ(t[1].maker_id, 3u);
}

TEST_F(BookTest, CancelMiddleKeepsFifoOfRest) {
    sell(1, 100, 1);
    sell(2, 100, 1);
    sell(3, 100, 1);
    cancel(2);
    clear();
    buy(4, 100, 2);
    const auto t = trades();
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[0].maker_id, 1u);
    EXPECT_EQ(t[1].maker_id, 3u);
}

TEST_F(BookTest, CancelTailKeepsFifoOfRest) {
    sell(1, 100, 1);
    sell(2, 100, 1);
    sell(3, 100, 1);
    cancel(3);
    sell(5, 100, 1);  // new tail after cancelling the old tail
    clear();
    buy(4, 100, 3);
    const auto t = trades();
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0].maker_id, 1u);
    EXPECT_EQ(t[1].maker_id, 2u);
    EXPECT_EQ(t[2].maker_id, 5u);
}

TEST_F(BookTest, CancelUpdatesLevelVolume) {
    buy(1, 100, 10);
    buy(2, 100, 7);
    cancel(1);
    EXPECT_EQ(book().volume_at(Side::Buy, 100), 7u);
    EXPECT_EQ(book().depth(Side::Buy, 1)[0].orders, 1u);
}

TEST_F(BookTest, DoubleCancelIsRejected) {
    buy(1, 100, 10);
    cancel(1);
    clear();
    cancel(1);
    ASSERT_EQ(events().size(), 1u);
    EXPECT_EQ(events()[0].type, EventType::Rejected);
    EXPECT_EQ(events()[0].reason, Reason::UnknownOrder);
}

TEST_F(BookTest, CancelFilledOrderIsRejected) {
    sell(1, 100, 5);
    buy(2, 100, 5);
    clear();
    cancel(1);
    EXPECT_EQ(events()[0].reason, Reason::UnknownOrder);
}

TEST_F(BookTest, CancelledOrderNoLongerMatches) {
    sell(1, 100, 5);
    cancel(1);
    clear();
    buy(2, 100, 5);
    EXPECT_EQ(count(EventType::Trade), 0u);
    EXPECT_EQ(*book().best_bid(), 100);
}

// ----------------------------- Multi-symbol --------------------------------

TEST_F(BookTest, SymbolsAreIsolated) {
    sell(1, 100, 5, /*symbol=*/0);
    buy(2, 100, 5, /*symbol=*/1);
    EXPECT_EQ(count(EventType::Trade), 0u);
    EXPECT_EQ(*book(0).best_ask(), 100);
    EXPECT_EQ(*book(1).best_bid(), 100);
}

TEST_F(BookTest, EventsCarrySymbol) {
    sell(1, 100, 5, 1);
    buy(2, 100, 5, 1);
    for (const auto& e : events()) EXPECT_EQ(e.symbol, 1u);
}

TEST_F(BookTest, CancelWithWrongSymbolIsRejected) {
    buy(1, 100, 5, 0);
    clear();
    cancel(1, /*symbol=*/1);
    EXPECT_EQ(events()[0].reason, Reason::UnknownOrder);
    EXPECT_TRUE(engine_.is_live(1));
}

TEST_F(BookTest, OrderIdsAreUniqueAcrossSymbols) {
    buy(1, 100, 5, 0);
    clear();
    buy(1, 100, 5, 1);
    EXPECT_EQ(events()[0].reason, Reason::DuplicateId);
}
