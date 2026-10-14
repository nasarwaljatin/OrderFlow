#pragma once
// TCP order-entry gateway.
//
//  clients ──TCP──▶ [network thread] ──SPSC ring (Inbound)──▶ [engine thread]
//                         ▲                                        │
//                         └────────── SPSC ring (Outbound) ◀───────┘
//
// * The network thread owns every socket (poll()-based, non-blocking I/O).
// * The engine thread owns the MatchingEngine. It never touches a socket and
//   never takes a lock: its only contact with the outside world is two
//   lock-free single-producer/single-consumer rings.
// * After pushing a batch of events, the engine thread writes one byte to a
//   self-pipe so the network thread wakes from poll() immediately.
//
// Wire protocol (host byte order, i.e. little-endian on x86/ARM):
//   client -> server : orderflow::Command, 32 bytes each
//   server -> client : orderflow::Event,   32 bytes each
//
// Each client only receives events about its own orders, and may only cancel
// orders it submitted.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "orderflow/types.hpp"

namespace orderflow {

struct GatewayConfig {
    std::uint16_t port{9000};          // 0 = pick a free port (used by tests)
    std::size_t num_symbols{4};
    std::size_t max_orders{1u << 20};
    std::size_t ring_capacity{1u << 16};
    bool busy_spin{false};             // true = engine thread never sleeps (lowest latency, 100% CPU)
    std::string journal_path;          // empty = no journaling
};

struct GatewayStats {
    std::atomic<std::uint64_t> commands_in{0};
    std::atomic<std::uint64_t> events_out{0};
    std::atomic<std::uint64_t> connections{0};
    std::atomic<std::uint64_t> ownership_rejects{0};
};

class Gateway {
public:
    explicit Gateway(GatewayConfig cfg);
    ~Gateway();
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    // Binds, listens and starts both threads. Throws on failure.
    void start();
    // Signals both threads to stop and joins them. Idempotent.
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept { return bound_port_; }
    [[nodiscard]] const GatewayStats& stats() const noexcept { return stats_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    GatewayConfig cfg_;
    GatewayStats stats_;
    std::uint16_t bound_port_{0};
    std::atomic<bool> running_{false};
    std::thread network_thread_;
    std::thread engine_thread_;
};

}  // namespace orderflow
