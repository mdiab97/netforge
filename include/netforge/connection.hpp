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

using SharedData = std::shared_ptr<std::vector<uint8_t>>;

struct WriteEntry {
    SharedData data;
    size_t offset{0};
};

// Connection is owned by the I/O thread.
// It holds only socket state and the write queue.
// Read buffers are managed separately by the processing thread.
struct Connection {
    ConnectionId id{0};
    socket_t socket{kInvalidSocket};
    ConnectionState state{ConnectionState::Closed};

    // Write queue: shared data entries waiting to be flushed
    std::deque<WriteEntry> write_queue;

    void reset() {
        id = 0;
        socket = kInvalidSocket;
        state = ConnectionState::Closed;
        write_queue.clear();
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
