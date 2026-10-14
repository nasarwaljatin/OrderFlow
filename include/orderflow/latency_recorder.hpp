#pragma once
// Records per-operation latencies and reports percentiles.
//
// Averages hide the problem in trading systems; the tail (p99, p99.9, max) is
// what matters. We keep every sample and sort once at the end - simple and
// exact. (A production system would use an HDR histogram instead.)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace orderflow {

class LatencyRecorder {
public:
    explicit LatencyRecorder(std::size_t expected = 0) { samples_.reserve(expected); }

    void record(std::uint64_t ns) { samples_.push_back(ns); }

    struct Summary {
        std::size_t count{0};
        double mean{0};
        std::uint64_t min{0}, p50{0}, p90{0}, p99{0}, p999{0}, max{0};
    };

    Summary summarize() {
        Summary s;
        s.count = samples_.size();
        if (samples_.empty()) return s;
        std::sort(samples_.begin(), samples_.end());
        auto pct = [&](double p) {
            auto idx = static_cast<std::size_t>(p * static_cast<double>(samples_.size() - 1));
            return samples_[idx];
        };
        long double sum = 0;
        for (auto v : samples_) sum += v;
        s.mean = static_cast<double>(sum / samples_.size());
        s.min = samples_.front();
        s.p50 = pct(0.50);
        s.p90 = pct(0.90);
        s.p99 = pct(0.99);
        s.p999 = pct(0.999);
        s.max = samples_.back();
        return s;
    }

    static void print(const char* label, const Summary& s, const char* unit = "ns") {
        std::printf("%-22s n=%zu  mean=%.1f%s  p50=%llu  p90=%llu  p99=%llu  p99.9=%llu  max=%llu (%s)\n",
                    label, s.count, s.mean, unit, static_cast<unsigned long long>(s.p50),
                    static_cast<unsigned long long>(s.p90), static_cast<unsigned long long>(s.p99),
                    static_cast<unsigned long long>(s.p999), static_cast<unsigned long long>(s.max),
                    unit);
    }

private:
    std::vector<std::uint64_t> samples_;
};

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace orderflow
