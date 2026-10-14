#pragma once
// Core value types shared by every component.
//
// Everything here is a trivially-copyable POD so it can be:
//   * pushed through the lock-free SPSC ring buffer by value,
//   * written to / read from the binary journal with a single memcpy,
//   * serialised onto the wire without per-field encoding logic.

#include <cstdint>
#include <type_traits>

namespace orderflow {

using OrderId  = std::uint64_t;
using Price    = std::int64_t;   // integer ticks, never floating point
using Qty      = std::uint32_t;
using SymbolId = std::uint16_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

enum class OrderType : std::uint8_t {
    Limit  = 0,  // rest any unfilled remainder on the book
    Market = 1,  // take liquidity at any price, cancel the remainder
    IOC    = 2,  // immediate-or-cancel: match up to limit price, cancel remainder
    FOK    = 3,  // fill-or-kill: fill completely up to limit price or do nothing
};

// ---------------------------------------------------------------------------
// Inbound: what clients ask the engine to do.
// ---------------------------------------------------------------------------
enum class CommandType : std::uint8_t { New = 0, Cancel = 1 };

struct Command {
    CommandType type{CommandType::New};
    Side side{Side::Buy};
    OrderType order_type{OrderType::Limit};
    std::uint8_t pad0{0};
    SymbolId symbol{0};
    std::uint16_t pad1{0};
    Qty qty{0};
    std::uint32_t pad2{0};  // explicit padding: no uninitialised bytes in the journal
    OrderId id{0};
    Price price{0};

    static constexpr Command new_order(SymbolId sym, OrderId id, Side side, OrderType t, Price px,
                                       Qty qty) {
        Command c;
        c.type = CommandType::New;
        c.side = side;
        c.order_type = t;
        c.symbol = sym;
        c.qty = qty;
        c.id = id;
        c.price = px;
        return c;
    }
    static constexpr Command limit(SymbolId sym, OrderId id, Side side, Price px, Qty qty) {
        return new_order(sym, id, side, OrderType::Limit, px, qty);
    }
    static constexpr Command cancel(SymbolId sym, OrderId id) {
        Command c;
        c.type = CommandType::Cancel;
        c.symbol = sym;
        c.id = id;
        return c;
    }
};
static_assert(std::is_trivially_copyable_v<Command>);
static_assert(sizeof(Command) == 32, "Command is a fixed 32-byte record (journal + wire format)");

// ---------------------------------------------------------------------------
// Outbound: what the engine reports back.
// ---------------------------------------------------------------------------
enum class EventType : std::uint8_t {
    Accepted  = 0,  // order passed validation and entered the book/matcher
    Rejected  = 1,  // command failed validation; nothing changed
    Trade     = 2,  // taker `id` traded `qty` @ `price` against resting `maker_id`
    Cancelled = 3,  // order (or its remainder) left the book without trading
};

enum class Reason : std::uint8_t {
    None = 0,
    // Rejections
    DuplicateId,
    UnknownOrder,
    InvalidQty,
    InvalidPrice,
    UnknownSymbol,
    PoolExhausted,
    // Cancellations
    UserCancel,
    IocRemainder,
    FokUnfilled,
    MarketNoLiquidity,
};

struct Event {
    EventType type{EventType::Accepted};
    Reason reason{Reason::None};
    SymbolId symbol{0};
    Qty qty{0};          // trade qty / cancelled remaining qty / accepted qty
    OrderId id{0};       // the order this event is about (taker for trades)
    OrderId maker_id{0}; // resting counterparty (trades only)
    Price price{0};      // trade price (trades only)

    friend bool operator==(const Event&, const Event&) = default;
};
static_assert(std::is_trivially_copyable_v<Event>);
static_assert(sizeof(Event) == 32, "Event is a fixed 32-byte record (wire format)");

constexpr const char* to_string(EventType t) {
    switch (t) {
        case EventType::Accepted: return "Accepted";
        case EventType::Rejected: return "Rejected";
        case EventType::Trade: return "Trade";
        case EventType::Cancelled: return "Cancelled";
    }
    return "?";
}

constexpr const char* to_string(Reason r) {
    switch (r) {
        case Reason::None: return "None";
        case Reason::DuplicateId: return "DuplicateId";
        case Reason::UnknownOrder: return "UnknownOrder";
        case Reason::InvalidQty: return "InvalidQty";
        case Reason::InvalidPrice: return "InvalidPrice";
        case Reason::UnknownSymbol: return "UnknownSymbol";
        case Reason::PoolExhausted: return "PoolExhausted";
        case Reason::UserCancel: return "UserCancel";
        case Reason::IocRemainder: return "IocRemainder";
        case Reason::FokUnfilled: return "FokUnfilled";
        case Reason::MarketNoLiquidity: return "MarketNoLiquidity";
    }
    return "?";
}

}  // namespace orderflow
