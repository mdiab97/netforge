#include <catch2/catch_test_macros.hpp>
#include <netforge/buffer_pool.hpp>

#include <set>
#include <vector>

using namespace netforge;

TEST_CASE("BufferPool initial allocation", "[buffer_pool]") {
    BufferPool pool(64);
    REQUIRE(pool.total_allocated() == 64);
    REQUIRE(pool.available() == 64);
}

TEST_CASE("BufferPool acquire returns valid buffer", "[buffer_pool]") {
    BufferPool pool(16);

    Buffer* buf = pool.acquire();
    REQUIRE(buf != nullptr);
    REQUIRE(buf->used == 0);
    REQUIRE(buf->remaining() == kBufferSize);
    REQUIRE(pool.available() == 15);

    pool.release(buf);
    REQUIRE(pool.available() == 16);
}

TEST_CASE("BufferPool acquire returns unique buffers", "[buffer_pool]") {
    BufferPool pool(32);
    std::set<Buffer*> acquired;

    for (int i = 0; i < 32; ++i) {
        Buffer* buf = pool.acquire();
        REQUIRE(buf != nullptr);
        REQUIRE(acquired.find(buf) == acquired.end());
        acquired.insert(buf);
    }

    REQUIRE(pool.available() == 0);

    // Release all
    for (Buffer* buf : acquired) {
        pool.release(buf);
    }
    REQUIRE(pool.available() == 32);
}

TEST_CASE("BufferPool grows when exhausted", "[buffer_pool]") {
    BufferPool pool(4);
    REQUIRE(pool.total_allocated() == 4);

    // Exhaust initial pool
    std::vector<Buffer*> buffers;
    for (int i = 0; i < 4; ++i) {
        buffers.push_back(pool.acquire());
    }
    REQUIRE(pool.available() == 0);

    // This should trigger growth
    Buffer* extra = pool.acquire();
    REQUIRE(extra != nullptr);
    REQUIRE(pool.total_allocated() > 4);

    // Cleanup
    pool.release(extra);
    for (auto* b : buffers) pool.release(b);
}

TEST_CASE("BufferPool release resets buffer on next acquire", "[buffer_pool]") {
    BufferPool pool(8);

    Buffer* buf = pool.acquire();
    buf->used = 1000;
    pool.release(buf);

    Buffer* reacquired = pool.acquire();
    REQUIRE(reacquired->used == 0);
}

TEST_CASE("BufferPool release null is safe", "[buffer_pool]") {
    BufferPool pool(8);
    pool.release(nullptr); // should not crash
    REQUIRE(pool.available() == 8);
}

TEST_CASE("BufferPool buffers are writable at full size", "[buffer_pool]") {
    BufferPool pool(4);
    Buffer* buf = pool.acquire();

    // Write to every byte
    std::memset(buf->data, 0xAB, kBufferSize);
    buf->used = kBufferSize;

    REQUIRE(buf->remaining() == 0);
    REQUIRE(buf->data[0] == 0xAB);
    REQUIRE(buf->data[kBufferSize - 1] == 0xAB);

    pool.release(buf);
}
