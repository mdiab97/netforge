#include "netforge/client.hpp"

#include <cstring>
#include <chrono>
#include <thread>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
#endif

namespace netforge {

Client::Client()
    : incoming_queue_(std::make_unique<SpscQueue<IncomingEvent, 4096>>()),
      outgoing_queue_(std::make_unique<SpscQueue<std::vector<uint8_t>, 4096>>()) {}

Client::~Client() {
    disconnect();
}

void Client::on_connect(ConnectCallback cb) { on_connect_cb_ = std::move(cb); }
void Client::on_disconnect(DisconnectCallback cb) { on_disconnect_cb_ = std::move(cb); }
void Client::on_message(MessageCallback cb) { on_message_cb_ = std::move(cb); }

bool Client::connect(const ClientConfig& config) {
    if (running_.load()) return false;

    config_ = config;

    if (!net_init()) return false;

    conn_.reset();
    conn_.socket = create_tcp_socket();
    if (conn_.socket == kInvalidSocket) return false;

    if (!connect_nonblocking(conn_.socket, config_.host.c_str(), config_.port)) {
        close_socket(conn_.socket);
        conn_.socket = kInvalidSocket;
        return false;
    }

    running_.store(true, std::memory_order_release);
    io_thread_ = std::thread(&Client::io_thread_func, this);

    return true;
}

void Client::disconnect() {
    if (!running_.load()) return;

    running_.store(false, std::memory_order_release);

    if (io_thread_.joinable()) {
        io_thread_.join();
    }

    if (conn_.socket != kInvalidSocket) {
        close_socket(conn_.socket);
        conn_.socket = kInvalidSocket;
    }

    connected_.store(false, std::memory_order_relaxed);
}

void Client::send(const Message& msg) {
    auto data = msg.serialize();
    outgoing_queue_->push(std::move(data));
}

void Client::poll() {
    while (auto event = incoming_queue_->pop()) {
        switch (event->type) {
            case IncomingEvent::Connected:
                if (on_connect_cb_) on_connect_cb_();
                break;
            case IncomingEvent::Disconnected:
                if (on_disconnect_cb_) on_disconnect_cb_();
                break;
            case IncomingEvent::Data:
                if (on_message_cb_) on_message_cb_(std::move(event->message));
                break;
        }
    }
}

void Client::io_thread_func() {
    // Wait for connection to complete using select
    {
        fd_set write_set, except_set;
        FD_ZERO(&write_set);
        FD_ZERO(&except_set);
        FD_SET(conn_.socket, &write_set);
        FD_SET(conn_.socket, &except_set);

        timeval tv{};
        tv.tv_sec = 5; // 5 second connect timeout

#ifdef _WIN32
        int result = ::select(0, nullptr, &write_set, &except_set, &tv);
#else
        int result = ::select(conn_.socket + 1, nullptr, &write_set, &except_set, &tv);
#endif

        if (result <= 0 || FD_ISSET(conn_.socket, &except_set)) {
            IncomingEvent ev;
            ev.type = IncomingEvent::Disconnected;
            incoming_queue_->push(std::move(ev));
            running_.store(false, std::memory_order_relaxed);
            return;
        }
    }

    conn_.state = ConnectionState::Connected;
    connected_.store(true, std::memory_order_release);

    {
        IncomingEvent ev;
        ev.type = IncomingEvent::Connected;
        incoming_queue_->push(std::move(ev));
    }

    // Main I/O loop
    while (running_.load(std::memory_order_relaxed)) {
        // Send any queued outgoing data
        while (auto data = outgoing_queue_->pop()) {
            size_t offset = 0;
            while (offset < data->size()) {
                int sent = socket_send(conn_.socket, data->data() + offset,
                                       data->size() - offset);
                if (sent > 0) {
                    offset += sent;
                } else if (sent < 0 && would_block()) {
                    // Yield and retry
                    std::this_thread::yield();
                } else {
                    // Error
                    goto disconnected;
                }
            }
        }

        // Try to receive data
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(conn_.socket, &read_set);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 0; // non-blocking poll for minimal latency

#ifdef _WIN32
        int ready = ::select(0, &read_set, nullptr, nullptr, &tv);
#else
        int ready = ::select(conn_.socket + 1, &read_set, nullptr, nullptr, &tv);
#endif

        if (ready > 0 && FD_ISSET(conn_.socket, &read_set)) {
            while (true) {
                if (conn_.read_pos >= conn_.read_buf.size()) {
                    conn_.read_buf.resize(conn_.read_buf.size() + kBufferSize);
                }

                size_t space = conn_.read_buf.size() - conn_.read_pos;
                int bytes = socket_recv(conn_.socket, conn_.read_buf.data() + conn_.read_pos,
                                        space);

                if (bytes > 0) {
                    conn_.read_pos += bytes;
                } else if (bytes == 0) {
                    goto disconnected;
                } else {
                    if (!would_block()) {
                        goto disconnected;
                    }
                    break;
                }
            }

            // Parse messages from read buffer
            size_t offset = 0;
            while (offset + kMessageHeaderSize <= conn_.read_pos) {
                auto header = MessageHeader::decode(conn_.read_buf.data() + offset);
                size_t total_size = kMessageHeaderSize + header.size;

                if (offset + total_size > conn_.read_pos) {
                    break;
                }

                Message msg;
                Message::deserialize(conn_.read_buf.data() + offset, total_size, msg);

                IncomingEvent ev;
                ev.type = IncomingEvent::Data;
                ev.message = std::move(msg);
                incoming_queue_->push(std::move(ev));

                offset += total_size;
            }

            // Compact buffer
            if (offset > 0) {
                size_t remaining = conn_.read_pos - offset;
                if (remaining > 0) {
                    std::memmove(conn_.read_buf.data(), conn_.read_buf.data() + offset, remaining);
                }
                conn_.read_pos = remaining;
            }
        }
    }

    return;

disconnected:
    connected_.store(false, std::memory_order_relaxed);
    IncomingEvent ev;
    ev.type = IncomingEvent::Disconnected;
    incoming_queue_->push(std::move(ev));
    running_.store(false, std::memory_order_relaxed);
}

} // namespace netforge
