#pragma once
#include <gtest/gtest.h>

#include <vector>

#include "orderflow/event_sink.hpp"
#include "orderflow/matching_engine.hpp"

namespace orderflow::test {

// Fixture: one engine with a VectorSink, plus terse helpers.
class EngineTest : public ::testing::Test {
protected:
    explicit EngineTest(std::size_t symbols = 2, std::size_t max_orders = 1024)
        : engine_(EngineConfig{symbols, max_orders}, sink_) {}

    const std::vector<Event>& events() const { return sink_.events; }
    void clear() { sink_.clear(); }

    void buy(OrderId id, Price px, Qty q, SymbolId s = 0) { engine_.process(Command::limit(s, id, Side::Buy, px, q)); }
    void sell(OrderId id, Price px, Qty q, SymbolId s = 0) { engine_.process(Command::limit(s, id, Side::Sell, px, q)); }
    void send(OrderType t, Side side, OrderId id, Price px, Qty q, SymbolId s = 0) {
        engine_.process(Command::new_order(s, id, side, t, px, q));
    }
    void cancel(OrderId id, SymbolId s = 0) { engine_.process(Command::cancel(s, id)); }

    const OrderBook& book(SymbolId s = 0) const { return engine_.book(s); }

    std::vector<Event> trades() const {
        std::vector<Event> t;
        for (const auto& e : sink_.events)
            if (e.type == EventType::Trade) t.push_back(e);
        return t;
    }
    std::size_t count(EventType t) const {
        std::size_t n = 0;
        for (const auto& e : sink_.events) n += e.type == t;
        return n;
    }

    VectorSink sink_;
    MatchingEngine engine_;
};

inline Event make_trade(OrderId taker, OrderId maker, Price px, Qty q, SymbolId s = 0) {
    Event e;
    e.type = EventType::Trade;
    e.symbol = s;
    e.id = taker;
    e.maker_id = maker;
    e.price = px;
    e.qty = q;
    return e;
}

inline Event make_event(EventType t, Reason r, OrderId id, Qty q, SymbolId s = 0) {
    Event e;
    e.type = t;
    e.reason = r;
    e.symbol = s;
    e.id = id;
    e.qty = q;
    return e;
}

}  // namespace orderflow::test

namespace orderflow {
inline void PrintTo(const Event& e, std::ostream* os) {
    *os << to_string(e.type) << "{reason=" << to_string(e.reason) << " sym=" << e.symbol
        << " id=" << e.id << " maker=" << e.maker_id << " px=" << e.price << " qty=" << e.qty << "}";
}
}  // namespace orderflow
