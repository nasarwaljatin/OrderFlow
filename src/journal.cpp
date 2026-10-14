#include "orderflow/journal.hpp"

#include <cstring>
#include <stdexcept>

#include "orderflow/matching_engine.hpp"

namespace orderflow {

JournalWriter::JournalWriter(const std::string& path) : buffer_(1 << 20) {
    file_ = std::fopen(path.c_str(), "wb");
    if (file_ == nullptr) throw std::runtime_error("cannot open journal for writing: " + path);
    std::setvbuf(file_, buffer_.data(), _IOFBF, buffer_.size());
    JournalHeader h;
    if (std::fwrite(&h, sizeof h, 1, file_) != 1) throw std::runtime_error("journal header write failed");
}

JournalWriter::~JournalWriter() {
    if (file_ != nullptr) std::fclose(file_);
}

void JournalWriter::append(const Command& cmd) {
    if (std::fwrite(&cmd, sizeof cmd, 1, file_) != 1) throw std::runtime_error("journal write failed");
    ++count_;
}

void JournalWriter::flush() { std::fflush(file_); }

JournalReader::JournalReader(const std::string& path) {
    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) throw std::runtime_error("cannot open journal: " + path);
    JournalHeader h;
    if (std::fread(&h, sizeof h, 1, file_) != 1 || std::memcmp(h.magic, "OFJ1", 4) != 0)
        throw std::runtime_error("not an OrderFlow journal: " + path);
    if (h.version != 1) throw std::runtime_error("unsupported journal version");
}

JournalReader::~JournalReader() {
    if (file_ != nullptr) std::fclose(file_);
}

bool JournalReader::next(Command& out) {
    const std::size_t n = std::fread(&out, 1, sizeof out, file_);
    if (n == 0) return false;
    if (n != sizeof out) throw std::runtime_error("truncated journal record");
    return true;
}

std::vector<Command> JournalReader::read_all(const std::string& path) {
    JournalReader r(path);
    std::vector<Command> cmds;
    Command c;
    while (r.next(c)) cmds.push_back(c);
    return cmds;
}

std::uint64_t replay(const std::string& path, MatchingEngine& engine) {
    JournalReader r(path);
    Command c;
    std::uint64_t n = 0;
    while (r.next(c)) {
        engine.process(c);
        ++n;
    }
    return n;
}

}  // namespace orderflow
