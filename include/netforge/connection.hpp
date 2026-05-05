#pragma once

#include "netforge/transport.hpp"
#include "netforge/message.hpp"
#include "netforge/buffer_pool.hpp"

#include <cstdint>
#include <vector>
#include <deque>

namespace netforge {

using ConnectionId = uint64_t;

enum class ConnectionState {
    Connecting,
    Connected,
    Disconnecting,
    Closed
};

// Connection is a plain state struct. No threads, no async logic.
// The event loop drives reads/writes on all connections.
struct Connection {
    ConnectionId id{0};
    socket_t socket{kInvalidSocket};
    ConnectionState state{ConnectionState::Closed};

    // Read buffer: accumulates incoming bytes until a full message is parsed
    std::vector<uint8_t> read_buf;
    size_t read_pos{0}; // bytes currently in read_buf

    // Write queue: serialized messages waiting to be flushed
    std::deque<std::vector<uint8_t>> write_queue;
    size_t write_offset{0}; // offset into front of write_queue (partial send)

    // Set when parse_messages couldn't push all parsed messages (queue full)
    bool needs_reparse{false};

    Connection() { read_buf.resize(kBufferSize); }

    void reset() {
        id = 0;
        socket = kInvalidSocket;
        state = ConnectionState::Closed;
        read_pos = 0;
        write_queue.clear();
        write_offset = 0;
        needs_reparse = false;
    }

    bool has_pending_writes() const {
        return !write_queue.empty();
    }

    // Queue a serialized message for sending
    void enqueue(std::vector<uint8_t> data) {
        write_queue.push_back(std::move(data));
    }
};

} // namespace netforge
