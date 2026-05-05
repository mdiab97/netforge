#include <netforge/netforge.hpp>
#include <cstdio>
#include <iostream>
#include <string>
#include <atomic>
#include <thread>

int main() {
    if (!netforge::net_init()) {
        std::fprintf(stderr, "Failed to initialize networking\n");
        return 1;
    }

    netforge::Client client;
    std::atomic<bool> connected{false};
    std::atomic<bool> running{true};

    client.on_connect([&]() {
        std::printf("[echo] Connected to server\n");
        connected.store(true);
    });

    client.on_disconnect([&]() {
        std::printf("[echo] Disconnected from server\n");
        running.store(false);
    });

    client.on_message([](netforge::Message msg) {
        std::printf("[echo] Reply: %s\n", msg.as_string().c_str());
    });

    if (!client.connect({.host = "127.0.0.1", .port = 9000})) {
        std::fprintf(stderr, "Failed to connect to server\n");
        return 1;
    }

    std::printf("[echo] Connecting to 127.0.0.1:9000...\n");

    // Poll thread for processing events
    std::thread poll_thread([&]() {
        while (running.load()) {
            client.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Wait for connection
    while (!connected.load() && running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!connected.load()) {
        std::fprintf(stderr, "Connection failed\n");
        running.store(false);
        poll_thread.join();
        return 1;
    }

    std::printf("[echo] Type messages to send (empty line to quit):\n");

    std::string line;
    while (running.load() && std::getline(std::cin, line)) {
        if (line.empty()) break;
        client.send(netforge::Message::from_string(1, line));
    }

    running.store(false);
    poll_thread.join();
    client.disconnect();
    netforge::net_cleanup();
    return 0;
}
