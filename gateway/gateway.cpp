#include "gateway.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "orderflow/journal.hpp"
#include "orderflow/matching_engine.hpp"
#include "orderflow/spsc_ring.hpp"

namespace orderflow {

namespace {

using ConnId = std::uint32_t;

struct Inbound {
    ConnId conn;
    std::uint32_t pad;
    Command cmd;
};

struct Outbound {
    ConnId conn;
    std::uint32_t pad;
    Event ev;
};

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error(std::string("fcntl: ") + std::strerror(errno));
}

// Routes engine events to the right connection(s). Runs on the engine thread.
class RoutingSink final : public EventSink {
public:
    RoutingSink(SpscRing<Outbound>& out, std::unordered_map<OrderId, ConnId>& owners,
                std::vector<OrderId>& touched_makers, std::atomic<bool>& running)
        : out_(out), owners_(owners), touched_(touched_makers), running_(running) {}

    ConnId current_conn{0};
    std::uint64_t pushed{0};

    void on_event(const Event& e) override {
        push(current_conn, e);
        if (e.type == EventType::Trade) {
            // The resting side also needs to hear about the fill.
            auto it = owners_.find(e.maker_id);
            if (it != owners_.end() && it->second != current_conn) push(it->second, e);
            touched_.push_back(e.maker_id);
        }
    }

private:
    void push(ConnId c, const Event& e) {
        const Outbound msg{c, 0, e};
        // Back-pressure: if the network thread is behind, wait for it.
        while (!out_.try_push(msg)) {
            if (!running_.load(std::memory_order_relaxed)) return;
            std::this_thread::yield();
        }
        ++pushed;
    }

    SpscRing<Outbound>& out_;
    std::unordered_map<OrderId, ConnId>& owners_;
    std::vector<OrderId>& touched_;
    std::atomic<bool>& running_;
};

struct Connection {
    int fd{-1};
    std::vector<char> rbuf;
    std::vector<char> wbuf;
    std::size_t wpos{0};
    bool dead{false};
};

}  // namespace

struct Gateway::Impl {
    explicit Impl(const GatewayConfig& cfg)
        : inbound(cfg.ring_capacity), outbound(cfg.ring_capacity) {}

    SpscRing<Inbound> inbound;
    SpscRing<Outbound> outbound;
    int listen_fd{-1};
    int wake_pipe[2]{-1, -1};

    void network_loop(std::atomic<bool>& running, GatewayStats& stats);
    void engine_loop(std::atomic<bool>& running, GatewayStats& stats, const GatewayConfig& cfg);
};

Gateway::Gateway(GatewayConfig cfg) : impl_(std::make_unique<Impl>(cfg)), cfg_(std::move(cfg)) {}

Gateway::~Gateway() { stop(); }

