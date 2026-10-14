#pragma once
// Event-sourcing journal.
//
// The engine's *inputs* (Commands) are appended to a binary file. Because the
// engine is deterministic, replaying the journal rebuilds the exact same book
// and re-emits the exact same events. This gives you:
//   * crash recovery (replay the journal on restart),
//   * reproducible bug reports (ship the journal, not a description),
//   * a fixed benchmark workload (generate once, replay many times).
//
// File format (little-endian):
//   [Header: magic "OFJ1" (4B) | version (4B) | reserved (8B)]
//   [Command 32B][Command 32B]...

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "orderflow/types.hpp"

namespace orderflow {

class MatchingEngine;

struct JournalHeader {
    char magic[4]{'O', 'F', 'J', '1'};
    std::uint32_t version{1};
    std::uint64_t reserved{0};
};
static_assert(sizeof(JournalHeader) == 16);

class JournalWriter {
public:
    explicit JournalWriter(const std::string& path);
    ~JournalWriter();
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    void append(const Command& cmd);
    void flush();
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

private:
    std::FILE* file_{nullptr};
    std::vector<char> buffer_;
    std::uint64_t count_{0};
};

class JournalReader {
public:
    explicit JournalReader(const std::string& path);
    ~JournalReader();
    JournalReader(const JournalReader&) = delete;
    JournalReader& operator=(const JournalReader&) = delete;

    // Returns false at end of file. Throws on a truncated record.
    bool next(Command& out);

    // Convenience: load an entire journal into memory (used by benchmarks so
    // disk I/O isn't part of the measurement).
    static std::vector<Command> read_all(const std::string& path);

private:
    std::FILE* file_{nullptr};
};

// Feed every command in the journal to the engine. Returns commands replayed.
std::uint64_t replay(const std::string& path, MatchingEngine& engine);

}  // namespace orderflow
