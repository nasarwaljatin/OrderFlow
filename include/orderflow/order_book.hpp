#pragma once
// Single-symbol limit order book with price-time priority.
//
// Data structure (see README "Design" section for the diagram):
//
//   bids_: std::map<Price, PriceLevel, std::greater<>>   best bid = begin()
//   asks_: std::map<Price, PriceLevel, std::less<>>      best ask = begin()
//
//   Each PriceLevel holds an *intrusive* doubly linked FIFO of Orders:
//     head (oldest, matched first) <-> ... <-> tail (newest)
//
//   The engine-wide hash index OrderId -> Order* gives O(1) lookup for cancels;
//   intrusive prev/next pointers give O(1) unlink. Removing the last order of a
//   level erases the level (O(log L), L = number of price levels, typically small).
//
// Orders are allocated from an ObjectPool, never from the heap.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "orderflow/event_sink.hpp"
#include "orderflow/object_pool.hpp"
#include "orderflow/types.hpp"

namespace orderflow {

struct PriceLevel;

struct Order {
    OrderId id;
    Price price;
    Qty remaining;
    Side side;
    SymbolId symbol;
    Order* prev;
    Order* next;
    PriceLevel* level;
};

struct PriceLevel {
    Price price{0};
    std::uint64_t total_qty{0};
    std::uint32_t order_count{0};
    Order* head{nullptr};
    Order* tail{nullptr};
};

struct LevelView {
    Price price;
    std::uint64_t qty;
    std::uint32_t orders;
    friend bool operator==(const LevelView&, const LevelView&) = default;
};

using OrderIndex = std::unordered_map<OrderId, Order*>;

class OrderBook {
public:
    OrderBook(SymbolId symbol, ObjectPool<Order>& pool, OrderIndex& index, EventSink& sink);

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Precondition: cmd has already passed engine-level validation
    // (qty > 0, valid price for priced types, id not live).
    void add(const Command& cmd);

    // Precondition: `o` is live and belongs to this book.
    void cancel(Order* o);

    // ---- Queries (used by tests, market data and the gateway) ----
    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] std::uint64_t volume_at(Side side, Price price) const;
    [[nodiscard]] std::size_t level_count(Side side) const;
    [[nodiscard]] std::vector<LevelView> depth(Side side, std::size_t max_levels) const;
    [[nodiscard]] std::size_t live_orders() const noexcept { return live_orders_; }
    [[nodiscard]] SymbolId symbol() const noexcept { return symbol_; }

private:
    using BidMap = std::map<Price, PriceLevel, std::greater<>>;
    using AskMap = std::map<Price, PriceLevel, std::less<>>;

    // Matches `remaining` of an incoming order against the opposite side.
    // Returns the quantity left unfilled.
    template <typename OppositeMap>
    Qty match(OppositeMap& opposite, const Command& taker, Qty remaining);

    // Total quantity available on the opposite side at prices acceptable to the taker
    // (stops counting as soon as `needed` is reached). Used for FOK.
    template <typename OppositeMap>
    std::uint64_t available(const OppositeMap& opposite, const Command& taker, Qty needed) const;

    template <typename SameMap>
    void rest(SameMap& same, const Command& cmd, Qty remaining);

    template <typename Map>
    void unlink(Map& map, Order* o);

    void emit(EventType t, Reason r, OrderId id, Qty qty, OrderId maker = 0, Price px = 0);

    SymbolId symbol_;
    ObjectPool<Order>& pool_;
    OrderIndex& index_;
    EventSink& sink_;
    BidMap bids_;
    AskMap asks_;
    std::size_t live_orders_{0};
};

}  // namespace orderflow
