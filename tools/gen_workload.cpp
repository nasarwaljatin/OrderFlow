// orderflow_gen: write a reproducible synthetic workload to a journal file.
//
//   ./orderflow_gen --out workload.ofj --orders 10000000 --seed 42 --symbols 4

#include <cstdio>
#include <cstdlib>
#include <string>

#include "orderflow/journal.hpp"
#include "orderflow/workload.hpp"

int main(int argc, char** argv) {
    std::string out = "workload.ofj";
    std::size_t n = 10'000'000;
    orderflow::WorkloadConfig cfg;

    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        const char* v = argv[i + 1];
        if (a == "--out") out = v;
        else if (a == "--orders") n = std::strtoull(v, nullptr, 10);
        else if (a == "--seed") cfg.seed = std::strtoull(v, nullptr, 10);
        else if (a == "--symbols") cfg.num_symbols = std::strtoull(v, nullptr, 10);
        else {
            std::fprintf(stderr, "usage: orderflow_gen --out FILE --orders N --seed S --symbols K\n");
            return 2;
        }
    }

    orderflow::WorkloadGenerator gen(cfg);
    orderflow::JournalWriter w(out);
    for (std::size_t i = 0; i < n; ++i) w.append(gen.next());
    w.flush();
    std::printf("wrote %llu commands to %s (seed=%llu, symbols=%zu)\n",
                static_cast<unsigned long long>(w.count()), out.c_str(),
                static_cast<unsigned long long>(cfg.seed), cfg.num_symbols);
    return 0;
}
