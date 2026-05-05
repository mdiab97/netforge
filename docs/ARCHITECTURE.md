# NetForge v2 Architecture

## Overview

NetForge v2 is a complete rewrite focused on high-performance game/MMO server networking. The core design replaces the old thread-per-connection model with a single I/O thread driving an event loop, communicating with the game thread via lock-free queues.

## Why Thread-Per-Connection Was Bad

The v1 architecture spawned 2 threads per connected client (one for reading, one for writing). This approach:

- **Doesn't scale**: 10,000 connections = 20,000 threads. Context switching dominates CPU time.
- **Wastes memory**: Each thread stack is 1-8MB. 20K threads = 20-160GB of virtual address space.
- **Introduces contention**: Shared state between threads requires mutexes, causing cache line bouncing.
- **Unpredictable latency**: OS thread scheduling adds jitter that makes game tick timing unreliable.
- **Makes batching impossible**: Each connection writes independently, preventing scatter-gather optimizations.

## The Event Loop Model

The new architecture uses ONE I/O thread for ALL connections:

```
Game Thread                    I/O Thread
    |                              |
    |--- outgoing queue --------->|
    |    (lock-free SPSC)         |
    |                              |--- event loop (select/epoll)
    |<--- incoming queue ---------|      handles ALL sockets
    |    (lock-free SPSC)         |
    |                              |
    v                              v
 poll() fires callbacks       non-blocking read/write
```

**Why this works**: A single thread with non-blocking I/O can handle tens of thousands of connections because most sockets are idle at any given moment. The kernel tells us exactly which sockets have data ready, so we never waste time blocking.

## Platform Differences: IOCP vs epoll

### Linux (epoll)
- Edge-triggered (`EPOLLET`): kernel only notifies on state *changes*
- Must drain all available data when notified (loop until `EAGAIN`)
- Single `epoll_wait()` call returns all ready file descriptors
- Zero-copy with `splice()` possible for advanced use cases

### Windows (select / future IOCP)
- Current implementation uses `select()` for portability and simplicity
- `select()` handles FD_SETSIZE (64 by default on Windows, but sufficient for thousands with careful management)
- Future upgrade path: full IOCP with overlapped I/O for maximum Windows performance
- IOCP is completion-based (notifies after I/O completes) vs epoll which is readiness-based (notifies when ready)

The abstraction layer (`EventLoop`) hides these differences behind a simple interface:
- `add(socket, user_data)` — register interest
- `set_writable(socket, bool)` — toggle write notifications
- `poll(events, timeout)` — wait for I/O readiness

## Buffer Pool Design

The buffer pool eliminates malloc/free on the hot path:

```
BufferPool
  |
  |-- blocks_: vector<unique_ptr<Buffer[]>>  (owns memory)
  |-- free_list_: vector<Buffer*>            (available buffers)
  |
  acquire() -> Buffer*  (pop from free_list, grow if empty)
  release(Buffer*)      (push back to free_list)
```

**Key properties:**
- Fixed 4KB buffer size matches common MTU multiples
- Memory is pre-allocated in chunks (grows by 64 buffers at a time)
- Never shrinks — once allocated, buffers stay in the pool forever
- Mutex-protected but rarely contended (buffers are held for full message lifetime)
- `reset()` clears the `used` counter without zeroing memory

**Why 4KB?** It's the page size on most architectures, aligns with typical TCP segment sizes, and is large enough for most game messages while small enough to not waste memory.

## Lock-Free SPSC Queue

The `SpscQueue` is a bounded ring buffer with exactly one producer thread and one consumer thread. No locks needed.

```
          head (written by producer)
            v
 [  ][  ][ A][ B][ C][  ][  ][  ]
                        ^
                       tail (written by consumer)
```

**How it works:**
1. Producer advances `head` after writing an element
2. Consumer advances `tail` after reading an element
3. `std::memory_order_release` on writes ensures the data is visible before the index update
4. `std::memory_order_acquire` on reads ensures we see the latest data
5. Power-of-two size enables fast modulo via bitmask: `index & (capacity - 1)`

**Why SPSC and not MPMC?** In our architecture, there's exactly one producer (I/O thread) and one consumer (game thread) for incoming events, and vice versa for outgoing commands. SPSC is simpler, faster, and provably correct without CAS loops.

**Cache line padding:** `head` and `tail` are on separate cache lines (64-byte aligned) to prevent false sharing between producer and consumer cores.

## Write Batching

When the game thread calls `send()` or `broadcast()`, messages are not sent immediately. Instead:

1. Message is serialized and pushed to the outgoing SPSC queue
2. On the next I/O thread iteration, all queued commands are drained
3. Messages are appended to each connection's `write_queue`
4. The event loop flushes write queues in bulk

This naturally batches writes. If the game thread sends 50 messages to a client during one tick, they all get queued and flushed together, potentially in a single `send()` syscall.

**Partial writes:** If `send()` returns less than the full buffer (kernel buffer full), we track the offset and re-enable write notifications. On the next writable event, we continue from where we left off.

## Compact Message Format

```
 0      1      2      3      4 ... N
[  msg_id  ][ payload_size ][ payload ]
  uint16      uint16          N bytes
```

- **4 bytes total header** — minimal overhead for small messages
- **Max 64KB payload** — sufficient for all game messages; larger data should be chunked
- **No length-prefix ambiguity** — size is explicit, no delimiters to parse
- **Network byte order not used** — both sides are assumed to be the same architecture (games control both client and server). This avoids unnecessary byte-swap overhead.

## Connection as State Struct

A `Connection` is purely data — no threads, no async operations, no virtual methods:

```cpp
struct Connection {
    ConnectionId id;
    socket_t socket;
    ConnectionState state;
    vector<uint8_t> read_buf;   // accumulate incoming bytes
    size_t read_pos;            // cursor into read_buf
    deque<vector<uint8_t>> write_queue;  // pending outgoing data
    size_t write_offset;        // partial send progress
};
```

The event loop drives all I/O by operating on these structs. This makes the system:
- Cache-friendly (data locality)
- Easy to reason about (no hidden state transitions)
- Simple to debug (inspect any connection's state at any point)

## Handling Partial Reads and Writes

Non-blocking sockets may read/write fewer bytes than requested. The system handles this correctly:

**Reads:** Bytes accumulate in `read_buf` at position `read_pos`. The parser scans from the beginning looking for complete messages (header + full payload). Parsed messages are removed, remaining bytes are compacted to the front.

**Writes:** The front of `write_queue` tracks `write_offset`. If a send is partial, we remember where we stopped. On the next writable event, we continue from that offset. When a buffer is fully sent, it's popped and we move to the next.

## Performance Characteristics

| Metric | Expected Range |
|--------|---------------|
| Connections | 10,000+ on single thread |
| Throughput | 100,000+ msgs/sec (small messages, localhost) |
| Latency | Sub-millisecond on localhost |
| Memory per connection | ~4KB read buffer + write queue |
| Syscalls per tick | 1 poll + N send/recv (batched) |
| Lock contention | Zero (SPSC queues are lock-free) |

## Thread Safety Model

- **Game thread**: Calls `poll()`, `send()`, `broadcast()`, registers callbacks
- **I/O thread**: Handles all socket operations, never touches game state
- **Communication**: Exclusively via SPSC queues (no shared mutable state)
- **Callbacks**: Always fire on the game thread (inside `poll()`)

This means game logic never needs locks for networking operations.
