// End-to-end tests: real TCP sockets against a running Gateway.
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "gateway.hpp"

using namespace orderflow;

namespace {

class TcpClient {
public:
    explicit TcpClient(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    }
    ~TcpClient() { ::close(fd_); }

    bool connected() const { return connected_; }

    void send(const Command& c) { ASSERT_EQ(::send(fd_, &c, sizeof c, 0), static_cast<ssize_t>(sizeof c)); }

    // Reads exactly `n` events (or fewer on timeout).
    std::vector<Event> read(std::size_t n, int timeout_ms = 2000) {
        std::vector<Event> out;
        while (out.size() < n) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, timeout_ms) <= 0) break;
            char tmp[4096];
            const ssize_t got = ::recv(fd_, tmp, sizeof tmp, 0);
            if (got <= 0) break;
            buf_.insert(buf_.end(), tmp, tmp + got);
            while (buf_.size() >= sizeof(Event)) {
                Event e;
                std::memcpy(&e, buf_.data(), sizeof e);
                buf_.erase(buf_.begin(), buf_.begin() + sizeof(Event));
                out.push_back(e);
            }
        }
        return out;
    }

private:
    int fd_{-1};
    bool connected_{false};
    std::vector<char> buf_;
};

class GatewayTest : public ::testing::Test {
protected:
    void SetUp() override {
        GatewayConfig cfg;
        cfg.port = 0;  // ephemeral port: tests can run in parallel
        cfg.num_symbols = 2;
        cfg.max_orders = 1024;
        cfg.ring_capacity = 1024;
        gw_ = std::make_unique<Gateway>(cfg);
        gw_->start();
    }
    void TearDown() override { gw_->stop(); }
    std::unique_ptr<Gateway> gw_;
};

}  // namespace

TEST_F(GatewayTest, BindsToEphemeralPort) { EXPECT_GT(gw_->port(), 0); }

TEST_F(GatewayTest, NewOrderIsAcknowledged) {
    TcpClient c(gw_->port());
    ASSERT_TRUE(c.connected());
    c.send(Command::limit(0, 1, Side::Buy, 100, 10));
    const auto ev = c.read(1);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(ev[0].id, 1u);
}

TEST_F(GatewayTest, BothCounterpartiesReceiveTheTrade) {
    TcpClient maker(gw_->port()), taker(gw_->port());
    maker.send(Command::limit(0, 1, Side::Sell, 100, 5));
    ASSERT_EQ(maker.read(1).size(), 1u);  // Accepted

    taker.send(Command::limit(0, 2, Side::Buy, 100, 5));
    const auto t = taker.read(2);  // Accepted + Trade
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[1].type, EventType::Trade);
    EXPECT_EQ(t[1].maker_id, 1u);

    const auto m = maker.read(1);  // Trade
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].type, EventType::Trade);
    EXPECT_EQ(m[0].qty, 5u);
}

TEST_F(GatewayTest, ClientCannotCancelSomeoneElsesOrder) {
    TcpClient owner(gw_->port()), attacker(gw_->port());
    owner.send(Command::limit(0, 7, Side::Buy, 100, 5));
    ASSERT_EQ(owner.read(1).size(), 1u);

    attacker.send(Command::cancel(0, 7));
    const auto ev = attacker.read(1);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Rejected);
    EXPECT_EQ(ev[0].reason, Reason::UnknownOrder);

    owner.send(Command::cancel(0, 7));  // the real owner can
    const auto ok = owner.read(1);
    ASSERT_EQ(ok.size(), 1u);
    EXPECT_EQ(ok[0].type, EventType::Cancelled);
}

TEST_F(GatewayTest, HandlesCommandsSplitAcrossTcpWrites) {
    const auto cmd = Command::limit(1, 3, Side::Sell, 200, 1);
    const char* p = reinterpret_cast<const char*>(&cmd);
    // Send the 32-byte frame in two pieces; the gateway must reassemble it.
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(gw_->port());
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    ASSERT_EQ(::send(fd, p, 10, 0), 10);
    usleep(20'000);
    ASSERT_EQ(::send(fd, p + 10, sizeof cmd - 10, 0), static_cast<ssize_t>(sizeof cmd - 10));
    pollfd pf{fd, POLLIN, 0};
    ASSERT_GT(::poll(&pf, 1, 2000), 0);
    Event e;
    ASSERT_EQ(::recv(fd, &e, sizeof e, MSG_WAITALL), static_cast<ssize_t>(sizeof e));
    EXPECT_EQ(e.type, EventType::Accepted);
    EXPECT_EQ(e.id, 3u);
    ::close(fd);
}

TEST_F(GatewayTest, ManyOrdersPipelined) {
    TcpClient c(gw_->port());
    constexpr int kN = 500;
    for (int i = 1; i <= kN; ++i) c.send(Command::limit(0, static_cast<OrderId>(i), Side::Buy, 100, 1));
    const auto ev = c.read(kN);
    ASSERT_EQ(ev.size(), static_cast<std::size_t>(kN));
    for (int i = 0; i < kN; ++i) EXPECT_EQ(ev[static_cast<std::size_t>(i)].id, static_cast<OrderId>(i + 1));
}