void Gateway::start() {
    if (running_.exchange(true)) return;

    if (::pipe(impl_->wake_pipe) != 0) throw std::runtime_error("pipe() failed");
    set_nonblocking(impl_->wake_pipe[0]);
    set_nonblocking(impl_->wake_pipe[1]);

    impl_->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listen_fd < 0) throw std::runtime_error("socket() failed");
    int one = 1;
    ::setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg_.port);
    if (::bind(impl_->listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    if (::listen(impl_->listen_fd, 128) != 0) throw std::runtime_error("listen() failed");
    set_nonblocking(impl_->listen_fd);

    socklen_t len = sizeof addr;
    ::getsockname(impl_->listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    bound_port_ = ntohs(addr.sin_port);

    engine_thread_ = std::thread([this] { impl_->engine_loop(running_, stats_, cfg_); });
    network_thread_ = std::thread([this] { impl_->network_loop(running_, stats_); });
}

void Gateway::stop() {
    if (!running_.exchange(false)) return;
    if (impl_->wake_pipe[1] >= 0) {
        const char b = 1;
        [[maybe_unused]] auto n = ::write(impl_->wake_pipe[1], &b, 1);
    }
    if (engine_thread_.joinable()) engine_thread_.join();
    if (network_thread_.joinable()) network_thread_.join();
    if (impl_->listen_fd >= 0) ::close(impl_->listen_fd);
    for (int& fd : impl_->wake_pipe) {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    impl_->listen_fd = -1;
}

// ---------------------------------------------------------------------------
// Engine thread: the only thread that touches the MatchingEngine.
// ---------------------------------------------------------------------------
void Gateway::Impl::engine_loop(std::atomic<bool>& running, GatewayStats& stats,
                                const GatewayConfig& cfg) {
    std::unordered_map<OrderId, ConnId> owners;
    owners.reserve(cfg.max_orders);
    std::vector<OrderId> touched;
    touched.reserve(1024);

    RoutingSink sink(outbound, owners, touched, running);
    MatchingEngine engine(EngineConfig{cfg.num_symbols, cfg.max_orders}, sink);
    std::unique_ptr<JournalWriter> journal;
    if (!cfg.journal_path.empty()) journal = std::make_unique<JournalWriter>(cfg.journal_path);

    std::uint64_t idle = 0;
    Inbound msg{};
    while (running.load(std::memory_order_relaxed)) {
        bool did_work = false;
        while (inbound.try_pop(msg)) {
            did_work = true;
            const Command& cmd = msg.cmd;

            // Ownership check: a client may only cancel its own orders.
            if (cmd.type == CommandType::Cancel) {
                auto it = owners.find(cmd.id);
                if (it == owners.end() || it->second != msg.conn) {
                    Event rej;
                    rej.type = EventType::Rejected;
                    rej.reason = Reason::UnknownOrder;
                    rej.symbol = cmd.symbol;
                    rej.id = cmd.id;
                    sink.current_conn = msg.conn;
                    sink.on_event(rej);
                    stats.ownership_rejects.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
            }

            if (journal) journal->append(cmd);
            sink.current_conn = msg.conn;
            const bool is_new = cmd.type == CommandType::New;
            const bool already_owned = is_new && owners.count(cmd.id) != 0;
            if (is_new && !already_owned) owners.emplace(cmd.id, msg.conn);

            engine.process(cmd);

            // Forget owners of orders that are no longer on the book.
            if (!already_owned && !engine.is_live(cmd.id)) owners.erase(cmd.id);
            for (OrderId m : touched)
                if (!engine.is_live(m)) owners.erase(m);
            touched.clear();
        }
        if (did_work) {
            if (journal) journal->flush();
            stats.commands_in.store(engine.commands_processed(), std::memory_order_relaxed);
            stats.events_out.store(sink.pushed, std::memory_order_relaxed);
            const char b = 1;
            [[maybe_unused]] auto n = ::write(wake_pipe[1], &b, 1);  // wake network thread
            idle = 0;
        } else if (!cfg.busy_spin && ++idle > 10'000) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

// ---------------------------------------------------------------------------
// Network thread: accept, read Commands, write Events. Never touches the engine.
// ---------------------------------------------------------------------------
void Gateway::Impl::network_loop(std::atomic<bool>& running, GatewayStats& stats) {
    std::unordered_map<ConnId, Connection> conns;
    ConnId next_conn = 1;
    std::vector<pollfd> fds;
    std::vector<ConnId> fd_owner;
    char tmp[64 * 1024];

    while (running.load(std::memory_order_relaxed)) {
        fds.clear();
        fd_owner.clear();
        fds.push_back({listen_fd, POLLIN, 0});
        fds.push_back({wake_pipe[0], POLLIN, 0});
        for (auto& [id, c] : conns) {
            short ev = POLLIN;
            if (c.wpos < c.wbuf.size()) ev |= POLLOUT;
            fds.push_back({c.fd, ev, 0});
            fd_owner.push_back(id);
        }

        const int timeout_ms = outbound.size_approx() > 0 ? 0 : 100;
        if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms) < 0 && errno != EINTR)
            break;

        // 1. New connections.
        if (fds[0].revents & POLLIN) {
            for (;;) {
                const int cfd = ::accept(listen_fd, nullptr, nullptr);
                if (cfd < 0) break;
                set_nonblocking(cfd);
                int one = 1;
                ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // no Nagle delay
                Connection c;
                c.fd = cfd;
                conns.emplace(next_conn++, std::move(c));
                stats.connections.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // 2. Drain the wake-up pipe.
        if (fds[1].revents & POLLIN) {
            while (::read(wake_pipe[0], tmp, sizeof tmp) > 0) {
            }
        }
        // 3. Read Commands from clients.
        for (std::size_t i = 2; i < fds.size(); ++i) {
            if (!(fds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            const ConnId id = fd_owner[i - 2];
            Connection& c = conns[id];
            for (;;) {
                const ssize_t n = ::recv(c.fd, tmp, sizeof tmp, 0);
                if (n > 0) {
                    c.rbuf.insert(c.rbuf.end(), tmp, tmp + n);
                    continue;
                }
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) c.dead = true;
                break;
            }
            // Frame complete 32-byte Commands; keep any partial tail for next time.
            std::size_t off = 0;
            while (c.rbuf.size() - off >= sizeof(Command)) {
                Inbound in{id, 0, {}};
                std::memcpy(&in.cmd, c.rbuf.data() + off, sizeof(Command));
                off += sizeof(Command);
                while (!inbound.try_push(in)) {
                    if (!running.load(std::memory_order_relaxed)) return;
                    std::this_thread::yield();
                }
            }
            c.rbuf.erase(c.rbuf.begin(), c.rbuf.begin() + static_cast<std::ptrdiff_t>(off));
        }
        // 4. Move Events from the engine into per-connection write buffers.
        Outbound out{};
        while (outbound.try_pop(out)) {
            auto it = conns.find(out.conn);
            if (it == conns.end()) continue;  // client already disconnected
            const char* p = reinterpret_cast<const char*>(&out.ev);
            it->second.wbuf.insert(it->second.wbuf.end(), p, p + sizeof(Event));
        }
        // 5. Flush write buffers.
        for (auto& [id, c] : conns) {
            while (c.wpos < c.wbuf.size()) {
                const ssize_t n = ::send(c.fd, c.wbuf.data() + c.wpos, c.wbuf.size() - c.wpos,
                                         MSG_NOSIGNAL);
                if (n > 0) {
                    c.wpos += static_cast<std::size_t>(n);
                    continue;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK) c.dead = true;
                break;
            }
            if (c.wpos == c.wbuf.size()) {
                c.wbuf.clear();
                c.wpos = 0;
            }
        }
        // 6. Reap closed connections. (Their resting orders stay on the book.)
        for (auto it = conns.begin(); it != conns.end();) {
            if (it->second.dead) {
                ::close(it->second.fd);
                it = conns.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& [id, c] : conns) ::close(c.fd);
}

}  // namespace orderflow
