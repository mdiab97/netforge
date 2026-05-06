#include "netforge/transport.hpp"

#include <algorithm>
#include <climits>

#ifdef _WIN32
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "mswsock.lib")
#endif

namespace netforge {

bool net_init() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void net_cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

socket_t create_tcp_socket() {
    socket_t sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidSocket) return kInvalidSocket;

    if (!set_nonblocking(sock)) {
        close_socket(sock);
        return kInvalidSocket;
    }

    set_nodelay(sock);
    return sock;
}

bool set_nonblocking(socket_t sock) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

bool set_nodelay(socket_t sock) {
    int val = 1;
    return setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&val), sizeof(val)) == 0;
}

bool set_reuseaddr(socket_t sock) {
    int val = 1;
    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                      reinterpret_cast<const char*>(&val), sizeof(val)) == 0;
}

bool bind_and_listen(socket_t sock, uint16_t port, int backlog) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return false;
    }

    return ::listen(sock, backlog) == 0;
}

socket_t accept_connection(socket_t listen_sock) {
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);

    socket_t client = ::accept(listen_sock, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (client == kInvalidSocket) return kInvalidSocket;

    if (!set_nonblocking(client)) {
        close_socket(client);
        return kInvalidSocket;
    }

    set_nodelay(client);
    return client;
}

bool connect_nonblocking(socket_t sock, const char* host, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        return false;
    }

    int result = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result == 0) return true;

#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS;
#endif
}

void close_socket(socket_t sock) {
    if (sock == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(sock);
#else
    ::close(sock);
#endif
}

bool would_block() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

int socket_send(socket_t sock, const uint8_t* data, size_t len) {
    // Clamp to INT_MAX to prevent integer overflow when casting to int
    int chunk = static_cast<int>((std::min)(len, static_cast<size_t>(INT_MAX)));
#ifdef _WIN32
    return ::send(sock, reinterpret_cast<const char*>(data), chunk, 0);
#else
    return static_cast<int>(::send(sock, data, chunk, MSG_NOSIGNAL));
#endif
}

int socket_recv(socket_t sock, uint8_t* buf, size_t len) {
    int chunk = static_cast<int>((std::min)(len, static_cast<size_t>(INT_MAX)));
#ifdef _WIN32
    return ::recv(sock, reinterpret_cast<char*>(buf), chunk, 0);
#else
    return static_cast<int>(::recv(sock, buf, chunk, 0));
#endif
}

} // namespace netforge
