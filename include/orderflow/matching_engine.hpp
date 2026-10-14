#pragma once
// Multi-symbol matching engine.
//
// Owns one OrderBook per symbol, a single shared ObjectPool<Order> and a
// single OrderId -> Order* index (order ids are unique across all symbols).
//
// The engine is single-threaded and deterministic: the same sequence of
// Commands always produces the same sequence of Events. That property is what
// makes the journal/replay and the differential fuzz tests possible.

#include <cstddef>
#include <memory>
#include <vector>

#include "orderflow/event_sink.hpp"
#include "orderflow/object_pool.hpp"
#include "orderflow/order_book.hpp"
#include "orderflow/types.hpp"

namespace orderflow {

struct EngineConfig {
    std::size_t num_symbols{1};
    std::size_t max_orders{1u << 20};  // max simultaneously *resting* orders
};

class MatchingEngine {
public:
    MatchingEngine(const EngineConfig& cfg, EventSink& sink);

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // The single entry point. Validates, routes and executes one command.
    void process(const Command& cmd);

    [[nodiscard]] const OrderBook& book(SymbolId symbol) const { return *books_.at(symbol); }
    [[nodiscard]] bool is_live(OrderId id) const { return index_.find(id) != index_.end(); }
    [[nodiscard]] std::size_t live_orders() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t num_symbols() const noexcept { return books_.size(); }
    [[nodiscard]] std::uint64_t commands_processed() const noexcept { return processed_; }

private:
    void reject(const Command& cmd, Reason r);

    EventSink& sink_;
    ObjectPool<Order> pool_;
    OrderIndex index_;
    std::vector<std::unique_ptr<OrderBook>> books_;
    std::uint64_t processed_{0};
};

}  // namespace orderflow
