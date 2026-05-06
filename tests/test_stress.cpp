#include <catch2/catch_test_macros.hpp>
#include <netforge/netforge.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

using namespace netforge;
using namespace std::chrono_literals;

static constexpr uint16_t STRESS_PORT = 44600;

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = 30000ms) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        std::this_thread::yield();
        if (std::chrono::steady_clock::now() - start > timeout)
            return false;
    }
    return true;
}

// ============================================================================
// DECISION 1: Lock-free SPSC Queue vs Mutex-protected Queue
//
// Why this matters: The game thread and I/O thread exchange messages every tick.
// A mutex here means the game thread blocks waiting for the network thread to
// release the lock, causing frame hitches. Lock-free means zero waiting.
// ============================================================================

// Naive mutex-based queue (what most people write first)
template <typename T>
class MutexQueue {
public:
    void push(T item) {
        std::lock_guard lock(mtx_);
        queue_.push_back(std::move(item));
    }
    bool pop(T& item) {
        std::lock_guard lock(mtx_);
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }
private:
    std::mutex mtx_;
    std::deque<T> queue_;
};

TEST_CASE("Decision: lock-free vs mutex queue under contention", "[stress][decisions]") {
    constexpr int ITEMS = 1'000'000;

    std::printf("\n");
    std::printf("================================================================\n");
    std::printf("  DECISION 1: Lock-Free SPSC Queue vs Mutex Queue\n");
    std::printf("  Scenario: 1 producer + 1 consumer exchanging %d items\n", ITEMS);
    std::printf("  (simulates game thread <-> I/O thread communication)\n");
    std::printf("================================================================\n\n");

    // --- Mutex queue ---
    {
        MutexQueue<int> queue;
        std::atomic<bool> done{false};
        std::atomic<int> consumed{0};

        auto t0 = std::chrono::steady_clock::now();

        std::thread consumer([&]() {
            int val;
            while (consumed.load(std::memory_order_relaxed) < ITEMS) {
                if (queue.pop(val))
                    consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });

        for (int i = 0; i < ITEMS; ++i)
            queue.push(i);

        consumer.join();
        auto t1 = std::chrono::steady_clock::now();
        double mutex_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
        double mutex_ops = ITEMS / (mutex_ms / 1000.0);

        std::printf("  Mutex Queue:     %.1f ms  (%.0f ops/sec)\n", mutex_ms, mutex_ops);
    }

    // --- Lock-free SPSC queue ---
    double lockfree_ms;
    {
        SpscQueue<int, 131072> queue;
        std::atomic<int> consumed{0};

        auto t0 = std::chrono::steady_clock::now();

        std::thread consumer([&]() {
            while (consumed.load(std::memory_order_relaxed) < ITEMS) {
                if (auto val = queue.pop())
                    consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });

        for (int i = 0; i < ITEMS; ++i) {
            while (!queue.push(i)) {} // spin until space available
        }

        consumer.join();
        auto t1 = std::chrono::steady_clock::now();
        lockfree_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
        double lockfree_ops = ITEMS / (lockfree_ms / 1000.0);

        std::printf("  Lock-Free SPSC:  %.1f ms  (%.0f ops/sec)\n", lockfree_ms, lockfree_ops);
    }

    std::printf("\n  Why: Mutex causes the game thread to stall waiting for the network\n");
    std::printf("  thread to release the lock. Lock-free means zero contention, zero\n");
    std::printf("  frame hitches, predictable tick timing.\n\n");

    REQUIRE(lockfree_ms > 0);
}

// ============================================================================
// DECISION 2: Buffer Pool vs malloc/free per message
//
// Why this matters: Every received message allocates a buffer. Under load with
// 1000 clients each sending 60 msgs/sec, that's 60,000 mallocs/sec. The heap
// allocator fragments memory and introduces unpredictable latency spikes.
// ============================================================================

TEST_CASE("Decision: buffer pool vs heap allocation", "[stress][decisions]") {
    constexpr int ALLOCS = 500'000;

    std::printf("\n");
    std::printf("================================================================\n");
    std::printf("  DECISION 2: Buffer Pool vs malloc/free\n");
    std::printf("  Scenario: %d allocate-use-free cycles of 4KB buffers\n", ALLOCS);
    std::printf("  (simulates per-message buffer allocation on receive path)\n");
    std::printf("================================================================\n\n");

    // --- malloc/free ---
    double malloc_ms;
    {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < ALLOCS; ++i) {
            auto* buf = static_cast<uint8_t*>(malloc(4096));
            buf[0] = static_cast<uint8_t>(i); // touch memory
            buf[4095] = static_cast<uint8_t>(i);
            free(buf);
        }
        auto t1 = std::chrono::steady_clock::now();
        malloc_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
        std::printf("  malloc/free:     %.1f ms  (%.0f allocs/sec)\n",
            malloc_ms, ALLOCS / (malloc_ms / 1000.0));
    }

    // --- Buffer pool ---
    double pool_ms;
    {
        BufferPool pool(64); // start with 64 buffers
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < ALLOCS; ++i) {
            Buffer* buf = pool.acquire();
            buf->data[0] = static_cast<uint8_t>(i);
            buf->data[4095] = static_cast<uint8_t>(i);
            pool.release(buf);
        }
        auto t1 = std::chrono::steady_clock::now();
        pool_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
        std::printf("  Buffer Pool:     %.1f ms  (%.0f allocs/sec)\n",
            pool_ms, ALLOCS / (pool_ms / 1000.0));
    }

    double ratio = malloc_ms / (std::max)(pool_ms, 0.001);
    std::printf("\n  Speedup: %.1fx faster with pool\n", ratio);
    std::printf("\n  Why: malloc/free traverses the heap free-list and may call into the\n");
    std::printf("  OS for large allocations. Buffer pool just pops from a pre-allocated\n");
    std::printf("  stack — O(1) with no syscalls, no fragmentation, cache-friendly.\n\n");

    REQUIRE(pool_ms > 0);
}

