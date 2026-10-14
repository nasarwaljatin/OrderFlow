// orderflow_client: load generator + round-trip latency measurement.
//
//   ./orderflow_client --host 127.0.0.1 --port 9000 --orders 200000 --window 64
//
// Sends a synthetic order stream (same generator as the benchmarks), keeping at
// most `--window` commands in flight, and measures the round-trip time from
// send() until the first response for that command (Accepted/Rejected for a
// new order, Cancelled/Rejected for a cancel).

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "orderflow/latency_recorder.hpp"
#include "orderflow/types.hpp"
#include "orderflow/workload.hpp"

using namespace orderflow;

namespace {

bool send_all(int fd, const void* data, std::size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 9000;
    std::size_t orders = 100'000;
    std::size_t window = 64;
    std::uint64_t seed = 7;
    std::size_t symbols = 4;
    OrderId id_offset = 0;

    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        const char* v = argv[i + 1];
        if (a == "--host") host = v;
        else if (a == "--port") port = std::atoi(v);
        else if (a == "--orders") orders = std::strtoull(v, nullptr, 10);
        else if (a == "--window") window = std::strtoull(v, nullptr, 10);
        else if (a == "--seed") seed = std::strtoull(v, nullptr, 10);
        else if (a == "--symbols") symbols = std::strtoull(v, nullptr, 10);
        else if (a == "--id-offset") id_offset = std::strtoull(v, nullptr, 10);
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }

    // Resolve host names too (e.g. "server" inside docker compose).
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || res == nullptr) {
        std::fprintf(stderr, "cannot resolve %s\n", host.c_str());
        return 1;
    }
    const int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        std::perror("connect");
        ::freeaddrinfo(res);
        return 1;
    }
    ::freeaddrinfo(res);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    WorkloadConfig wc;
    wc.seed = seed;
    wc.num_symbols = symbols;
    wc.target_live = 5'000;
    WorkloadGenerator gen(wc);

    std::unordered_map<OrderId, std::uint64_t> pending_new, pending_cancel;
    LatencyRecorder rtt(orders);
    std::size_t sent = 0, completed = 0;
    std::uint64_t trades = 0, rejects = 0;
    std::vector<char> rbuf;
    char tmp[64 * 1024];

    const std::uint64_t t0 = now_ns();
    while (completed < orders) {
        // Fill the window.
        while (sent < orders && pending_new.size() + pending_cancel.size() < window) {
            Command c = gen.next();
            c.id += id_offset;
            const std::uint64_t ts = now_ns();
            if (c.type == CommandType::New) pending_new[c.id] = ts;
            else pending_cancel[c.id] = ts;
            if (!send_all(fd, &c, sizeof c)) {
                std::fprintf(stderr, "send failed\n");
                return 1;
            }
            ++sent;
        }
        // Read responses.
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 5000) <= 0) {
            std::fprintf(stderr, "timeout waiting for server (%zu/%zu done)\n", completed, orders);
            return 1;
        }
        const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) {
            std::fprintf(stderr, "server closed connection\n");
            return 1;
        }
        rbuf.insert(rbuf.end(), tmp, tmp + n);
        std::size_t off = 0;
        const std::uint64_t now = now_ns();
        while (rbuf.size() - off >= sizeof(Event)) {
            Event e;
            std::memcpy(&e, rbuf.data() + off, sizeof e);
            off += sizeof e;
            if (e.type == EventType::Trade) ++trades;
            if (e.type == EventType::Rejected) ++rejects;

            if (auto it = pending_new.find(e.id);
                it != pending_new.end() &&
                (e.type == EventType::Accepted || e.type == EventType::Rejected)) {
                rtt.record(now - it->second);
                pending_new.erase(it);
                ++completed;
            } else if (auto jt = pending_cancel.find(e.id);
                       jt != pending_cancel.end() &&
                       ((e.type == EventType::Cancelled && e.reason == Reason::UserCancel) ||
                        e.type == EventType::Rejected)) {
                rtt.record(now - jt->second);
                pending_cancel.erase(jt);
                ++completed;
            }
        }
        rbuf.erase(rbuf.begin(), rbuf.begin() + static_cast<std::ptrdiff_t>(off));
    }
    const double secs = static_cast<double>(now_ns() - t0) / 1e9;

    std::printf("sent %zu commands in %.3f s  ->  %.0f commands/s  (window=%zu)\n", sent, secs,
                static_cast<double>(sent) / secs, window);
    std::printf("trades=%llu  rejects=%llu\n", static_cast<unsigned long long>(trades),
                static_cast<unsigned long long>(rejects));
    LatencyRecorder::print("round-trip latency", rtt.summarize());
    ::close(fd);
    return 0;
}
