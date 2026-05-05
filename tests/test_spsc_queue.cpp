#include <catch2/catch_test_macros.hpp>
#include <netforge/spsc_queue.hpp>

#include <thread>
#include <atomic>
#include <vector>

using namespace netforge;

TEST_CASE("SpscQueue basic push and pop", "[spsc]") {
    SpscQueue<int, 16> q;

    REQUIRE(q.empty());
    REQUIRE(q.size() == 0);

    REQUIRE(q.push(42));
    REQUIRE_FALSE(q.empty());
    REQUIRE(q.size() == 1);

    auto val = q.pop();
    REQUIRE(val.has_value());
    REQUIRE(*val == 42);
    REQUIRE(q.empty());
}

TEST_CASE("SpscQueue fills to capacity", "[spsc]") {
    SpscQueue<int, 8> q; // capacity is 7 (power-of-two minus 1)

    for (int i = 0; i < 7; ++i) {
        REQUIRE(q.push(i));
    }
    REQUIRE(q.size() == 7);

    // Should be full
    REQUIRE_FALSE(q.push(99));

    // Pop one, then push succeeds
    auto val = q.pop();
    REQUIRE(val.has_value());
    REQUIRE(*val == 0);
    REQUIRE(q.push(99));
}

TEST_CASE("SpscQueue maintains FIFO order", "[spsc]") {
    SpscQueue<int, 64> q;

    for (int i = 0; i < 50; ++i) {
        REQUIRE(q.push(i));
    }

    for (int i = 0; i < 50; ++i) {
        auto val = q.pop();
        REQUIRE(val.has_value());
        REQUIRE(*val == i);
    }

    REQUIRE(q.empty());
}

TEST_CASE("SpscQueue pop on empty returns nullopt", "[spsc]") {
    SpscQueue<int, 16> q;
    auto val = q.pop();
    REQUIRE_FALSE(val.has_value());
}

TEST_CASE("SpscQueue wraps around correctly", "[spsc]") {
    SpscQueue<int, 8> q;

    // Fill and drain multiple times to force wraparound
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 5; ++i) {
            REQUIRE(q.push(round * 100 + i));
        }
        for (int i = 0; i < 5; ++i) {
            auto val = q.pop();
            REQUIRE(val.has_value());
            REQUIRE(*val == round * 100 + i);
        }
        REQUIRE(q.empty());
    }
}

TEST_CASE("SpscQueue works with move-only types", "[spsc]") {
    SpscQueue<std::unique_ptr<int>, 16> q;

    auto ptr = std::make_unique<int>(42);
    REQUIRE(q.push(std::move(ptr)));

    auto val = q.pop();
    REQUIRE(val.has_value());
    REQUIRE(**val == 42);
}

TEST_CASE("SpscQueue concurrent producer/consumer", "[spsc]") {
    SpscQueue<int, 1024> q;
    constexpr int count = 100000;
    std::atomic<bool> done{false};
    std::vector<int> received;
    received.reserve(count);

    // Producer thread
    std::thread producer([&]() {
        for (int i = 0; i < count; ++i) {
            while (!q.push(i)) {
                std::this_thread::yield();
            }
        }
        done.store(true, std::memory_order_release);
    });

    // Consumer (this thread)
    while (!done.load(std::memory_order_acquire) || !q.empty()) {
        if (auto val = q.pop()) {
            received.push_back(*val);
        } else {
            std::this_thread::yield();
        }
    }

    producer.join();

    // Verify all values received in order
    REQUIRE(received.size() == count);
    for (int i = 0; i < count; ++i) {
        REQUIRE(received[i] == i);
    }
}

TEST_CASE("SpscQueue capacity method", "[spsc]") {
    SpscQueue<int, 256> q;
    REQUIRE(SpscQueue<int, 256>::capacity() == 255);
}
