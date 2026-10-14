// Micro-benchmarks (Google Benchmark).
//
//   ./orderflow_bench                                  # all
//   ./orderflow_bench --benchmark_filter=Replay        # one family
//   ./orderflow_bench --benchmark_format=json > out.json
#include <benchmark/benchmark.h>

#include <vector>

#include "orderflow/event_sink.hpp"
#include "orderflow/matching_engine.hpp"
#include "orderflow/spsc_ring.hpp"
#include "orderflow/workload.hpp"

using namespace orderflow;

// Add a passive order and cancel it: the two most frequent operations on a
// real exchange (the vast majority of orders are cancelled, not filled).
static void BM_AddCancel(benchmark::State& state) {
    CountingSink sink;
    MatchingEngine engine(EngineConfig{1, 1 << 16}, sink);
    // Background liquidity so levels/maps are non-trivial.
    for (OrderId i = 1; i <= 1000; ++i)
        engine.process(Command::limit(0, i, Side::Buy, 1000 - static_cast<Price>(i % 50), 10));
    OrderId id = 1'000'000;
    for (auto _ : state) {
        engine.process(Command::limit(0, id, Side::Buy, 990, 10));
        engine.process(Command::cancel(0, id));
        ++id;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_AddCancel);

// Add a maker then a taker that fully fills it (one trade per iteration).
static void BM_AddAndMatch(benchmark::State& state) {
    CountingSink sink;
    MatchingEngine engine(EngineConfig{1, 1 << 16}, sink);
    OrderId id = 1;
    for (auto _ : state) {
        engine.process(Command::limit(0, id++, Side::Sell, 100, 10));
        engine.process(Command::limit(0, id++, Side::Buy, 100, 10));
    }
    state.SetItemsProcessed(state.iterations() * 2);
    benchmark::DoNotOptimize(sink.trades());
}
BENCHMARK(BM_AddAndMatch);

// One aggressive order sweeping N price levels.
static void BM_SweepLevels(benchmark::State& state) {
    const auto levels = static_cast<Price>(state.range(0));
    CountingSink sink;
    MatchingEngine engine(EngineConfig{1, 1 << 16}, sink);
    OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        for (Price p = 0; p < levels; ++p) engine.process(Command::limit(0, id++, Side::Sell, 100 + p, 1));
        state.ResumeTiming();
        engine.process(Command::new_order(0, id++, Side::Buy, OrderType::Market, 0,
                                          static_cast<Qty>(levels)));
    }
    state.SetItemsProcessed(state.iterations() * levels);  // fills per second
}
BENCHMARK(BM_SweepLevels)->Arg(1)->Arg(10)->Arg(100);

// The headline number: a realistic mixed workload replayed from memory.
static void BM_ReplayWorkload(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    static std::vector<Command> cmds;
    if (cmds.size() != n) {
        WorkloadGenerator gen(WorkloadConfig{});
        cmds = gen.generate(n);
    }
    for (auto _ : state) {
        state.PauseTiming();
        auto sink = std::make_unique<CountingSink>();
        auto engine = std::make_unique<MatchingEngine>(EngineConfig{4, 1 << 20}, *sink);
        state.ResumeTiming();
        for (const auto& c : cmds) engine->process(c);
        benchmark::DoNotOptimize(sink->trades());
        state.PauseTiming();
        engine.reset();  // don't time teardown
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_ReplayWorkload)->Arg(1'000'000)->Unit(benchmark::kMillisecond);

// Single-threaded push+pop cost of the lock-free ring.
static void BM_SpscPushPop(benchmark::State& state) {
    SpscRing<Command> ring(1024);
    const auto c = Command::limit(0, 1, Side::Buy, 100, 1);
    Command out;
    for (auto _ : state) {
        ring.try_push(c);
        ring.try_pop(out);
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscPushPop);
