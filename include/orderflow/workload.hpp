#pragma once
// Synthetic, reproducible order-flow generator.
//
// Produces a realistic-ish stream: a random-walk mid price per symbol, most
// limit orders resting a few ticks away from mid, some crossing the spread,
// plus cancels, IOC, market and FOK orders. A fixed seed always produces the
// same stream, so benchmark numbers are comparable between runs and machines.

#include <cstdint>
#include <random>
#include <vector>

#include "orderflow/types.hpp"

namespace orderflow {

struct WorkloadConfig {
    std::uint64_t seed{42};
    std::size_t num_symbols{4};
    Price start_mid{10'000};      // ticks
    std::size_t target_live{200'000};  // size of the cancel-candidate list
    // Mix of new orders (percent, must sum to <= 100; remainder = cancels).
    int pct_limit{58};
    int pct_ioc{6};
    int pct_market{3};
    int pct_fok{3};
};

class WorkloadGenerator {
public:
    explicit WorkloadGenerator(const WorkloadConfig& cfg = {})
        : cfg_(cfg), rng_(cfg.seed), mids_(cfg.num_symbols, cfg.start_mid) {
        live_.reserve(cfg.target_live * 2);
    }

    Command next() {
        const auto sym = static_cast<SymbolId>(pick(cfg_.num_symbols));

        // Drift the mid price occasionally.
        if (pick(100) < 5) mids_[sym] += (pick(2) == 0 ? -1 : 1);
        if (mids_[sym] < 100) mids_[sym] = 100;

        const bool force_cancel = live_.size() > cfg_.target_live;
        const int roll = static_cast<int>(pick(100));
        const int c1 = cfg_.pct_limit;
        const int c2 = c1 + cfg_.pct_ioc;
        const int c3 = c2 + cfg_.pct_market;
        const int c4 = c3 + cfg_.pct_fok;

        if (!live_.empty() && (force_cancel || roll >= c4)) return make_cancel();

        const Side side = pick(2) == 0 ? Side::Buy : Side::Sell;
        const Qty qty = static_cast<Qty>(1 + pick(100));
        const OrderId id = next_id_++;

        if (roll < c1) {
            // Offset from mid: mostly passive (rest), sometimes aggressive (cross).
            const auto off = static_cast<Price>(pick(25)) - 3;  // [-3, 21]
            const Price px = side == Side::Buy ? mids_[sym] - off : mids_[sym] + off;
            live_.push_back({sym, id});
            return Command::limit(sym, id, side, px, qty);
        }
        if (roll < c2) {
            const auto off = static_cast<Price>(pick(5));
            const Price px = side == Side::Buy ? mids_[sym] + off : mids_[sym] - off;
            return Command::new_order(sym, id, side, OrderType::IOC, px, qty);
        }
        if (roll < c3) return Command::new_order(sym, id, side, OrderType::Market, 0, qty);
        const auto off = static_cast<Price>(pick(5));
        const Price px = side == Side::Buy ? mids_[sym] + off : mids_[sym] - off;
        return Command::new_order(sym, id, side, OrderType::FOK, px, qty);
    }

    std::vector<Command> generate(std::size_t n) {
        std::vector<Command> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) out.push_back(next());
        return out;
    }

private:
    struct LiveRef {
        SymbolId sym;
        OrderId id;
    };

    std::uint64_t pick(std::uint64_t n) { return std::uniform_int_distribution<std::uint64_t>(0, n - 1)(rng_); }

    Command make_cancel() {
        // Cancel a random previously-submitted limit order. It may already have
        // been filled - that produces an UnknownOrder reject, just like real life.
        const std::size_t i = pick(live_.size());
        const LiveRef ref = live_[i];
        live_[i] = live_.back();
        live_.pop_back();
        return Command::cancel(ref.sym, ref.id);
    }

    WorkloadConfig cfg_;
    std::mt19937_64 rng_;
    std::vector<Price> mids_;
    std::vector<LiveRef> live_;
    OrderId next_id_{1};
};

}  // namespace orderflow
