#pragma once

#include "netforge/connection.hpp"
#include "netforge/message.hpp"
#include "netforge/event_loop.hpp"
#include "netforge/buffer_pool.hpp"
#include "netforge/spsc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

namespace netforge {

struct ServerConfig {
    uint16_t port{9000};
    size_t max_connections{10000};
    size_t buffer_pool_size{1024};
};

class Server {
public:
    using ConnectCallback = std::function<void(ConnectionId)>;
    using DisconnectCallback = std::function<void(ConnectionId)>;
    using MessageCallback = std::function<void(ConnectionId, Message)>;

    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Set callbacks before calling start()
    void on_connect(ConnectCallback cb);
    void on_disconnect(DisconnectCallback cb);
    void on_message(MessageCallback cb);

    // Start the server (spawns I/O thread)
    bool start(const ServerConfig& config);

    // Stop the server and join I/O thread
    void stop();

    // Send a message to a specific connection (thread-safe, queues for I/O thread)
    void send(ConnectionId id, const Message& msg);

    // Broadcast a message to all connected clients
    void broadcast(const Message& msg);

    // Call from game thread to process incoming events (fires callbacks)
    void poll();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

private:
    void io_thread_func();
    void handle_accept();
    void handle_read(Connection& conn);
    void handle_write(Connection& conn);
    void parse_messages(Connection& conn);
    void disconnect(Connection& conn);
    void process_outgoing();

    // Incoming event from I/O thread to game thread
    struct IncomingEvent {
        enum Type { Connect, Disconnect, Data };
        Type type;
        ConnectionId conn_id;
        Message message;
    };

    // Outgoing command from game thread to I/O thread
    struct OutgoingCommand {
        enum Type { Send, Broadcast, Disconnect };
        Type type;
        ConnectionId conn_id;
        std::vector<uint8_t> data;
    };

    ServerConfig config_;
    socket_t listen_socket_{kInvalidSocket};
    std::unique_ptr<EventLoop> event_loop_;
    std::unique_ptr<BufferPool> buffer_pool_;

    std::unordered_map<ConnectionId, std::unique_ptr<Connection>> connections_;
    std::atomic<uint64_t> next_id_{1};

    // SPSC queues for lock-free communication between threads (heap-allocated
    // because the arrays are too large for default stack size on Windows)
    static constexpr size_t kQueueCapacity = 131072;
    std::unique_ptr<SpscQueue<IncomingEvent, kQueueCapacity>> incoming_queue_;
    std::unique_ptr<SpscQueue<OutgoingCommand, kQueueCapacity>> outgoing_queue_;

    ConnectCallback on_connect_cb_;
    DisconnectCallback on_disconnect_cb_;
    MessageCallback on_message_cb_;

    std::thread io_thread_;
    std::atomic<bool> running_{false};
};

} // namespace netforge
