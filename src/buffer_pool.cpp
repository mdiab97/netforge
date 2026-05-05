#include "netforge/buffer_pool.hpp"

namespace netforge {

BufferPool::BufferPool(size_t initial_count) {
    grow(initial_count);
}

Buffer* BufferPool::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_list_.empty()) {
        grow(64); // grow in chunks
    }
    Buffer* buf = free_list_.back();
    free_list_.pop_back();
    buf->reset();
    return buf;
}

void BufferPool::release(Buffer* buf) {
    if (!buf) return;
    std::lock_guard<std::mutex> lock(mutex_);
    free_list_.push_back(buf);
}

size_t BufferPool::total_allocated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_;
}

size_t BufferPool::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return free_list_.size();
}

void BufferPool::grow(size_t count) {
    auto block = std::make_unique<Buffer[]>(count);
    for (size_t i = 0; i < count; ++i) {
        free_list_.push_back(&block[i]);
    }
    blocks_.push_back(std::move(block));
    total_ += count;
}

} // namespace netforge
