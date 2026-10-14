// Differential fuzzing: the optimised engine vs. the naive ReferenceEngine.
//
// Each seed generates a random but adversarial command stream: tight price
// band (lots of crossing), all four order types, invalid quantities/prices,
// unknown symbols, duplicate ids and cancels of random (often dead) ids.
// After *every* command, both engines must have emitted identical events.
#include <gtest/gtest.h>

#include <random>

#include "orderflow/event_sink.hpp"
#include "orderflow/matching_engine.hpp"
#include "reference_book.hpp"
#include "test_helpers.hpp"

using namespace orderflow;
using namespace orderflow::test;

namespace {

constexpr std::size_t kSymbols = 3;

Command random_command(std::mt19937_64& rng, OrderId& next_id) {
    auto pick = [&](std::uint64_t n) { return std::uniform_int_distribution<std::uint64_t>(0, n - 1)(rng); };

    // Symbol: occasionally out of range.
    const auto sym = static_cast<SymbolId>(pick(100) < 2 ? kSymbols + pick(3) : pick(kSymbols));

    if (pick(100) < 25) {
        // Cancel a recent id (often still live), sometimes a never-used one.
        const OrderId target = next_id > 1 ? 1 + pick(next_id + 5) : 1;
        return Command::cancel(sym, target);
    }

    // Mostly fresh ids, sometimes a reused one (-> DuplicateId if still live).
    const OrderId id = (pick(100) < 5 && next_id > 1) ? 1 + pick(next_id - 1) : next_id++;
    const Side side = pick(2) == 0 ? Side::Buy : Side::Sell;
    Qty qty = static_cast<Qty>(1 + pick(20));
    if (pick(100) < 2) qty = 0;
    Price px = 95 + static_cast<Price>(pick(11));  // [95, 105] -> constant crossing
    if (pick(100) < 2) px = pick(2) == 0 ? 0 : -1;

    const auto roll = pick(100);
    const OrderType t = roll < 70   ? OrderType::Limit
                        : roll < 82 ? OrderType::IOC
                        : roll < 91 ? OrderType::FOK
                                    : OrderType::Market;
    return Command::new_order(sym, id, side, t, px, qty);
}

void run_differential(std::uint64_t seed, std::size_t steps) {
    std::mt19937_64 rng(seed);
    VectorSink sink;
    MatchingEngine engine(EngineConfig{kSymbols, 1 << 16}, sink);
    ReferenceEngine ref(kSymbols);
    OrderId next_id = 1;

    for (std::size_t step = 0; step < steps; ++step) {
        const Command c = random_command(rng, next_id);
        sink.clear();
        ref.events.clear();
        engine.process(c);
        ref.process(c);
        ASSERT_EQ(sink.events, ref.events) << "divergence at step " << step << " (seed " << seed << ")";
    }

    // Final state must match too.
    ASSERT_EQ(engine.live_orders(), ref.live());
    for (SymbolId s = 0; s < kSymbols; ++s) {
        EXPECT_EQ(engine.book(s).best_bid(), ref.best(s, Side::Buy));
        EXPECT_EQ(engine.book(s).best_ask(), ref.best(s, Side::Sell));
        for (Price px = 95; px <= 105; ++px) {
            EXPECT_EQ(engine.book(s).volume_at(Side::Buy, px), ref.volume_at(s, Side::Buy, px));
            EXPECT_EQ(engine.book(s).volume_at(Side::Sell, px), ref.volume_at(s, Side::Sell, px));
        }
    }
}

}  // namespace

class DifferentialFuzz : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(DifferentialFuzz, EngineMatchesReferenceModel) { run_differential(GetParam(), 5'000); }

INSTANTIATE_TEST_SUITE_P(Seeds, DifferentialFuzz, ::testing::Range<std::uint64_t>(0, 50));

TEST(DifferentialFuzzLong, OneHundredThousandSteps) { run_differential(0xC0FFEE, 100'000); }
