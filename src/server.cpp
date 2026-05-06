#include "netforge/server.hpp"

#include <cstring>
#include <thread>

namespace netforge {

static constexpr size_t kMaxWriteQueueSize = 256;

Server::Server()
    : raw_queue_(std::make_unique<SpscQueue<RawEvent, kQueueCapacity>>()),
      incoming_queue_(std::make_unique<SpscQueue<IncomingEvent, kQueueCapacity>>()),
      outgoing_queue_(std::make_unique<SpscQueue<OutgoingCommand, kQueueCapacity>>()) {}

Server::~Server() {
    stop();
}

void Server::on_connect(ConnectCallback cb) { on_connect_cb_ = std::move(cb); }
void Server::on_disconnect(DisconnectCallback cb) { on_disconnect_cb_ = std::move(cb); }
void Server::on_message(MessageCallback cb) { on_message_cb_ = std::move(cb); }

bool Server::start(const ServerConfig& config) {
    if (running_.load(std::memory_order_acquire)) return false;

    config_ = config;

    if (!net_init()) return false;

    listen_socket_ = create_tcp_socket();
    if (listen_socket_ == kInvalidSocket) return false;

    set_reuseaddr(listen_socket_);
    set_nonblocking(listen_socket_);

    if (!bind_and_listen(listen_socket_, config_.port)) {
        close_socket(listen_socket_);
        listen_socket_ = kInvalidSocket;
        return false;
    }

    event_loop_ = std::make_unique<EventLoop>();
    buffer_pool_ = std::make_unique<BufferPool>(config_.buffer_pool_size);
    event_loop_->add(listen_socket_, nullptr);

    running_.store(true, std::memory_order_release);
    return true;
}

bool Server::start_threaded(const ServerConfig& config) {
    if (!start(config)) return false;

    io_thread_ = std::thread([this]() {
        while (running_.load(std::memory_order_acquire))
            tick_io(1);
    });

    proc_thread_ = std::thread([this]() {
        while (running_.load(std::memory_order_acquire))
            tick_process();
    });

    return true;
}

void Server::stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    running_.store(false, std::memory_order_release);

    if (io_thread_.joinable()) io_thread_.join();
    if (proc_thread_.joinable()) proc_thread_.join();

    for (auto& [id, conn] : connections_)
        close_socket(conn->socket);
    connections_.clear();
    read_buffers_.clear();

    if (listen_socket_ != kInvalidSocket) {
        close_socket(listen_socket_);
        listen_socket_ = kInvalidSocket;
    }

    event_loop_.reset();
    buffer_pool_.reset();
}

void Server::send(ConnectionId id, const Message& msg) {
    OutgoingCommand cmd;
    cmd.type = OutgoingCommand::Send;
    cmd.conn_id = id;
    cmd.data = msg.serialize();
    outgoing_queue_->push(std::move(cmd));
}

void Server::broadcast(const Message& msg) {
    OutgoingCommand cmd;
    cmd.type = OutgoingCommand::Broadcast;
    cmd.conn_id = 0;
    cmd.data = msg.serialize();
    outgoing_queue_->push(std::move(cmd));
}

// ==========================================================================
// Game thread
// ==========================================================================

void Server::poll() {
    while (auto event = incoming_queue_->pop()) {
        switch (event->type) {
            case IncomingEvent::Connect:
                if (on_connect_cb_) on_connect_cb_(event->conn_id);
                break;
            case IncomingEvent::Disconnect:
                if (on_disconnect_cb_) on_disconnect_cb_(event->conn_id);
                break;
            case IncomingEvent::Data:
                if (on_message_cb_) on_message_cb_(event->conn_id, std::move(event->message));
                break;
        }
    }
}

// ==========================================================================
// I/O thread tick
// ==========================================================================

