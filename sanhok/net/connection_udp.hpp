#pragma once

#include <boost/asio.hpp>
#include <boost/core/noncopyable.hpp>
#include <flatbuffers/detached_buffer.h>
#include <sanhok/container/concurrent_queue.hpp>
#include <spdlog/spdlog.h>

namespace sanhok::net {
using boost::asio::ip::udp;

class ConnectionUDP : boost::noncopyable {
public:
    ConnectionUDP(boost::asio::io_context& ctx, const udp::endpoint& local_endpoint,
                  std::function<void(std::vector<uint8_t>&&)>&& on_packet, size_t receive_buffer_size);
    ~ConnectionUDP();

    void connect(const udp::endpoint& remote_endpoint);
    void open();
    void close();
    void send_packet(std::shared_ptr<flatbuffers::DetachedBuffer> packet);
    void join_multicast_group(const boost::asio::ip::address &address);
    void leave_multicast_group(const boost::asio::ip::address &address);

    bool is_open() const { return is_open_; }
    udp::endpoint local_endpoint() const { return socket_.local_endpoint(); }
    udp::endpoint remote_endpoint() const { return socket_.remote_endpoint(); }

private:
    boost::asio::awaitable<void> receive_packet();
    const std::function<void(std::vector<uint8_t>&&)> on_packet_;

    boost::asio::io_context& ctx_;
    udp::socket socket_;
    std::atomic<bool> is_open_ {false};

    const size_t receive_buffer_size_;
    ConcurrentQueue<std::vector<uint8_t>> receive_queue_;
    std::thread worker_;
};

inline ConnectionUDP::ConnectionUDP(boost::asio::io_context& ctx,
                                    const udp::endpoint& local_endpoint, std::function<void(std::vector<uint8_t>&&)>&& on_packet,
                                    const size_t receive_buffer_size = 65536)
    : on_packet_(std::move(on_packet)), ctx_(ctx),
    socket_(ctx_, local_endpoint), receive_buffer_size_(receive_buffer_size) {
    spdlog::info("[ConnectionUDP] Bound on {}:{}",
        socket_.local_endpoint().address().to_string(), socket_.local_endpoint().port());
}

inline ConnectionUDP::~ConnectionUDP() {
    close();
    if (worker_.joinable()) worker_.join();
}

inline void ConnectionUDP::connect(const udp::endpoint& remote_endpoint) {
    try {
        socket_.connect(remote_endpoint);
    } catch (const boost::system::system_error& e) {
        spdlog::error("[ConnectionUDP] Error connecting socket: {}", e.what());
    }
    spdlog::info("[ConnectionUDP] Connected to {}:{}",
        socket_.remote_endpoint().address().to_string(), socket_.remote_endpoint().port());
}

inline void ConnectionUDP::open() {
    if (is_open_.exchange(true)) return;

    // Start receiving packets
    co_spawn(ctx_, [this]()->boost::asio::awaitable<void> {
        while (is_open_) {
            co_await receive_packet();
        }
    }, boost::asio::detached);

    // Start handling packets
    worker_ = std::thread([this] {
        while (is_open_) {
            auto packet = receive_queue_.pop_wait();
            if (!packet) return;
            on_packet_(std::move(*packet));
        }
    });
    worker_.detach();
}

inline void ConnectionUDP::close() {
    if (!is_open_.exchange(false)) return;
    spdlog::info("[ConnectionUDP] Close on {}:{}", local_endpoint().address().to_string(), local_endpoint().port());

    receive_queue_.clear();

    try {
        socket_.close();
    } catch (const boost::system::system_error& e) {
        spdlog::error("[ConnectionUDP] Error closing socket: {}", e.what());
    }
}

inline void ConnectionUDP::send_packet(std::shared_ptr<flatbuffers::DetachedBuffer> packet) {
  if (!is_open_)
    return;
  co_spawn(
      ctx_,
      [this, packet = std::move(packet)]() -> boost::asio::awaitable<void> {
        const auto [ec, _bytes] = co_await socket_.async_send(
            boost::asio::buffer(packet->data(), packet->size()),
            as_tuple(boost::asio::use_awaitable));
        if (ec) {
          spdlog::error("[ConnectionUDP] Error sending packet: {}", ec.what());
          close();
          co_return;
        }
        // spdlog::debug("[PeerUDP] {} bytes of packet sent", _bytes);
      },
      boost::asio::detached);
}

inline void ConnectionUDP::join_multicast_group(const boost::asio::ip::address &address) {
    if (boost::system::error_code ec;
        socket_.set_option(udp::socket::reuse_address(true), ec) ||
        socket_.set_option(boost::asio::ip::multicast::join_group(address), ec)) {
        spdlog::error("[ConnectionUDP] Error joining multicast group: {}", ec.what());
    }
}

inline void ConnectionUDP::leave_multicast_group(const boost::asio::ip::address &address) {
    if (boost::system::error_code ec;
        socket_.set_option(boost::asio::ip::multicast::leave_group(address), ec)) {
        spdlog::error("[ConnectionUDP] Error leaving multicast group: {}", ec.what());
    }
}

inline boost::asio::awaitable<void> ConnectionUDP::receive_packet() {
    if (!is_open_) co_return;

    std::vector<uint8_t> buffer(receive_buffer_size_);
    const auto [ec, _bytes] = co_await socket_.async_receive(
        boost::asio::buffer(buffer.data(), buffer.size()), as_tuple(boost::asio::use_awaitable));
    if (ec) {
        spdlog::error("[ConnectionUDP] Error receiving packet: {}", ec.what());
        close();
        co_return;
    }
    // spdlog::debug("[PeerUDP] {} bytes of packet received", _bytes);

    receive_queue_.push(std::move(buffer));
}
}
