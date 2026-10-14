// orderflow_server: run the matching engine behind a TCP gateway.
//
//   ./orderflow_server --port 9000 --symbols 4 --max-orders 1048576
//                      --journal orders.ofj [--busy-spin]

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "gateway.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

void usage() {
    std::puts(
        "usage: orderflow_server [--port N] [--symbols N] [--max-orders N]\n"
        "                        [--ring N] [--journal PATH] [--busy-spin]");
}
}  // namespace

int main(int argc, char** argv) {
    orderflow::GatewayConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") cfg.port = static_cast<std::uint16_t>(std::atoi(val()));
        else if (a == "--symbols") cfg.num_symbols = std::strtoull(val(), nullptr, 10);
        else if (a == "--max-orders") cfg.max_orders = std::strtoull(val(), nullptr, 10);
        else if (a == "--ring") cfg.ring_capacity = std::strtoull(val(), nullptr, 10);
        else if (a == "--journal") cfg.journal_path = val();
        else if (a == "--busy-spin") cfg.busy_spin = true;
        else {
            usage();
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    orderflow::Gateway gw(cfg);
    try {
        gw.start();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "failed to start: %s\n", e.what());
        return 1;
    }
    std::printf("OrderFlow listening on port %u (%zu symbols, %zu max resting orders%s)\n",
                gw.port(), cfg.num_symbols, cfg.max_orders,
                cfg.journal_path.empty() ? "" : (", journal " + cfg.journal_path).c_str());
    std::fflush(stdout);

    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    gw.stop();
    const auto& s = gw.stats();
    std::printf("\nshutdown: %llu commands, %llu events, %llu connections, %llu cancels rejected at gateway (unknown or not owned)\n",
                static_cast<unsigned long long>(s.commands_in.load()),
                static_cast<unsigned long long>(s.events_out.load()),
                static_cast<unsigned long long>(s.connections.load()),
                static_cast<unsigned long long>(s.ownership_rejects.load()));
    return 0;
}
