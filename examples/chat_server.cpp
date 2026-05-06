#include <netforge/netforge.hpp>

#include <cstdio>
#include <csignal>
#include <atomic>
#include <string>
#include <unordered_map>
#include <thread>

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
    std::unordered_map<netforge::ConnectionId, std::string> nicknames;

    server.on_connect([&](netforge::ConnectionId id) {
        std::string name = "User" + std::to_string(id);
        nicknames[id] = name;
        std::printf("[chat] %s joined\n", name.c_str());

        auto msg = netforge::Message::from_string(1, name + " joined the chat");
        server.broadcast(msg);
    });

    server.on_disconnect([&](netforge::ConnectionId id) {
        auto it = nicknames.find(id);
        std::string name = (it != nicknames.end()) ? it->second : "Unknown";
        std::printf("[chat] %s left\n", name.c_str());

        auto msg = netforge::Message::from_string(1, name + " left the chat");
        server.broadcast(msg);

        nicknames.erase(id);
    });

    server.on_message([&](netforge::ConnectionId id, netforge::Message msg) {
        auto it = nicknames.find(id);
        std::string name = (it != nicknames.end()) ? it->second : "Unknown";
        std::string text = msg.as_string();

        // Check for /nick command
        if (text.substr(0, 6) == "/nick ") {
            std::string old_name = name;
            std::string new_name = text.substr(6);
            nicknames[id] = new_name;
            std::printf("[chat] %s is now known as %s\n", old_name.c_str(), new_name.c_str());

            auto announce = netforge::Message::from_string(1,
                old_name + " is now known as " + new_name);
            server.broadcast(announce);
            return;
        }

        std::printf("[chat] %s: %s\n", name.c_str(), text.c_str());

        auto broadcast_msg = netforge::Message::from_string(1, name + ": " + text);
        server.broadcast(broadcast_msg);
    });

    if (!server.start_threaded({.port = 9001})) {
        std::fprintf(stderr, "Failed to start chat server on port 9001\n");
        return 1;
    }

    std::printf("[chat] Server running on port 9001. Press Ctrl+C to stop.\n");

    while (g_running.load()) {
        server.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    server.stop();
    netforge::net_cleanup();
    std::printf("[chat] Server stopped.\n");
    return 0;
}
