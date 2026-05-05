#include <netforge/netforge.hpp>
#include <cstdio>
#include <csignal>
#include <atomic>

static std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running.store(false);
}

int main() {
    std::signal(SIGINT, signal_handler);

    if (!netforge::net_init()) {
        std::fprintf(stderr, "Failed to initialize networking\n");
        return 1;
    }

    netforge::Server server;

    server.on_connect([](netforge::ConnectionId id) {
        std::printf("[echo] Client %llu connected\n", (unsigned long long)id);
    });

    server.on_disconnect([](netforge::ConnectionId id) {
        std::printf("[echo] Client %llu disconnected\n", (unsigned long long)id);
    });

    server.on_message([&server](netforge::ConnectionId id, netforge::Message msg) {
        std::printf("[echo] Received %zu bytes from client %llu, echoing back\n",
                    msg.size(), (unsigned long long)id);
        server.send(id, msg);
    });

    if (!server.start({.port = 9000})) {
        std::fprintf(stderr, "Failed to start server on port 9000\n");
        return 1;
    }

    std::printf("[echo] Server running on port 9000. Press Ctrl+C to stop.\n");

    while (g_running.load()) {
        server.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    server.stop();
    netforge::net_cleanup();
    std::printf("[echo] Server stopped.\n");
    return 0;
}
