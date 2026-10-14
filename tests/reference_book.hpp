#pragma once
// A deliberately naive reference implementation of the matching rules.
//
// It keeps every resting order in one flat vector and finds the best maker by
// linear scan. It is slow (O(n) per match) but small enough to verify by eye,
// which is the point: the fuzz test feeds the same random commands to this
// model and to the optimised engine and requires *identical* event streams.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "orderflow/types.hpp"

namespace orderflow::test {

class ReferenceEngine {
public:
    explicit ReferenceEngine(std::size_t symbols) : symbols_(symbols) {}

    std::vector<Event> events;

    void process(const Command& c) {
        if (c.symbol >= symbols_) return reject(c, Reason::UnknownSymbol);
        if (c.type == CommandType::Cancel) {
            auto it = find(c.id);
            if (it == book_.end() || it->symbol != c.symbol) return reject(c, Reason::UnknownOrder);
            emit(EventType::Cancelled, Reason::UserCancel, c.symbol, c.id, it->qty);
            book_.erase(it);
            return;
        }
        if (c.qty == 0) return reject(c, Reason::InvalidQty);
        if (c.order_type != OrderType::Market && c.price <= 0) return reject(c, Reason::InvalidPrice);
        if (find(c.id) != book_.end()) return reject(c, Reason::DuplicateId);

        emit(EventType::Accepted, Reason::None, c.symbol, c.id, c.qty);

        if (c.order_type == OrderType::FOK) {
            std::uint64_t avail = 0;
            for (const auto& o : book_)
                if (o.symbol == c.symbol && o.side != c.side && acceptable(c, o.price)) avail += o.qty;
            if (avail < c.qty) {
                emit(EventType::Cancelled, Reason::FokUnfilled, c.symbol, c.id, c.qty);
                return;
            }
        }

        Qty remaining = c.qty;
        while (remaining > 0) {
            auto best = best_maker(c);
            if (!best) break;
            Resting& m = book_[*best];
            const Qty fill = std::min(remaining, m.qty);
            Event t;
            t.type = EventType::Trade;
            t.symbol = c.symbol;
            t.id = c.id;
            t.maker_id = m.id;
            t.price = m.price;
            t.qty = fill;
            events.push_back(t);
            m.qty -= fill;
            remaining -= fill;
            if (m.qty == 0) book_.erase(book_.begin() + static_cast<std::ptrdiff_t>(*best));
        }
        if (remaining == 0) return;

        switch (c.order_type) {
            case OrderType::Limit:
                book_.push_back({c.id, c.symbol, c.side, c.price, remaining, seq_++});
                break;
            case OrderType::IOC:
                emit(EventType::Cancelled, Reason::IocRemainder, c.symbol, c.id, remaining);
                break;
            case OrderType::Market:
                emit(EventType::Cancelled, Reason::MarketNoLiquidity, c.symbol, c.id, remaining);
                break;
            case OrderType::FOK:
                break;
        }
    }

    std::optional<Price> best(SymbolId s, Side side) const {
        std::optional<Price> b;
        for (const auto& o : book_) {
            if (o.symbol != s || o.side != side) continue;
            if (!b || (side == Side::Buy ? o.price > *b : o.price < *b)) b = o.price;
        }
        return b;
    }

    std::uint64_t volume_at(SymbolId s, Side side, Price px) const {
        std::uint64_t v = 0;
        for (const auto& o : book_)
            if (o.symbol == s && o.side == side && o.price == px) v += o.qty;
        return v;
    }

    std::size_t live() const { return book_.size(); }

private:
    struct Resting {
        OrderId id;
        SymbolId symbol;
        Side side;
        Price price;
        Qty qty;
        std::uint64_t seq;
    };

    static bool acceptable(const Command& c, Price maker_px) {
        if (c.order_type == OrderType::Market) return true;
        return c.side == Side::Buy ? maker_px <= c.price : maker_px >= c.price;
    }

    // Best price first, then lowest sequence number (oldest).
    std::optional<std::size_t> best_maker(const Command& c) const {
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < book_.size(); ++i) {
            const auto& o = book_[i];
            if (o.symbol != c.symbol || o.side == c.side || !acceptable(c, o.price)) continue;
            if (!best) {
                best = i;
                continue;
            }
            const auto& b = book_[*best];
            const bool better_px = c.side == Side::Buy ? o.price < b.price : o.price > b.price;
            if (better_px || (o.price == b.price && o.seq < b.seq)) best = i;
        }
        return best;
    }

    std::vector<Resting>::iterator find(OrderId id) {
        return std::find_if(book_.begin(), book_.end(), [&](const Resting& r) { return r.id == id; });
    }

    void reject(const Command& c, Reason r) {
        Event e;
        e.type = EventType::Rejected;
        e.reason = r;
        e.symbol = c.symbol;
        e.qty = c.qty;
        e.id = c.id;
        events.push_back(e);
    }

    void emit(EventType t, Reason r, SymbolId s, OrderId id, Qty q) {
        Event e;
        e.type = t;
        e.reason = r;
        e.symbol = s;
        e.id = id;
        e.qty = q;
        events.push_back(e);
    }

    std::size_t symbols_;
    std::vector<Resting> book_;
    std::uint64_t seq_{0};
};

}  // namespace orderflow::test
