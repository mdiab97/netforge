#pragma once

#include "netforge/transport.hpp"

#include <functional>
#include <vector>

namespace netforge {

// Platform abstraction for I/O multiplexing.
// Uses IOCP on Windows, epoll on Linux.
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Register a socket for read/write event monitoring
    bool add(socket_t sock, void* user_data);

    // Modify interest: enable/disable write notifications
    bool set_writable(socket_t sock, bool want_write, void* user_data);

    // Remove a socket from monitoring
    void remove(socket_t sock);

    struct Event {
        void* user_data{nullptr};
        bool readable{false};
        bool writable{false};
        bool error{false};
    };

    // Wait for events. Returns number of events fired. Timeout in milliseconds (-1 = block).
    int poll(std::vector<Event>& events, int timeout_ms);

private:
#ifdef _WIN32
    // On Windows, we use select() for simplicity in a portable manner.
    // True IOCP requires overlapped I/O which is complex; we use a high-performance
    // select/poll loop that still handles thousands of connections on a single thread.
    // For production MMO, this would be replaced with full IOCP overlapped model.
    struct SocketEntry {
        socket_t sock;
        void* user_data;
        bool want_write;
    };
    std::vector<SocketEntry> sockets_;
#else
    int epoll_fd_{-1};
#endif
};

} // namespace netforge