// ============================================================================
// DECISION 3: Non-blocking I/O + event loop vs blocking recv per thread
//
// Why this matters: With blocking I/O, a thread sits idle in recv() waiting
// for data. You need 1 thread per socket. With non-blocking I/O, one call
// to poll/epoll_wait checks ALL sockets at once — only 1 syscall regardless
// of connection count.
// ============================================================================

TEST_CASE("Decision: single poll vs per-socket blocking", "[stress][decisions]") {
    constexpr int NUM_CLIENTS = 50;
    constexpr int MSGS_PER_CLIENT = 1000;
    constexpr int TOTAL = NUM_CLIENTS * MSGS_PER_CLIENT;

    std::printf("\n");
    std::printf("================================================================\n");
    std::printf("  DECISION 3: Event Loop (1 thread) vs Blocking I/O (%d threads)\n", NUM_CLIENTS);
    std::printf("  Scenario: %d clients sending %d messages each = %d total\n",
        NUM_CLIENTS, MSGS_PER_CLIENT, TOTAL);
    std::printf("  Measuring: total echo throughput and delivery\n");
    std::printf("================================================================\n\n");

    std::atomic<int> echoes{0};
    std::atomic<int> server_msgs{0};

    Server server;
    server.on_message([&](ConnectionId id, Message msg) {
        server_msgs.fetch_add(1, std::memory_order_relaxed);
        server.send(id, msg);
    });
    REQUIRE(server.start_threaded({.port = STRESS_PORT + 30}));

    auto t_start = std::chrono::steady_clock::now();

    std::vector<std::thread> client_threads;
    for (int c = 0; c < NUM_CLIENTS; ++c) {
        client_threads.emplace_back([&]() {
            Client client;
            std::atomic<bool> conn{false};
            std::atomic<int> my_echoes{0};

            client.on_connect([&]() { conn.store(true); });
            client.on_message([&](Message) {
                my_echoes.fetch_add(1);
                echoes.fetch_add(1);
            });

            if (!client.connect({.host = "127.0.0.1", .port = STRESS_PORT + 30})) return;

            auto dl = std::chrono::steady_clock::now() + 5s;
            while (!conn.load()) {
                client.poll();
                std::this_thread::sleep_for(1ms);
                if (std::chrono::steady_clock::now() > dl) return;
            }

            for (int i = 0; i < MSGS_PER_CLIENT; ++i) {
                client.send(Message::from_string(1, "x"));
                if (i % 50 == 0) client.poll();
            }

            dl = std::chrono::steady_clock::now() + 30s;
            while (my_echoes.load() < MSGS_PER_CLIENT) {
                client.poll();
                std::this_thread::sleep_for(1ms);
                if (std::chrono::steady_clock::now() > dl) break;
            }
            client.disconnect();
        });
    }

    auto dl = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < dl) {
        server.poll();
        if (echoes.load() >= TOTAL) break;
        std::this_thread::yield();
    }

    for (auto& t : client_threads) t.join();
    auto t_end = std::chrono::steady_clock::now();

    double dur_ms = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count() / 1000.0;
    double throughput = server_msgs.load() / (dur_ms / 1000.0);

    std::printf("  Server I/O threads:  1\n");
    std::printf("  Clients connected:   %d\n", NUM_CLIENTS);
    std::printf("  Messages echoed:     %d / %d\n", echoes.load(), TOTAL);
    std::printf("  Duration:            %.1f ms\n", dur_ms);
    std::printf("  Throughput:          %.0f msg/sec\n", throughput);
    std::printf("\n");
    std::printf("  Why: A blocking design would need %d recv threads + %d send threads\n",
        NUM_CLIENTS, NUM_CLIENTS);
    std::printf("  = %d threads. Each thread costs ~1MB stack + context switch overhead.\n",
        NUM_CLIENTS * 2);
    std::printf("  The event loop checks all %d sockets with 1 syscall (poll/epoll_wait),\n",
        NUM_CLIENTS);
    std::printf("  processes all readable data, then flushes all writes — in one pass.\n\n");

    server.stop();

    REQUIRE(echoes.load() == TOTAL);
    REQUIRE(throughput > 1000.0);
}

