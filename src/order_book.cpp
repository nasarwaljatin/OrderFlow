#include "orderflow/order_book.hpp"

#include <algorithm>
#include <cassert>

namespace orderflow {

namespace {

// Does a resting level at `level_price` satisfy the taker's price limit?
inline bool crosses(const Command& taker, Price level_price) {
    if (taker.order_type == OrderType::Market) return true;
    return taker.side == Side::Buy ? level_price <= taker.price : level_price >= taker.price;
}

}  // namespace

OrderBook::OrderBook(SymbolId symbol, ObjectPool<Order>& pool, OrderIndex& index, EventSink& sink)
    : symbol_(symbol), pool_(pool), index_(index), sink_(sink) {}

void OrderBook::emit(EventType t, Reason r, OrderId id, Qty qty, OrderId maker, Price px) {
    Event e;
    e.type = t;
    e.reason = r;
    e.symbol = symbol_;
    e.qty = qty;
    e.id = id;
    e.maker_id = maker;
    e.price = px;
    sink_.on_event(e);
}

// ---------------------------------------------------------------------------
// add(): the whole lifecycle of an incoming order
//   1. Accepted
//   2. FOK pre-check (all-or-nothing)
//   3. match against the opposite side, best price first, FIFO within a price
//   4. remainder: Limit -> rest on book; IOC/FOK/Market -> Cancelled
// ---------------------------------------------------------------------------
void OrderBook::add(const Command& cmd) {
    emit(EventType::Accepted, Reason::None, cmd.id, cmd.qty);

    const bool is_buy = cmd.side == Side::Buy;

    if (cmd.order_type == OrderType::FOK) {
        const std::uint64_t avail =
            is_buy ? available(asks_, cmd, cmd.qty) : available(bids_, cmd, cmd.qty);
        if (avail < cmd.qty) {
            emit(EventType::Cancelled, Reason::FokUnfilled, cmd.id, cmd.qty);
            return;
        }
    }

    Qty remaining = is_buy ? match(asks_, cmd, cmd.qty) : match(bids_, cmd, cmd.qty);
    if (remaining == 0) return;

    switch (cmd.order_type) {
        case OrderType::Limit:
            if (is_buy) rest(bids_, cmd, remaining);
            else rest(asks_, cmd, remaining);
            break;
        case OrderType::IOC:
            emit(EventType::Cancelled, Reason::IocRemainder, cmd.id, remaining);
            break;
        case OrderType::Market:
            emit(EventType::Cancelled, Reason::MarketNoLiquidity, cmd.id, remaining);
            break;
        case OrderType::FOK:
            // Unreachable: the pre-check guarantees a full fill.
            assert(false && "FOK order partially filled");
            break;
    }
}

template <typename OppositeMap>
Qty OrderBook::match(OppositeMap& opposite, const Command& taker, Qty remaining) {
    while (remaining > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();  // best price on the opposite side
        PriceLevel& level = level_it->second;
        if (!crosses(taker, level.price)) break;

        Order* maker = level.head;
        while (maker != nullptr && remaining > 0) {
            const Qty fill = std::min(remaining, maker->remaining);
            // Trades always execute at the *resting* order's price.
            emit(EventType::Trade, Reason::None, taker.id, fill, maker->id, level.price);
            maker->remaining -= fill;
            level.total_qty -= fill;
            remaining -= fill;

            Order* next = maker->next;
            if (maker->remaining == 0) {
                // Fully filled maker: pop from the FIFO head.
                level.head = next;
                if (next != nullptr) next->prev = nullptr;
                else level.tail = nullptr;
                --level.order_count;
                --live_orders_;
                index_.erase(maker->id);
                pool_.release(maker);
            }
            maker = next;
        }

        if (level.head == nullptr) opposite.erase(level_it);
    }
    return remaining;
}

template <typename OppositeMap>
std::uint64_t OrderBook::available(const OppositeMap& opposite, const Command& taker,
                                   Qty needed) const {
    std::uint64_t total = 0;
    for (const auto& [price, level] : opposite) {
        if (!crosses(taker, price)) break;
        total += level.total_qty;
        if (total >= needed) break;
    }
    return total;
}

template <typename SameMap>
void OrderBook::rest(SameMap& same, const Command& cmd, Qty remaining) {
    // The engine checked the pool has room before calling add().
    Order* o = pool_.acquire(Order{cmd.id, cmd.price, remaining, cmd.side, symbol_, nullptr,
                                   nullptr, nullptr});
    assert(o != nullptr);

    // try_emplace: O(log L) lookup, creates the level only if it is new.
    auto [it, inserted] = same.try_emplace(cmd.price);
    PriceLevel& level = it->second;
    if (inserted) level.price = cmd.price;

    // Append to the FIFO tail -> time priority.
    o->level = &level;
    o->prev = level.tail;
    if (level.tail != nullptr) level.tail->next = o;
    else level.head = o;
    level.tail = o;
    level.total_qty += remaining;
    ++level.order_count;

    index_.emplace(o->id, o);
    ++live_orders_;
}

template <typename Map>
void OrderBook::unlink(Map& map, Order* o) {
    PriceLevel& level = *o->level;
    if (o->prev != nullptr) o->prev->next = o->next;
    else level.head = o->next;
    if (o->next != nullptr) o->next->prev = o->prev;
    else level.tail = o->prev;
    level.total_qty -= o->remaining;
    --level.order_count;
    if (level.head == nullptr) map.erase(level.price);
}

void OrderBook::cancel(Order* o) {
    const OrderId id = o->id;
    const Qty remaining = o->remaining;
    if (o->side == Side::Buy) unlink(bids_, o);
    else unlink(asks_, o);
    --live_orders_;
    index_.erase(id);
    pool_.release(o);
    emit(EventType::Cancelled, Reason::UserCancel, id, remaining);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::uint64_t OrderBook::volume_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total_qty;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total_qty;
}

std::size_t OrderBook::level_count(Side side) const {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

std::vector<LevelView> OrderBook::depth(Side side, std::size_t max_levels) const {
    std::vector<LevelView> out;
    auto collect = [&](const auto& map) {
        for (const auto& [price, level] : map) {
            if (out.size() >= max_levels) break;
            out.push_back({price, level.total_qty, level.order_count});
        }
    };
    if (side == Side::Buy) collect(bids_);
    else collect(asks_);
    return out;
}

}  // namespace orderflow
