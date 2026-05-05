#include "netforge/server.hpp"

#include <cstring>
#include <thread>

namespace netforge {

// Maximum write queue size per connection before we consider it a slow client
static constexpr size_t kMaxWriteQueueSize = 256;

Server::Server()
    : incoming_queue_(std::make_unique<SpscQueue<IncomingEvent, kQueueCapacity>>()),
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
    io_thread_ = std::thread(&Server::io_thread_func, this);

    return true;
}

void Server::stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    running_.store(false, std::memory_order_release);

    if (io_thread_.joinable()) {
        io_thread_.join();
    }

    for (auto& [id, conn] : connections_) {
        close_socket(conn->socket);
    }
    connections_.clear();

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

void Server::io_thread_func() {
    std::vector<EventLoop::Event> events;
    events.reserve(256);

    while (running_.load(std::memory_order_acquire)) {
        process_outgoing();

        int n = event_loop_->poll(events, 1);

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

            if (ev.readable) {
                handle_read(*conn);
            }

            // Check conn is still valid after read (may have been disconnected)
            if (conn->state != ConnectionState::Connected) continue;

            if (ev.writable) {
                handle_write(*conn);
            }
        }

        handle_accept();

        // Retry parsing for connections where the incoming queue was previously full
        for (auto& [id, conn] : connections_) {
            if (conn->needs_reparse && conn->state == ConnectionState::Connected) {
                conn->needs_reparse = false;
                parse_messages(*conn);
            }
        }

        // Clean up closed connections
        for (auto it = connections_.begin(); it != connections_.end(); ) {
            if (it->second->state == ConnectionState::Closed) {
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void Server::handle_accept() {
    for (int i = 0; i < 32; ++i) {
        // Enforce max connections
        if (connections_.size() >= config_.max_connections) break;

        socket_t client_sock = accept_connection(listen_socket_);
        if (client_sock == kInvalidSocket) break;

        auto conn = std::make_unique<Connection>();
        conn->id = next_id_.fetch_add(1, std::memory_order_relaxed);
        conn->socket = client_sock;
        conn->state = ConnectionState::Connected;

        Connection* raw = conn.get();
        event_loop_->add(client_sock, raw);

        ConnectionId id = conn->id;
        connections_[id] = std::move(conn);

        IncomingEvent ev;
        ev.type = IncomingEvent::Connect;
        ev.conn_id = id;
        incoming_queue_->push(std::move(ev));
    }
}

void Server::handle_read(Connection& conn) {
    while (true) {
        if (conn.read_pos >= conn.read_buf.size()) {
            if (conn.read_buf.size() >= kMaxPayloadSize + kMessageHeaderSize) {
                // Buffer is at max and full — client is sending garbage
                disconnect(conn);
                return;
            }
            conn.read_buf.resize(conn.read_buf.size() + kBufferSize);
        }

        size_t space = conn.read_buf.size() - conn.read_pos;
        int bytes = socket_recv(conn.socket, conn.read_buf.data() + conn.read_pos, space);

        if (bytes > 0) {
            conn.read_pos += bytes;
            parse_messages(conn);
        } else if (bytes == 0) {
            disconnect(conn);
            return;
        } else {
            if (!would_block()) {
                disconnect(conn);
            }
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
            if (entry.offset >= buf.size()) {
                conn.write_queue.pop_front();
            }
        } else {
            if (!would_block()) {
                disconnect(conn);
            }
            return;
        }
    }

    if (conn.write_queue.empty()) {
        event_loop_->set_writable(conn.socket, false, &conn);
    }
}

void Server::parse_messages(Connection& conn) {
    size_t offset = 0;

    while (offset + kMessageHeaderSize <= conn.read_pos) {
        auto header = MessageHeader::decode(conn.read_buf.data() + offset);
        size_t total_size = kMessageHeaderSize + header.size;

        if (offset + total_size > conn.read_pos) {
            break;
        }

        Message msg;
        Message::deserialize(conn.read_buf.data() + offset, total_size, msg);

        IncomingEvent ev;
        ev.type = IncomingEvent::Data;
        ev.conn_id = conn.id;
        ev.message = std::move(msg);

        if (!incoming_queue_->push(std::move(ev))) {
            conn.needs_reparse = true;
            break;
        }

        offset += total_size;
    }

    if (offset > 0) {
        size_t remaining = conn.read_pos - offset;
        if (remaining > 0) {
            std::memmove(conn.read_buf.data(), conn.read_buf.data() + offset, remaining);
        }
        conn.read_pos = remaining;
    }
}

void Server::disconnect(Connection& conn) {
    if (conn.state == ConnectionState::Closed) return;

    conn.state = ConnectionState::Closed;
    event_loop_->remove(conn.socket);
    close_socket(conn.socket);
    conn.socket = kInvalidSocket;

    IncomingEvent ev;
    ev.type = IncomingEvent::Disconnect;
    ev.conn_id = conn.id;
    incoming_queue_->push(std::move(ev));
}

void Server::process_outgoing() {
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
                        if (conn.has_pending_writes()) {
                            event_loop_->set_writable(conn.socket, true, &conn);
                        }
                    }
                }
                break;
            }
            case OutgoingCommand::Broadcast: {
                // Share one copy across all connections instead of copying per client
                auto shared = std::make_shared<std::vector<uint8_t>>(std::move(cmd->data));
                for (auto& [id, conn_ptr] : connections_) {
                    if (conn_ptr->state == ConnectionState::Connected) {
                        auto& conn = *conn_ptr;

                        // Backpressure: skip slow clients with bloated write queues
                        if (conn.write_queue.size() >= kMaxWriteQueueSize) continue;

                        bool was_empty = conn.write_queue.empty();
                        conn.enqueue(shared);
                        if (was_empty) {
                            handle_write(conn);
                            if (conn.has_pending_writes()) {
                                event_loop_->set_writable(conn.socket, true, &conn);
                            }
                        }
                    }
                }
                break;
            }
            case OutgoingCommand::Disconnect: {
                auto it = connections_.find(cmd->conn_id);
                if (it != connections_.end()) {
                    disconnect(*it->second);
                }
                break;
            }
        }
    }
}

} // namespace netforge
