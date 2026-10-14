// orderflow_latency: throughput + per-command latency percentiles.
//
//   ./orderflow_latency                       # 10M synthetic commands
//   ./orderflow_latency --orders 2000000
//   ./orderflow_latency --journal workload.ofj
//
// Two passes over the same in-memory command stream:
//   1. Throughput pass - no timing inside the loop, measures raw commands/sec.
//   2. Latency pass    - timestamps every process() call (adds ~20-40 ns of
//                        clock overhead per sample, so reported latencies are
//                        a slight over-estimate).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "orderflow/event_sink.hpp"
#include "orderflow/journal.hpp"
#include "orderflow/latency_recorder.hpp"
#include "orderflow/matching_engine.hpp"
#include "orderflow/workload.hpp"

using namespace orderflow;

int main(int argc, char** argv) {
    std::size_t n = 10'000'000;
    std::size_t symbols = 4;
    std::string journal;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--orders") n = std::strtoull(argv[i + 1], nullptr, 10);
        else if (a == "--symbols") symbols = std::strtoull(argv[i + 1], nullptr, 10);
        else if (a == "--journal") journal = argv[i + 1];
    }

    std::vector<Command> cmds;
    if (!journal.empty()) {
        cmds = JournalReader::read_all(journal);
    } else {
        WorkloadConfig wc;
        wc.num_symbols = symbols;
        cmds = WorkloadGenerator(wc).generate(n);
    }
    n = cmds.size();
    std::printf("workload: %zu commands, %zu symbols\n", n, symbols);

    const EngineConfig cfg{symbols, 1u << 21};

    // ---- Pass 1: throughput ----
    {
        CountingSink sink;
        MatchingEngine engine(cfg, sink);
        const std::uint64_t t0 = now_ns();
        for (const auto& c : cmds) engine.process(c);
        const double secs = static_cast<double>(now_ns() - t0) / 1e9;
        std::printf("throughput: %.2f M commands/s  (%.3f s total)\n",
                    static_cast<double>(n) / secs / 1e6, secs);
        std::printf("events: accepted=%llu rejected=%llu trades=%llu cancelled=%llu  resting at end=%zu\n",
                    static_cast<unsigned long long>(sink.accepted()),
                    static_cast<unsigned long long>(sink.rejected()),
                    static_cast<unsigned long long>(sink.trades()),
                    static_cast<unsigned long long>(sink.cancelled()), engine.live_orders());
    }

    // ---- Pass 2: per-command latency ----
    {
        CountingSink sink;
        MatchingEngine engine(cfg, sink);
        LatencyRecorder all(n), news(n), cancels(n / 2);
        for (const auto& c : cmds) {
            const std::uint64_t t0 = now_ns();
            engine.process(c);
            const std::uint64_t dt = now_ns() - t0;
            all.record(dt);
            if (c.type == CommandType::New) news.record(dt);
            else cancels.record(dt);
        }
        LatencyRecorder::print("all commands", all.summarize());
        LatencyRecorder::print("new orders", news.summarize());
        LatencyRecorder::print("cancels", cancels.summarize());
    }
    return 0;
}
