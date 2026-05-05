#include "netforge/event_loop.hpp"

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/epoll.h>
    #include <unistd.h>
#endif

#include <algorithm>

namespace netforge {

#ifdef _WIN32

// Windows implementation using WSAPoll, which handles arbitrary numbers of sockets
// without the FD_SETSIZE limitation of select().

EventLoop::EventLoop() {}

EventLoop::~EventLoop() {}

bool EventLoop::add(socket_t sock, void* user_data) {
    sockets_.push_back({sock, user_data, false});
    return true;
}

bool EventLoop::set_writable(socket_t sock, bool want_write, void* user_data) {
    for (auto& entry : sockets_) {
        if (entry.sock == sock) {
            entry.want_write = want_write;
            entry.user_data = user_data;
            return true;
        }
    }
    return false;
}

void EventLoop::remove(socket_t sock) {
    sockets_.erase(
        std::remove_if(sockets_.begin(), sockets_.end(),
                       [sock](const SocketEntry& e) { return e.sock == sock; }),
        sockets_.end());
}

int EventLoop::poll(std::vector<Event>& events, int timeout_ms) {
    events.clear();
    if (sockets_.empty()) return 0;

    // Build WSAPOLLFD array
    std::vector<WSAPOLLFD> pollfds;
    pollfds.reserve(sockets_.size());

    for (const auto& entry : sockets_) {
        WSAPOLLFD pfd{};
        pfd.fd = entry.sock;
        pfd.events = POLLRDNORM;
        if (entry.want_write) {
            pfd.events |= POLLWRNORM;
        }
        pollfds.push_back(pfd);
    }

    int result = WSAPoll(pollfds.data(), static_cast<ULONG>(pollfds.size()), timeout_ms);
    if (result <= 0) return 0;

    for (size_t i = 0; i < pollfds.size(); ++i) {
        if (pollfds[i].revents == 0) continue;

        Event ev{};
        ev.user_data = sockets_[i].user_data;
        ev.readable = (pollfds[i].revents & (POLLRDNORM | POLLHUP)) != 0;
        ev.writable = (pollfds[i].revents & POLLWRNORM) != 0;
        ev.error = (pollfds[i].revents & POLLERR) != 0;
        events.push_back(ev);
    }

    return static_cast<int>(events.size());
}

#else

// Linux implementation using epoll with edge-triggered notifications.

EventLoop::EventLoop() {
    epoll_fd_ = epoll_create1(0);
}

EventLoop::~EventLoop() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

bool EventLoop::add(socket_t sock, void* user_data) {
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = user_data;
    return epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, sock, &ev) == 0;
}

bool EventLoop::set_writable(socket_t sock, bool want_write, void* user_data) {
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    if (want_write) ev.events |= EPOLLOUT;
    ev.data.ptr = user_data;
    return epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, sock, &ev) == 0;
}

void EventLoop::remove(socket_t sock) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, sock, nullptr);
}

int EventLoop::poll(std::vector<Event>& events, int timeout_ms) {
    events.clear();

    static constexpr int kMaxEvents = 512;
    epoll_event raw_events[kMaxEvents];

    int n = epoll_wait(epoll_fd_, raw_events, kMaxEvents, timeout_ms);
    if (n <= 0) return 0;

    events.reserve(n);
    for (int i = 0; i < n; ++i) {
        Event ev{};
        ev.user_data = raw_events[i].data.ptr;
        ev.readable = (raw_events[i].events & EPOLLIN) != 0;
        ev.writable = (raw_events[i].events & EPOLLOUT) != 0;
        ev.error = (raw_events[i].events & (EPOLLERR | EPOLLHUP)) != 0;
        events.push_back(ev);
    }

    return static_cast<int>(events.size());
}

#endif

} // namespace netforge
