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
    : incoming_queue_(std::make_unique<SpscQueue<IncomingEvent, kQueueCapacity>>()),
      outgoing_queue_(std::make_unique<SpscQueue<std::vector<uint8_t>, kQueueCapacity>>()) {}

Client::~Client() {
    disconnect();
}

void Client::on_connect(ConnectCallback cb) { on_connect_cb_ = std::move(cb); }
void Client::on_disconnect(DisconnectCallback cb) { on_disconnect_cb_ = std::move(cb); }
void Client::on_message(MessageCallback cb) { on_message_cb_ = std::move(cb); }

bool Client::connect(const ClientConfig& config) {
    if (running_.load(std::memory_order_acquire)) return false;

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
    if (!running_.load(std::memory_order_acquire)) return;

    running_.store(false, std::memory_order_release);

    if (io_thread_.joinable()) {
        io_thread_.join();
    }

    if (conn_.socket != kInvalidSocket) {
        close_socket(conn_.socket);
        conn_.socket = kInvalidSocket;
    }

    write_queue_.clear();
    connected_.store(false, std::memory_order_release);
}

bool Client::send(const Message& msg) {
    auto data = msg.serialize();
    return outgoing_queue_->push(std::move(data));
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
    // Wait for connection to complete (5 second timeout)
    {
#ifdef _WIN32
        WSAPOLLFD pfd{};
        pfd.fd = conn_.socket;
        pfd.events = POLLWRNORM;
        int result = WSAPoll(&pfd, 1, 5000);
        if (result <= 0 || (pfd.revents & POLLERR)) {
            push_disconnect();
            return;
        }
#else
        fd_set write_set, except_set;
        FD_ZERO(&write_set);
        FD_ZERO(&except_set);
        FD_SET(conn_.socket, &write_set);
        FD_SET(conn_.socket, &except_set);
        timeval tv{5, 0};
        int result = ::select(static_cast<int>(conn_.socket) + 1, nullptr, &write_set, &except_set, &tv);
        if (result <= 0 || FD_ISSET(conn_.socket, &except_set)) {
            push_disconnect();
            return;
        }
#endif
    }

    conn_.state = ConnectionState::Connected;
    connected_.store(true, std::memory_order_release);

    {
        IncomingEvent ev;
        ev.type = IncomingEvent::Connected;
        incoming_queue_->push(std::move(ev));
    }

    // Main I/O loop
    while (running_.load(std::memory_order_acquire)) {
        // Move queued data from game thread into local write buffer
        while (auto data = outgoing_queue_->pop()) {
            write_queue_.push_back({std::make_shared<std::vector<uint8_t>>(std::move(*data)), 0});
        }

        bool want_write = !write_queue_.empty();

#ifdef _WIN32
        WSAPOLLFD pfd{};
        pfd.fd = conn_.socket;
        pfd.events = POLLRDNORM;
        if (want_write) pfd.events |= POLLWRNORM;

        int ready = WSAPoll(&pfd, 1, 1);

        if (ready < 0) {
            push_disconnect();
            return;
        }

        bool can_read = ready > 0 && (pfd.revents & (POLLRDNORM | POLLHUP));
        bool can_write = ready > 0 && (pfd.revents & POLLWRNORM);
        bool has_error = ready > 0 && (pfd.revents & POLLERR);
#else
        fd_set read_set, write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(conn_.socket, &read_set);
        if (want_write) FD_SET(conn_.socket, &write_set);

        timeval tv{0, 500};
        int ready = ::select(static_cast<int>(conn_.socket) + 1, &read_set, &write_set, nullptr, &tv);

        if (ready < 0) {
            push_disconnect();
            return;
        }

        bool can_read = ready > 0 && FD_ISSET(conn_.socket, &read_set);
        bool can_write = ready > 0 && FD_ISSET(conn_.socket, &write_set);
        bool has_error = false;
#endif

        if (has_error) {
            push_disconnect();
            return;
        }

        if (can_write) {
            flush_outgoing();
        }

        if (can_read) {
            if (!read_incoming()) {
                push_disconnect();
                return;
            }
            parse_incoming();
        }
    }
}

void Client::flush_outgoing() {
    while (!write_queue_.empty()) {
        auto& entry = write_queue_.front();
        auto& buf = *entry.data;
        size_t remaining = buf.size() - entry.offset;

        int sent = socket_send(conn_.socket, buf.data() + entry.offset, remaining);

        if (sent > 0) {
            entry.offset += sent;
            if (entry.offset >= buf.size()) {
                write_queue_.pop_front();
            }
        } else {
            if (!would_block()) {
                push_disconnect();
            }
            return; // socket full or error, try again next poll
        }
    }
}

bool Client::read_incoming() {
    while (true) {
        if (conn_.read_pos >= conn_.read_buf.size()) {
            // Cap buffer growth to prevent memory exhaustion
            if (conn_.read_buf.size() >= kMaxPayloadSize + kMessageHeaderSize) {
                return false;
            }
            conn_.read_buf.resize(conn_.read_buf.size() + kBufferSize);
        }

        size_t space = conn_.read_buf.size() - conn_.read_pos;
        int bytes = socket_recv(conn_.socket, conn_.read_buf.data() + conn_.read_pos, space);

        if (bytes > 0) {
            conn_.read_pos += bytes;
        } else if (bytes == 0) {
            return false;
        } else {
            if (!would_block()) return false;
            break;
        }
    }
    return true;
}

void Client::parse_incoming() {
    size_t offset = 0;
    while (offset + kMessageHeaderSize <= conn_.read_pos) {
        auto header = MessageHeader::decode(conn_.read_buf.data() + offset);
        size_t total_size = kMessageHeaderSize + header.size;

        if (offset + total_size > conn_.read_pos) break;

        Message msg;
        Message::deserialize(conn_.read_buf.data() + offset, total_size, msg);

        IncomingEvent ev;
        ev.type = IncomingEvent::Data;
        ev.message = std::move(msg);

        if (!incoming_queue_->push(std::move(ev))) {
            // Queue full — stop parsing, leave data in buffer for next poll
            break;
        }

        offset += total_size;
    }

    if (offset > 0) {
        size_t remaining = conn_.read_pos - offset;
        if (remaining > 0) {
            std::memmove(conn_.read_buf.data(), conn_.read_buf.data() + offset, remaining);
        }
        conn_.read_pos = remaining;
    }
}

void Client::push_disconnect() {
    connected_.store(false, std::memory_order_release);
    IncomingEvent ev;
    ev.type = IncomingEvent::Disconnected;
    incoming_queue_->push(std::move(ev));
    running_.store(false, std::memory_order_release);
}

} // namespace netforge