void Server::tick_io(int timeout_ms) {
    drain_outgoing();

    std::vector<EventLoop::Event> events;
    events.reserve(256);

    int n = event_loop_->poll(events, timeout_ms);

    for (int i = 0; i < n; ++i) {
        auto& ev = events[i];

        if (ev.user_data == nullptr) {
            handle_accept();
            continue;
        }

        auto* conn = static_cast<Connection*>(ev.user_data);

        if (ev.error) {
            disconnect(*conn);
            continue;
        }

        if (ev.readable)
            handle_read(*conn);

        if (conn->state != ConnectionState::Connected) continue;

        if (ev.writable)
            handle_write(*conn);
    }

    handle_accept();

    for (auto it = connections_.begin(); it != connections_.end(); ) {
        if (it->second->state == ConnectionState::Closed)
            it = connections_.erase(it);
        else
            ++it;
    }
}

void Server::handle_accept() {
    for (int i = 0; i < 32; ++i) {
        if (connections_.size() >= config_.max_connections) break;

        socket_t client_sock = accept_connection(listen_socket_);
        if (client_sock == kInvalidSocket) break;

        auto conn = std::make_unique<Connection>();
        conn->id = next_id_.fetch_add(1, std::memory_order_relaxed);
        conn->socket = client_sock;
        conn->state = ConnectionState::Connected;

        Connection* raw = conn.get();
        ConnectionId id = conn->id;

        if (!event_loop_->add(client_sock, raw)) {
            close_socket(client_sock);
            continue;
        }

        RawEvent ev;
        ev.type = RawEvent::Connect;
        ev.conn_id = id;

        if (!raw_queue_->push(std::move(ev))) {
            // Queue full — reject this connection
            event_loop_->remove(client_sock);
            close_socket(client_sock);
            continue;
        }

        connections_[id] = std::move(conn);
    }
}

void Server::handle_read(Connection& conn) {
    uint8_t tmp[4096];

    while (true) {
        int bytes = socket_recv(conn.socket, tmp, sizeof(tmp));

        if (bytes > 0) {
            RawEvent ev;
            ev.type = RawEvent::Data;
            ev.conn_id = conn.id;
            ev.data.assign(tmp, tmp + bytes);
            raw_queue_->push(std::move(ev));
        } else if (bytes == 0) {
            disconnect(conn);
            return;
        } else {
            if (!would_block())
                disconnect(conn);
            return;
        }
    }
}

void Server::handle_write(Connection& conn) {
    while (!conn.write_queue.empty()) {
        auto& entry = conn.write_queue.front();
        auto& buf = *entry.data;
        size_t remaining = buf.size() - entry.offset;

        int sent = socket_send(conn.socket, buf.data() + entry.offset, remaining);

        if (sent > 0) {
            entry.offset += sent;
            if (entry.offset >= buf.size())
                conn.write_queue.pop_front();
        } else {
            if (!would_block())
                disconnect(conn);
            return;
        }
    }

    if (conn.write_queue.empty())
        event_loop_->set_writable(conn.socket, false, &conn);
}

void Server::disconnect(Connection& conn) {
    if (conn.state == ConnectionState::Closed) return;

    conn.state = ConnectionState::Closed;
    event_loop_->remove(conn.socket);
    close_socket(conn.socket);
    conn.socket = kInvalidSocket;

    RawEvent ev;
    ev.type = RawEvent::Disconnect;
    ev.conn_id = conn.id;
    raw_queue_->push(std::move(ev));
}

void Server::drain_outgoing() {
    while (auto cmd = outgoing_queue_->pop()) {
        switch (cmd->type) {
            case OutgoingCommand::Send: {
                auto it = connections_.find(cmd->conn_id);
                if (it != connections_.end() && it->second->state == ConnectionState::Connected) {
                    auto& conn = *it->second;
                    bool was_empty = conn.write_queue.empty();
                    conn.enqueue(std::move(cmd->data));
                    if (was_empty) {
                        handle_write(conn);
                        if (conn.has_pending_writes())
                            event_loop_->set_writable(conn.socket, true, &conn);
                    }
                }
                break;
            }
            case OutgoingCommand::Broadcast: {
                auto shared = std::make_shared<std::vector<uint8_t>>(std::move(cmd->data));
                for (auto& [id, conn_ptr] : connections_) {
                    if (conn_ptr->state == ConnectionState::Connected) {
                        auto& conn = *conn_ptr;
                        if (conn.write_queue.size() >= kMaxWriteQueueSize) continue;

                        bool was_empty = conn.write_queue.empty();
                        conn.enqueue(shared);
                        if (was_empty) {
                            handle_write(conn);
                            if (conn.has_pending_writes())
                                event_loop_->set_writable(conn.socket, true, &conn);
                        }
                    }
                }
                break;
            }
            case OutgoingCommand::Disconnect: {
                auto it = connections_.find(cmd->conn_id);
                if (it != connections_.end())
                    disconnect(*it->second);
                break;
            }
        }
    }
}

