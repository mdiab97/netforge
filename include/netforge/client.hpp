#pragma once

#include "netforge/transport.hpp"
#include "netforge/message.hpp"
#include "netforge/connection.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "netforge/spsc_queue.hpp"

namespace netforge {

struct ClientConfig {
    std::string host{"127.0.0.1"};
    uint16_t port{9000};
    int connect_timeout_ms{5000};
};

class Client {
public:
    using ConnectCallback = std::function<void()>;
    using DisconnectCallback = std::function<void()>;
    using MessageCallback = std::function<void(Message)>;

    Client();
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void on_connect(ConnectCallback cb);
    void on_disconnect(DisconnectCallback cb);
    void on_message(MessageCallback cb);

    bool connect(const ClientConfig& config);
    void disconnect();

    // Send a message (thread-safe, queues to I/O thread).
    // Returns false if the outgoing queue is full (caller should retry or drop).
    bool send(const Message& msg);

    // Process incoming events on the calling thread (fires callbacks)
    void poll();

    bool is_connected() const { return connected_.load(std::memory_order_acquire); }

private:
    void io_thread_func();
    bool read_incoming();
    void parse_incoming();
    void flush_outgoing();
    void push_disconnect();

    struct IncomingEvent {
        enum Type { Connected, Disconnected, Data };
        Type type;
        Message message;
    };

    Connection conn_;
    ClientConfig config_;

    // Client-local read buffer (not shared with server's processing thread)
    std::vector<uint8_t> read_buf_;
    size_t read_pos_{0};

    // Write buffer for partial sends (owned by I/O thread)
    std::deque<WriteEntry> write_queue_;

    static constexpr size_t kQueueCapacity = 131072;
    std::unique_ptr<SpscQueue<IncomingEvent, kQueueCapacity>> incoming_queue_;
    std::unique_ptr<SpscQueue<std::vector<uint8_t>, kQueueCapacity>> outgoing_queue_;

    ConnectCallback on_connect_cb_;
    DisconnectCallback on_disconnect_cb_;
    MessageCallback on_message_cb_;

    std::thread io_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
};

} // namespace netforge
