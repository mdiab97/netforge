#include <catch2/catch_test_macros.hpp>
#include <netforge/message.hpp>

using namespace netforge;

TEST_CASE("Message header size is 4 bytes", "[message]") {
    REQUIRE(kMessageHeaderSize == 4);
    REQUIRE(Message::header_size() == 4);
}

TEST_CASE("MessageHeader encode/decode roundtrip", "[message]") {
    MessageHeader h{42, 1024};
    uint8_t buf[4];
    h.encode(buf);

    auto decoded = MessageHeader::decode(buf);
    REQUIRE(decoded.id == 42);
    REQUIRE(decoded.size == 1024);
}

TEST_CASE("Message serialize and deserialize", "[message]") {
    Message msg = Message::from_string(1, "Hello, World!");

    REQUIRE(msg.id() == 1);
    REQUIRE(msg.size() == 13);
    REQUIRE(msg.as_string() == "Hello, World!");

    auto wire = msg.serialize();
    REQUIRE(wire.size() == kMessageHeaderSize + 13);

    Message restored;
    REQUIRE(Message::deserialize(wire.data(), wire.size(), restored));
    REQUIRE(restored.id() == 1);
    REQUIRE(restored.size() == 13);
    REQUIRE(restored.as_string() == "Hello, World!");
}

TEST_CASE("Message with empty payload", "[message]") {
    std::vector<uint8_t> empty;
    Message msg(100, std::move(empty));

    REQUIRE(msg.id() == 100);
    REQUIRE(msg.size() == 0);

    auto wire = msg.serialize();
    REQUIRE(wire.size() == kMessageHeaderSize);

    Message restored;
    REQUIRE(Message::deserialize(wire.data(), wire.size(), restored));
    REQUIRE(restored.id() == 100);
    REQUIRE(restored.size() == 0);
}

TEST_CASE("Message with binary payload", "[message]") {
    std::vector<uint8_t> payload = {0x00, 0xFF, 0x42, 0x13, 0x37};
    Message msg(7, payload);

    auto wire = msg.serialize();
    Message restored;
    REQUIRE(Message::deserialize(wire.data(), wire.size(), restored));
    REQUIRE(restored.id() == 7);
    REQUIRE(restored.payload() == payload);
}

TEST_CASE("Deserialize fails on truncated header", "[message]") {
    uint8_t buf[2] = {0, 0};
    Message msg;
    REQUIRE_FALSE(Message::deserialize(buf, 2, msg));
}

TEST_CASE("Deserialize fails on truncated payload", "[message]") {
    // Encode a header claiming 100 bytes of payload, but only provide header
    MessageHeader h{1, 100};
    uint8_t buf[4];
    h.encode(buf);

    Message msg;
    REQUIRE_FALSE(Message::deserialize(buf, 4, msg));
}

TEST_CASE("Multiple messages can be parsed sequentially from a stream", "[message]") {
    Message m1 = Message::from_string(1, "First");
    Message m2 = Message::from_string(2, "Second");
    Message m3 = Message::from_string(3, "Third");

    auto w1 = m1.serialize();
    auto w2 = m2.serialize();
    auto w3 = m3.serialize();

    // Concatenate into a stream
    std::vector<uint8_t> stream;
    stream.insert(stream.end(), w1.begin(), w1.end());
    stream.insert(stream.end(), w2.begin(), w2.end());
    stream.insert(stream.end(), w3.begin(), w3.end());

    // Parse them back
    size_t offset = 0;
    for (int expected_id = 1; expected_id <= 3; ++expected_id) {
        REQUIRE(offset + kMessageHeaderSize <= stream.size());
        auto hdr = MessageHeader::decode(stream.data() + offset);
        size_t total = kMessageHeaderSize + hdr.size;
        REQUIRE(offset + total <= stream.size());

        Message msg;
        REQUIRE(Message::deserialize(stream.data() + offset, total, msg));
        REQUIRE(msg.id() == expected_id);
        offset += total;
    }

    REQUIRE(offset == stream.size());
}
