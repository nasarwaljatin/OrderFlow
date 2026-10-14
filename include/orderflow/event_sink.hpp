#pragma once
// Where the engine sends its output events.
//
// The engine is decoupled from *what happens* to events: tests collect them
// in a vector, benchmarks count them, the replay tool hashes them, and the
// TCP gateway pushes them onto a lock-free ring for the network thread.

#include <cstdint>
#include <cstring>
#include <vector>

#include "orderflow/types.hpp"

namespace orderflow {

class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void on_event(const Event& e) = 0;
};

// Collects every event. Used by unit tests.
class VectorSink final : public EventSink {
public:
    void on_event(const Event& e) override { events.push_back(e); }
    void clear() { events.clear(); }
    std::vector<Event> events;
};

// Counts events by type and accumulates traded volume. Used by benchmarks.
class CountingSink final : public EventSink {
public:
    void on_event(const Event& e) override {
        ++counts[static_cast<std::size_t>(e.type)];
        if (e.type == EventType::Trade) traded_qty += e.qty;
    }
    std::uint64_t accepted() const { return counts[0]; }
    std::uint64_t rejected() const { return counts[1]; }
    std::uint64_t trades() const { return counts[2]; }
    std::uint64_t cancelled() const { return counts[3]; }
    std::uint64_t counts[4]{};
    std::uint64_t traded_qty{0};
};

// FNV-1a hash over the full event stream. Two runs over the same journal must
// produce the same hash -> proves the engine is deterministic.
class HashingSink final : public EventSink {
public:
    void on_event(const Event& e) override {
        unsigned char buf[sizeof(Event)];
        std::memcpy(buf, &e, sizeof(Event));
        for (unsigned char b : buf) {
            hash ^= b;
            hash *= 1099511628211ULL;
        }
        ++count;
    }
    std::uint64_t hash{14695981039346656037ULL};
    std::uint64_t count{0};
};

// Fan out to two sinks (e.g. hash + count).
class TeeSink final : public EventSink {
public:
    TeeSink(EventSink& a, EventSink& b) : a_(a), b_(b) {}
    void on_event(const Event& e) override {
        a_.on_event(e);
        b_.on_event(e);
    }

private:
    EventSink& a_;
    EventSink& b_;
};

}  // namespace orderflow
