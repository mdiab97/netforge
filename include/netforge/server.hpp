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

    void on_connect(ConnectCallback cb);
    void on_disconnect(DisconnectCallback cb);
    void on_message(MessageCallback cb);

    // Initialize the server (listen socket, event loop, queues).
    // Does NOT spawn any threads — you drive it with tick functions.
    bool start(const ServerConfig& config);

    // Shut down: close all connections and the listen socket.
    void stop();

    // ── Thread tick functions ──────────────────────────────────────
    // Call each from whatever thread you choose. They are safe to call
    // concurrently (each operates on its own data, connected by SPSC queues).

    // I/O tick: poll sockets, accept, recv, send. Call from your I/O thread.
    // timeout_ms: event loop poll timeout (-1 = block, 0 = non-blocking).
    void tick_io(int timeout_ms = 1);

    // Processing tick: parse raw bytes into messages. Call from your processing thread.
    void tick_process();

    // Game tick: fire callbacks (on_connect, on_message, on_disconnect).
    // Call from your game/main thread.
    void poll();

    // ── Convenience: auto-threaded mode ───────────────────────────
    // Spawns I/O and processing threads internally.
    // Equivalent to calling start() then running tick_io()/tick_process() in loops.
    bool start_threaded(const ServerConfig& config);

    // ── Send API (thread-safe, pushes to outgoing queue) ──────────
    void send(ConnectionId id, const Message& msg);
    void broadcast(const Message& msg);

    bool is_running() const { return running_.load(std::memory_order_acquire); }

private:
    void handle_accept();
    void handle_read(Connection& conn);
    void handle_write(Connection& conn);
    void disconnect(Connection& conn);
    void drain_outgoing();

    void process_raw_data(ConnectionId id, const uint8_t* data, size_t len);
    void flush_connection_buffer(ConnectionId id);

    // Raw event from I/O -> processing
    struct RawEvent {
        enum Type { Data, Connect, Disconnect };
        Type type;
        ConnectionId conn_id;
        std::vector<uint8_t> data;
    };

    // Parsed event from processing -> game
    struct IncomingEvent {
        enum Type { Connect, Disconnect, Data };
        Type type;
        ConnectionId conn_id;
        Message message;
    };

    // Command from game -> I/O
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

    // I/O thread state
    std::unordered_map<ConnectionId, std::unique_ptr<Connection>> connections_;
    std::atomic<uint64_t> next_id_{1};

    // Processing thread state
    struct ReadBuffer {
        std::vector<uint8_t> buf;
        size_t pos{0};
        ReadBuffer() { buf.resize(kBufferSize); }
    };
    std::unordered_map<ConnectionId, ReadBuffer> read_buffers_;

    static constexpr size_t kQueueCapacity = 131072;
    std::unique_ptr<SpscQueue<RawEvent, kQueueCapacity>> raw_queue_;
    std::unique_ptr<SpscQueue<IncomingEvent, kQueueCapacity>> incoming_queue_;
    std::unique_ptr<SpscQueue<OutgoingCommand, kQueueCapacity>> outgoing_queue_;

    ConnectCallback on_connect_cb_;
    DisconnectCallback on_disconnect_cb_;
    MessageCallback on_message_cb_;

    // Only used in start_threaded() mode
    std::thread io_thread_;
    std::thread proc_thread_;
    std::atomic<bool> running_{false};
};

} // namespace netforge
