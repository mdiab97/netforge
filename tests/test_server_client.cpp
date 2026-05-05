#include <catch2/catch_test_macros.hpp>
#include <netforge/netforge.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <string>

using namespace netforge;
using namespace std::chrono_literals;

static constexpr uint16_t TEST_PORT = 44500;

// Helper to wait for a condition with timeout
template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = 3000ms) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        std::this_thread::sleep_for(5ms);
        if (std::chrono::steady_clock::now() - start > timeout) {
            return false;
        }
    }
    return true;
}

TEST_CASE("Server starts and stops cleanly", "[integration]") {
    Server server;
    REQUIRE(server.start({.port = TEST_PORT}));
    REQUIRE(server.is_running());
    server.stop();
    REQUIRE_FALSE(server.is_running());
}

TEST_CASE("Client connects to server", "[integration]") {
    std::atomic<bool> client_connected{false};
    std::atomic<bool> server_saw_connect{false};

    Server server;
    server.on_connect([&](ConnectionId) {
        server_saw_connect.store(true);
    });
    REQUIRE(server.start({.port = TEST_PORT + 1}));

    Client client;
    client.on_connect([&]() {
        client_connected.store(true);
    });
    REQUIRE(client.connect({.host = "127.0.0.1", .port = TEST_PORT + 1}));

    // Poll both until connected
    REQUIRE(wait_for([&]() {
        server.poll();
        client.poll();
        return client_connected.load() && server_saw_connect.load();
    }));

    client.disconnect();
    server.stop();
}

TEST_CASE("Client sends message to server", "[integration]") {
    std::atomic<bool> received{false};
    std::string received_text;

    Server server;
    server.on_message([&](ConnectionId, Message msg) {
        received_text = msg.as_string();
        received.store(true);
    });
    REQUIRE(server.start({.port = TEST_PORT + 2}));

    Client client;
    std::atomic<bool> connected{false};
    client.on_connect([&]() { connected.store(true); });
    REQUIRE(client.connect({.host = "127.0.0.1", .port = TEST_PORT + 2}));

    REQUIRE(wait_for([&]() {
        client.poll();
        server.poll();
        return connected.load();
    }));

    client.send(Message::from_string(1, "Hello Server!"));

    REQUIRE(wait_for([&]() {
        server.poll();
        return received.load();
    }));

    REQUIRE(received_text == "Hello Server!");

    client.disconnect();
    server.stop();
}

TEST_CASE("Server sends message to client", "[integration]") {
    std::atomic<bool> received{false};
    std::string received_text;
    ConnectionId server_conn_id{0};

    Server server;
    server.on_connect([&](ConnectionId id) {
        server_conn_id = id;
    });
    REQUIRE(server.start({.port = TEST_PORT + 3}));

    Client client;
    std::atomic<bool> connected{false};
    client.on_connect([&]() { connected.store(true); });
    client.on_message([&](Message msg) {
        received_text = msg.as_string();
        received.store(true);
    });
    REQUIRE(client.connect({.host = "127.0.0.1", .port = TEST_PORT + 3}));

    REQUIRE(wait_for([&]() {
        server.poll();
        client.poll();
        return connected.load() && server_conn_id != 0;
    }));

    server.send(server_conn_id, Message::from_string(2, "Hello Client!"));

    REQUIRE(wait_for([&]() {
        client.poll();
        return received.load();
    }));

    REQUIRE(received_text == "Hello Client!");

    client.disconnect();
    server.stop();
}

TEST_CASE("Server broadcast reaches all clients", "[integration]") {
    constexpr int NUM_CLIENTS = 5;
    std::atomic<int> messages_received{0};

    Server server;
    std::atomic<int> connections{0};
    server.on_connect([&](ConnectionId) {
        connections.fetch_add(1);
    });
    REQUIRE(server.start({.port = TEST_PORT + 4}));

    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        auto c = std::make_unique<Client>();
        c->on_message([&](Message) {
            messages_received.fetch_add(1);
        });
        REQUIRE(c->connect({.host = "127.0.0.1", .port = TEST_PORT + 4}));
        clients.push_back(std::move(c));
    }

    // Wait for all to connect
    REQUIRE(wait_for([&]() {
        server.poll();
        for (auto& c : clients) c->poll();
        return connections.load() == NUM_CLIENTS;
    }));

    // Broadcast
    server.broadcast(Message::from_string(10, "Broadcast!"));

    REQUIRE(wait_for([&]() {
        for (auto& c : clients) c->poll();
        return messages_received.load() == NUM_CLIENTS;
    }));

    for (auto& c : clients) c->disconnect();
    server.stop();
}

TEST_CASE("Disconnect callback fires", "[integration]") {
    std::atomic<bool> server_saw_disconnect{false};
    std::atomic<bool> client_saw_disconnect{false};

    Server server;
    server.on_disconnect([&](ConnectionId) {
        server_saw_disconnect.store(true);
    });
    REQUIRE(server.start({.port = TEST_PORT + 5}));

    Client client;
    std::atomic<bool> connected{false};
    client.on_connect([&]() { connected.store(true); });
    client.on_disconnect([&]() { client_saw_disconnect.store(true); });
    REQUIRE(client.connect({.host = "127.0.0.1", .port = TEST_PORT + 5}));

    REQUIRE(wait_for([&]() {
        server.poll();
        client.poll();
        return connected.load();
    }));

    // Client disconnects
    client.disconnect();

    // Server should see disconnect
    REQUIRE(wait_for([&]() {
        server.poll();
        return server_saw_disconnect.load();
    }));

    server.stop();
}

TEST_CASE("Echo roundtrip", "[integration]") {
    std::atomic<bool> got_echo{false};
    std::string echo_text;

    Server server;
    server.on_message([&](ConnectionId id, Message msg) {
        server.send(id, msg); // echo back
    });
    REQUIRE(server.start({.port = TEST_PORT + 6}));

    Client client;
    std::atomic<bool> connected{false};
    client.on_connect([&]() { connected.store(true); });
    client.on_message([&](Message msg) {
        echo_text = msg.as_string();
        got_echo.store(true);
    });
    REQUIRE(client.connect({.host = "127.0.0.1", .port = TEST_PORT + 6}));

    REQUIRE(wait_for([&]() {
        server.poll();
        client.poll();
        return connected.load();
    }));

    client.send(Message::from_string(1, "Ping!"));

    REQUIRE(wait_for([&]() {
        server.poll();
        client.poll();
        return got_echo.load();
    }));

    REQUIRE(echo_text == "Ping!");

    client.disconnect();
    server.stop();
}
