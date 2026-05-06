#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace netforge {

// Wire format: [uint16_t message_id][uint16_t payload_size][payload...]
// Total header: 4 bytes. Max payload: 65535 bytes.
static constexpr size_t kMessageHeaderSize = 4;
static constexpr size_t kMaxPayloadSize = 65535;

struct MessageHeader {
    uint16_t id{0};
    uint16_t size{0};

    void encode(uint8_t* dst) const {
        std::memcpy(dst, &id, 2);
        std::memcpy(dst + 2, &size, 2);
    }

    static MessageHeader decode(const uint8_t* src) {
        MessageHeader h;
        std::memcpy(&h.id, src, 2);
        std::memcpy(&h.size, src + 2, 2);
        return h;
    }
};

class Message {
public:
    Message() = default;

    Message(uint16_t id, const void* payload, size_t len)
        : id_(id), payload_(static_cast<const uint8_t*>(payload),
                            static_cast<const uint8_t*>(payload) + len) {}

    Message(uint16_t id, std::vector<uint8_t> payload)
        : id_(id), payload_(std::move(payload)) {}

    static Message from_string(uint16_t id, const std::string& str) {
        return Message(id, str.data(), str.size());
    }

    uint16_t id() const { return id_; }
    size_t size() const { return payload_.size(); }
    const uint8_t* data() const { return payload_.data(); }

    const std::vector<uint8_t>& payload() const { return payload_; }

    std::string as_string() const {
        return std::string(reinterpret_cast<const char*>(payload_.data()), payload_.size());
    }

    // Serialize to wire format (header + payload)
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(kMessageHeaderSize + payload_.size());
        MessageHeader h{id_, static_cast<uint16_t>(payload_.size())};
        h.encode(out.data());
        if (!payload_.empty()) {
            std::memcpy(out.data() + kMessageHeaderSize, payload_.data(), payload_.size());
        }
        return out;
    }

    // Deserialize from wire bytes. Returns true if successful.
    static bool deserialize(const uint8_t* data, size_t len, Message& out) {
        if (len < kMessageHeaderSize) return false;
        auto h = MessageHeader::decode(data);
        if (h.size > kMaxPayloadSize) return false;
        if (len < kMessageHeaderSize + h.size) return false;
        out.id_ = h.id;
        out.payload_.assign(data + kMessageHeaderSize, data + kMessageHeaderSize + h.size);
        return true;
    }

    static constexpr size_t header_size() { return kMessageHeaderSize; }

private:
    uint16_t id_{0};
    std::vector<uint8_t> payload_;
};

} // namespace netforge
