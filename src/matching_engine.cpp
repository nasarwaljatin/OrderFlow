#include "orderflow/matching_engine.hpp"

namespace orderflow {

MatchingEngine::MatchingEngine(const EngineConfig& cfg, EventSink& sink)
    : sink_(sink), pool_(cfg.max_orders) {
    // Reserve the index up front: a rehash in the middle of trading is a
    // multi-millisecond latency spike.
    index_.reserve(cfg.max_orders);
    books_.reserve(cfg.num_symbols);
    for (std::size_t s = 0; s < cfg.num_symbols; ++s) {
        books_.push_back(
            std::make_unique<OrderBook>(static_cast<SymbolId>(s), pool_, index_, sink_));
    }
}

void MatchingEngine::reject(const Command& cmd, Reason r) {
    Event e;
    e.type = EventType::Rejected;
    e.reason = r;
    e.symbol = cmd.symbol;
    e.qty = cmd.qty;
    e.id = cmd.id;
    sink_.on_event(e);
}

void MatchingEngine::process(const Command& cmd) {
    ++processed_;

    if (cmd.symbol >= books_.size()) {
        reject(cmd, Reason::UnknownSymbol);
        return;
    }
    OrderBook& book = *books_[cmd.symbol];

    if (cmd.type == CommandType::Cancel) {
        auto it = index_.find(cmd.id);
        if (it == index_.end() || it->second->symbol != cmd.symbol) {
            reject(cmd, Reason::UnknownOrder);
            return;
        }
        book.cancel(it->second);
        return;
    }

    // ---- New order validation ----
    if (cmd.qty == 0) {
        reject(cmd, Reason::InvalidQty);
        return;
    }
    if (cmd.order_type != OrderType::Market && cmd.price <= 0) {
        reject(cmd, Reason::InvalidPrice);
        return;
    }
    if (index_.find(cmd.id) != index_.end()) {
        reject(cmd, Reason::DuplicateId);
        return;
    }
    // Only Limit orders can rest, so only they need a free pool slot.
    // (Conservative: we reject even if the order would have fully matched.)
    if (cmd.order_type == OrderType::Limit && pool_.empty()) {
        reject(cmd, Reason::PoolExhausted);
        return;
    }

    book.add(cmd);
}

}  // namespace orderflow