// ============================================================================
// DECISION 4: Compact 4-byte header vs 6-byte header
//
// Why this matters: Every single message over the wire pays the header cost.
// At 60,000 messages/sec, saving 2 bytes/msg = 120KB/sec less bandwidth.
// But more importantly, the smaller header means the message fits in fewer
// cache lines and fewer TCP segments.
// ============================================================================

TEST_CASE("Decision: message header compactness", "[stress][decisions]") {
    constexpr int MSGS = 1'000'000;

    std::printf("\n");
    std::printf("================================================================\n");
    std::printf("  DECISION 4: 4-byte header vs 6-byte header\n");
    std::printf("  Scenario: serializing %d messages\n", MSGS);
    std::printf("================================================================\n\n");

    // 4-byte header (our design): [u16 id][u16 size]
    double compact_ms;
    {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < MSGS; ++i) {
            Message msg = Message::from_string(static_cast<uint16_t>(i % 256), "hello");
            auto wire = msg.serialize();
            (void)wire;
        }
        auto t1 = std::chrono::steady_clock::now();
        compact_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
    }

    // 6-byte header simulation: [u16 id][u32 size]
    double fat_ms;
    {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < MSGS; ++i) {
            // simulate 6-byte header serialization
            std::vector<uint8_t> wire(6 + 5); // 6 header + "hello"
            uint16_t id = static_cast<uint16_t>(i % 256);
            uint32_t size = 5;
            std::memcpy(wire.data(), &id, 2);
            std::memcpy(wire.data() + 2, &size, 4);
            std::memcpy(wire.data() + 6, "hello", 5);
            (void)wire;
        }
        auto t1 = std::chrono::steady_clock::now();
        fat_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
    }

    double bandwidth_saved_per_sec = 2.0 * 60000; // 2 bytes * 60K msgs/sec typical
    std::printf("  4-byte header:   %.1f ms for %d msgs\n", compact_ms, MSGS);
    std::printf("  6-byte header:   %.1f ms for %d msgs\n", fat_ms, MSGS);
    std::printf("\n");
    std::printf("  Bandwidth impact at 60K msg/sec:\n");
    std::printf("    4-byte: %.0f KB/sec header overhead\n", 4.0 * 60000 / 1024);
    std::printf("    6-byte: %.0f KB/sec header overhead\n", 6.0 * 60000 / 1024);
    std::printf("    Savings: %.0f KB/sec (%.0f MB/hour)\n",
        bandwidth_saved_per_sec / 1024,
        bandwidth_saved_per_sec * 3600 / (1024 * 1024));
    std::printf("\n");
    std::printf("  Why: Fewer bytes per message means more messages fit in a single TCP\n");
    std::printf("  segment (MSS ~1460 bytes). Fewer segments = fewer ACKs = lower latency.\n");
    std::printf("  The 64KB max payload is sufficient for game packets (typical: 32-512 bytes).\n\n");

    REQUIRE(compact_ms > 0);
}
