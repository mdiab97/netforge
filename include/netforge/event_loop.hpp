#pragma once

#include "netforge/transport.hpp"

#include <vector>

#ifdef _WIN32
    #include <unordered_map>
#endif

namespace netforge {

// Platform abstraction for I/O multiplexing.
// Uses WSAPoll on Windows, epoll (edge-triggered) on Linux.
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    bool add(socket_t sock, void* user_data);
    bool set_writable(socket_t sock, bool want_write, void* user_data);
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
    struct SocketEntry {
        void* user_data;
        size_t poll_index; // index into pollfds_ array
        bool want_write;
    };

    // O(1) lookup by socket handle
    std::unordered_map<socket_t, SocketEntry> entries_;

    // Cached poll array — rebuilt only when sockets are added/removed
    std::vector<WSAPOLLFD> pollfds_;
    std::vector<socket_t> poll_sockets_; // parallel array mapping index -> socket
    bool dirty_{false}; // true when pollfds_ needs rebuild
    void rebuild_pollfds();
#else
    int epoll_fd_{-1};
#endif
};

} // namespace netforge
