#pragma once

#include "netforge/transport.hpp"
#include "netforge/message.hpp"
#include "netforge/buffer_pool.hpp"

#include <cstdint>
#include <memory>
#include <vector>
#include <deque>

namespace netforge {

using ConnectionId = uint64_t;

enum class ConnectionState {
    Connected,
    Closed
};

// Shared write data — avoids copying the same message N times during broadcast.
// For unicast, the shared_ptr overhead is negligible compared to the send syscall.
using SharedData = std::shared_ptr<std::vector<uint8_t>>;

struct WriteEntry {
    SharedData data;
    size_t offset{0}; // partial send progress within this entry
};

// Connection is a plain state struct. No threads, no async logic.
// The event loop drives reads/writes on all connections.
struct Connection {
    ConnectionId id{0};
    socket_t socket{kInvalidSocket};
    ConnectionState state{ConnectionState::Closed};

    // Read buffer: accumulates incoming bytes until a full message is parsed
    std::vector<uint8_t> read_buf;
    size_t read_pos{0};

    // Write queue: shared data entries waiting to be flushed
    std::deque<WriteEntry> write_queue;

    // Set when parse_messages couldn't push all parsed messages (queue full)
    bool needs_reparse{false};

    Connection() { read_buf.resize(kBufferSize); }

    void reset() {
        id = 0;
        socket = kInvalidSocket;
        state = ConnectionState::Closed;
        read_pos = 0;
        write_queue.clear();
        needs_reparse = false;
    }

    bool has_pending_writes() const {
        return !write_queue.empty();
    }

    void enqueue(SharedData data) {
        write_queue.push_back({std::move(data), 0});
    }

    void enqueue(std::vector<uint8_t> data) {
        enqueue(std::make_shared<std::vector<uint8_t>>(std::move(data)));
    }
};

} // namespace netforge