// ==========================================================================
// Processing thread tick
// ==========================================================================

void Server::tick_process() {
    bool did_work = false;

    while (auto raw = raw_queue_->pop()) {
        did_work = true;

        switch (raw->type) {
            case RawEvent::Connect: {
                read_buffers_[raw->conn_id] = ReadBuffer{};
                IncomingEvent ev;
                ev.type = IncomingEvent::Connect;
                ev.conn_id = raw->conn_id;
                incoming_queue_->push(std::move(ev));
                break;
            }
            case RawEvent::Disconnect: {
                flush_connection_buffer(raw->conn_id);
                read_buffers_.erase(raw->conn_id);
                IncomingEvent ev;
                ev.type = IncomingEvent::Disconnect;
                ev.conn_id = raw->conn_id;
                incoming_queue_->push(std::move(ev));
                break;
            }
            case RawEvent::Data: {
                process_raw_data(raw->conn_id, raw->data.data(), raw->data.size());
                break;
            }
        }
    }

    if (!did_work)
        std::this_thread::sleep_for(std::chrono::microseconds(100));
}

void Server::process_raw_data(ConnectionId id, const uint8_t* data, size_t len) {
    auto it = read_buffers_.find(id);
    if (it == read_buffers_.end()) return;

    auto& rb = it->second;

    // Guard against overflow before arithmetic
    if (len > kMaxPayloadSize + kMessageHeaderSize) return;
    if (rb.pos > kMaxPayloadSize + kMessageHeaderSize) return;

    if (rb.pos + len > rb.buf.size()) {
        size_t needed = rb.pos + len;
        if (needed > kMaxPayloadSize + kMessageHeaderSize)
            return;
        rb.buf.resize(needed + kBufferSize);
    }

    std::memcpy(rb.buf.data() + rb.pos, data, len);
    rb.pos += len;

    size_t offset = 0;
    while (offset + kMessageHeaderSize <= rb.pos) {
        auto header = MessageHeader::decode(rb.buf.data() + offset);
        size_t total_size = kMessageHeaderSize + header.size;

        if (offset + total_size > rb.pos)
            break;

        Message msg;
        Message::deserialize(rb.buf.data() + offset, total_size, msg);

        IncomingEvent ev;
        ev.type = IncomingEvent::Data;
        ev.conn_id = id;
        ev.message = std::move(msg);

        if (!incoming_queue_->push(std::move(ev)))
            break;

        offset += total_size;
    }

    if (offset > 0) {
        size_t remaining = rb.pos - offset;
        if (remaining > 0)
            std::memmove(rb.buf.data(), rb.buf.data() + offset, remaining);
        rb.pos = remaining;
    }
}

void Server::flush_connection_buffer(ConnectionId id) {
    auto it = read_buffers_.find(id);
    if (it == read_buffers_.end()) return;

    auto& rb = it->second;
    size_t offset = 0;

    while (offset + kMessageHeaderSize <= rb.pos) {
        auto header = MessageHeader::decode(rb.buf.data() + offset);
        size_t total_size = kMessageHeaderSize + header.size;

        if (offset + total_size > rb.pos) break;

        Message msg;
        Message::deserialize(rb.buf.data() + offset, total_size, msg);

        IncomingEvent ev;
        ev.type = IncomingEvent::Data;
        ev.conn_id = id;
        ev.message = std::move(msg);
        incoming_queue_->push(std::move(ev));

        offset += total_size;
    }
}

} // namespace netforge
