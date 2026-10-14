// Journal round-trip and deterministic replay.
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "orderflow/event_sink.hpp"
#include "orderflow/journal.hpp"
#include "orderflow/matching_engine.hpp"
#include "orderflow/workload.hpp"

using namespace orderflow;

namespace {
std::string temp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
}  // namespace

TEST(Journal, RoundTripPreservesEveryField) {
    const auto path = temp_path("of_roundtrip.ofj");
    const auto a = Command::new_order(2, 123456789, Side::Sell, OrderType::FOK, -77, 4242);
    const auto b = Command::cancel(1, 99);
    {
        JournalWriter w(path);
        w.append(a);
        w.append(b);
        EXPECT_EQ(w.count(), 2u);
    }
    const auto cmds = JournalReader::read_all(path);
    ASSERT_EQ(cmds.size(), 2u);
    EXPECT_EQ(cmds[0].id, a.id);
    EXPECT_EQ(cmds[0].price, a.price);
    EXPECT_EQ(cmds[0].qty, a.qty);
    EXPECT_EQ(cmds[0].order_type, OrderType::FOK);
    EXPECT_EQ(cmds[0].side, Side::Sell);
    EXPECT_EQ(cmds[0].symbol, 2u);
    EXPECT_EQ(cmds[1].type, CommandType::Cancel);
    std::filesystem::remove(path);
}

TEST(Journal, EmptyJournalReadsNothing) {
    const auto path = temp_path("of_empty.ofj");
    { JournalWriter w(path); }
    EXPECT_TRUE(JournalReader::read_all(path).empty());
    std::filesystem::remove(path);
}

TEST(Journal, RejectsFileWithoutMagic) {
    const auto path = temp_path("of_bad.ofj");
    {
        std::ofstream f(path, std::ios::binary);
        f << "this is not a journal file at all";
    }
    EXPECT_THROW(JournalReader r(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(Journal, DetectsTruncatedRecord) {
    const auto path = temp_path("of_trunc.ofj");
    {
        JournalWriter w(path);
        w.append(Command::limit(0, 1, Side::Buy, 100, 1));
    }
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 5);
    JournalReader r(path);
    Command c;
    EXPECT_THROW(r.next(c), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(Journal, MissingFileThrows) {
    EXPECT_THROW(JournalReader r("/nonexistent/dir/file.ofj"), std::runtime_error);
}

// The key property behind event sourcing.
TEST(Journal, ReplayIsDeterministic) {
    const auto path = temp_path("of_replay.ofj");
    WorkloadGenerator gen(WorkloadConfig{.seed = 9, .num_symbols = 4, .target_live = 2000});
    {
        JournalWriter w(path);
        for (int i = 0; i < 50'000; ++i) w.append(gen.next());
    }
    auto run = [&] {
        HashingSink h;
        MatchingEngine e(EngineConfig{4, 1 << 16}, h);
        EXPECT_EQ(replay(path, e), 50'000u);
        return h.hash;
    };
    const auto h1 = run();
    const auto h2 = run();
    EXPECT_EQ(h1, h2);
    std::filesystem::remove(path);
}

TEST(Journal, ReplayRebuildsSameBookAsLiveRun) {
    const auto path = temp_path("of_rebuild.ofj");
    WorkloadGenerator gen(WorkloadConfig{.seed = 3, .num_symbols = 2, .target_live = 500});
    CountingSink live_sink;
    MatchingEngine live(EngineConfig{2, 1 << 16}, live_sink);
    {
        JournalWriter w(path);
        for (int i = 0; i < 20'000; ++i) {
            const auto c = gen.next();
            w.append(c);
            live.process(c);
        }
    }
    CountingSink replay_sink;
    MatchingEngine rebuilt(EngineConfig{2, 1 << 16}, replay_sink);
    replay(path, rebuilt);

    EXPECT_EQ(live.live_orders(), rebuilt.live_orders());
    for (SymbolId s = 0; s < 2; ++s) {
        EXPECT_EQ(live.book(s).depth(Side::Buy, 50), rebuilt.book(s).depth(Side::Buy, 50));
        EXPECT_EQ(live.book(s).depth(Side::Sell, 50), rebuilt.book(s).depth(Side::Sell, 50));
    }
    EXPECT_EQ(live_sink.traded_qty, replay_sink.traded_qty);
    std::filesystem::remove(path);
}

TEST(Workload, SameSeedSameStream) {
    WorkloadGenerator a(WorkloadConfig{.seed = 1}), b(WorkloadConfig{.seed = 1});
    for (int i = 0; i < 1000; ++i) {
        const auto x = a.next();
        const auto y = b.next();
        ASSERT_EQ(x.id, y.id);
        ASSERT_EQ(x.price, y.price);
        ASSERT_EQ(x.type, y.type);
    }
}

TEST(Workload, ProducesAllCommandKinds) {
    WorkloadGenerator g(WorkloadConfig{.seed = 5, .num_symbols = 1, .target_live = 100});
    int kinds[4]{}, cancels = 0;
    for (int i = 0; i < 20'000; ++i) {
        const auto c = g.next();
        if (c.type == CommandType::Cancel) ++cancels;
        else ++kinds[static_cast<int>(c.order_type)];
    }
    for (int k : kinds) EXPECT_GT(k, 0);
    EXPECT_GT(cancels, 0);
}
