#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <mutex>

namespace netforge {

static constexpr size_t kBufferSize = 4096; // 4KB fixed buffer

struct Buffer {
    uint8_t data[kBufferSize];
    size_t used{0};

    void reset() { used = 0; }
    size_t remaining() const { return kBufferSize - used; }
    uint8_t* write_ptr() { return data + used; }
    const uint8_t* read_ptr() const { return data; }
};

// Pre-allocated pool of fixed-size buffers. Buffers are recycled after use.
// The pool itself is not lock-free, but allocation/deallocation is infrequent:
// buffers are grabbed in bulk and recycled in bulk between ticks.
class BufferPool {
public:
    explicit BufferPool(size_t initial_count = 256);
    ~BufferPool() = default;

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // Acquire a buffer from the pool. Returns nullptr only if pool is exhausted
    // and growth fails (should never happen in practice since we grow on demand).
    Buffer* acquire();

    // Return a buffer to the pool for reuse.
    void release(Buffer* buf);

    // Stats
    size_t total_allocated() const;
    size_t available() const;

private:
    void grow(size_t count);

    std::vector<std::unique_ptr<Buffer[]>> blocks_;
    std::vector<Buffer*> free_list_;
    mutable std::mutex mutex_;
    size_t total_{0};
};

} // namespace netforge
