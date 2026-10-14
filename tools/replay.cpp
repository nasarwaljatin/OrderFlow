// orderflow_replay: rebuild engine state from a journal and print a summary.
//
//   ./orderflow_replay workload.ofj [--symbols 4] [--max-orders 1048576]
//
// The printed hash is a fingerprint of the *entire* output event stream. Run
// it twice (or on two machines) - identical hashes prove determinism.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "orderflow/event_sink.hpp"
#include "orderflow/journal.hpp"
#include "orderflow/latency_recorder.hpp"
#include "orderflow/matching_engine.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: orderflow_replay FILE [--symbols N] [--max-orders N]\n");
        return 2;
    }
    const std::string path = argv[1];
    orderflow::EngineConfig cfg;
    cfg.num_symbols = 4;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--symbols") cfg.num_symbols = std::strtoull(argv[i + 1], nullptr, 10);
        else if (a == "--max-orders") cfg.max_orders = std::strtoull(argv[i + 1], nullptr, 10);
    }

    orderflow::HashingSink hash;
    orderflow::CountingSink counts;
    orderflow::TeeSink tee(hash, counts);
    orderflow::MatchingEngine engine(cfg, tee);

    const std::uint64_t t0 = orderflow::now_ns();
    const std::uint64_t n = orderflow::replay(path, engine);
    const double secs = static_cast<double>(orderflow::now_ns() - t0) / 1e9;

    std::printf("replayed %llu commands in %.3f s (%.0f cmd/s, includes disk I/O)\n",
                static_cast<unsigned long long>(n), secs, static_cast<double>(n) / secs);
    std::printf("events: accepted=%llu rejected=%llu trades=%llu cancelled=%llu traded_qty=%llu\n",
                static_cast<unsigned long long>(counts.accepted()),
                static_cast<unsigned long long>(counts.rejected()),
                static_cast<unsigned long long>(counts.trades()),
                static_cast<unsigned long long>(counts.cancelled()),
                static_cast<unsigned long long>(counts.traded_qty));
    std::printf("resting orders at end: %zu\n", engine.live_orders());
    for (std::size_t s = 0; s < engine.num_symbols(); ++s) {
        const auto& b = engine.book(static_cast<orderflow::SymbolId>(s));
        auto bid = b.best_bid();
        auto ask = b.best_ask();
        std::printf("  symbol %zu: best bid %lld | best ask %lld | %zu bid levels, %zu ask levels\n", s,
                    bid ? static_cast<long long>(*bid) : -1LL, ask ? static_cast<long long>(*ask) : -1LL,
                    b.level_count(orderflow::Side::Buy), b.level_count(orderflow::Side::Sell));
    }
    std::printf("event-stream hash: %016llx (%llu events)\n",
                static_cast<unsigned long long>(hash.hash), static_cast<unsigned long long>(hash.count));
    return 0;
}
