#include <catch2/catch_test_macros.hpp>
#include <netforge/buffer_pool.hpp>

using namespace netforge;

TEST_CASE("Buffer basic properties", "[buffer]") {
    Buffer buf;
    REQUIRE(buf.used == 0);
    REQUIRE(buf.remaining() == kBufferSize);
    REQUIRE(buf.write_ptr() == buf.data);
}

TEST_CASE("Buffer write and read", "[buffer]") {
    Buffer buf;
    const char* msg = "Hello";
    std::memcpy(buf.write_ptr(), msg, 5);
    buf.used = 5;

    REQUIRE(buf.used == 5);
    REQUIRE(buf.remaining() == kBufferSize - 5);
    REQUIRE(std::memcmp(buf.read_ptr(), "Hello", 5) == 0);
}

TEST_CASE("Buffer reset clears used count", "[buffer]") {
    Buffer buf;
    buf.used = 100;
    buf.reset();
    REQUIRE(buf.used == 0);
    REQUIRE(buf.remaining() == kBufferSize);
}

TEST_CASE("Buffer size is 4096", "[buffer]") {
    REQUIRE(kBufferSize == 4096);
}
