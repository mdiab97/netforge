#pragma once

#include <cstdint>
#include <cstddef>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <mswsock.h>
    using socket_t = SOCKET;
    constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <cerrno>
    using socket_t = int;
    constexpr socket_t kInvalidSocket = -1;
#endif

namespace netforge {

// Initialize platform socket layer (WSAStartup on Windows, no-op on Linux)
bool net_init();
void net_cleanup();

// Create a non-blocking TCP socket
socket_t create_tcp_socket();

// Set socket to non-blocking mode
bool set_nonblocking(socket_t sock);

// Set TCP_NODELAY
bool set_nodelay(socket_t sock);

// Set SO_REUSEADDR
bool set_reuseaddr(socket_t sock);

// Bind and listen on the given port
bool bind_and_listen(socket_t sock, uint16_t port, int backlog = 128);

// Accept a new connection (non-blocking). Returns kInvalidSocket if no pending connection.
socket_t accept_connection(socket_t listen_sock);

// Connect to a remote host (non-blocking). Returns true if connect initiated.
bool connect_nonblocking(socket_t sock, const char* host, uint16_t port);

// Close a socket
void close_socket(socket_t sock);

// Check if last error is EWOULDBLOCK/EAGAIN
bool would_block();

// Non-blocking send. Returns bytes sent, or -1 on error (check would_block).
int socket_send(socket_t sock, const uint8_t* data, size_t len);

// Non-blocking recv. Returns bytes received, 0 on disconnect, -1 on would-block/error.
int socket_recv(socket_t sock, uint8_t* buf, size_t len);

} // namespace netforge
