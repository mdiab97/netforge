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

namespace netforge {

#ifdef _WIN32

// Windows implementation using WSAPoll with O(1) socket lookup via hash map
// and a cached WSAPOLLFD array that's only rebuilt on add/remove.

EventLoop::EventLoop() {}

EventLoop::~EventLoop() {}

bool EventLoop::add(socket_t sock, void* user_data) {
    if (entries_.count(sock)) return false; // duplicate

    entries_[sock] = {user_data, 0, false};
    dirty_ = true;
    return true;
}

bool EventLoop::set_writable(socket_t sock, bool want_write, void* user_data) {
    auto it = entries_.find(sock);
    if (it == entries_.end()) return false;

    it->second.want_write = want_write;
    it->second.user_data = user_data;

    // Update the cached pollfd events inline when possible
    if (!dirty_ && it->second.poll_index < pollfds_.size()) {
        SHORT events = POLLRDNORM;
        if (want_write) events |= POLLWRNORM;
        pollfds_[it->second.poll_index].events = events;
    } else {
        // Array is dirty (pending add/remove), force rebuild on next poll
        dirty_ = true;
    }

    return true;
}

void EventLoop::remove(socket_t sock) {
    if (entries_.erase(sock) > 0) {
        dirty_ = true;
    }
}

void EventLoop::rebuild_pollfds() {
    pollfds_.clear();
    poll_sockets_.clear();
    pollfds_.reserve(entries_.size());
    poll_sockets_.reserve(entries_.size());

    size_t index = 0;
    for (auto& [sock, entry] : entries_) {
        WSAPOLLFD pfd{};
        pfd.fd = sock;
        pfd.events = POLLRDNORM;
        if (entry.want_write) pfd.events |= POLLWRNORM;
        pollfds_.push_back(pfd);
        poll_sockets_.push_back(sock);
        entry.poll_index = index++;
    }

    dirty_ = false;
}

int EventLoop::poll(std::vector<Event>& events, int timeout_ms) {
    events.clear();
    if (entries_.empty()) return 0;

    if (dirty_) {
        rebuild_pollfds();
    }

    int result = WSAPoll(pollfds_.data(), static_cast<ULONG>(pollfds_.size()), timeout_ms);
    if (result <= 0) return 0;

    for (size_t i = 0; i < pollfds_.size(); ++i) {
        if (pollfds_[i].revents == 0) continue;

        auto it = entries_.find(poll_sockets_[i]);
        if (it == entries_.end()) continue;

        Event ev{};
        ev.user_data = it->second.user_data;
        ev.readable = (pollfds_[i].revents & (POLLRDNORM | POLLHUP)) != 0;
        ev.writable = (pollfds_[i].revents & POLLWRNORM) != 0;
        ev.error = (pollfds_[i].revents & POLLERR) != 0;
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
