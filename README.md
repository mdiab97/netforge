# NetForge

High-performance event-loop networking library for game servers, built with modern C++20.

Designed for MMO/game server workloads: one I/O thread handles thousands of connections with lock-free message passing between the network layer and game logic.

## Features

- **Event-loop architecture** — single I/O thread serves all connections (no thread-per-connection)
- **Lock-free SPSC queues** — zero-mutex communication between game thread and I/O thread
- **Buffer pooling** — pre-allocated 4KB buffers, no allocation on hot path
- **Compact wire format** — 4-byte header (`[u16 id][u16 size]`), max 64KB payload
- **Non-blocking I/O** — all sockets non-blocking, driven by platform event loop
- **Cross-platform** — Windows (WSAPoll) and Linux (epoll)
- **Zero external dependencies** — no Boost, no ASIO, just platform APIs

## Performance

Each architectural decision is benchmarked. Run `netforge_stress_test` to reproduce.

### Decision Benchmarks

**Lock-free SPSC queue vs mutex queue** (1M items, 1 producer + 1 consumer):
```
Mutex Queue:       3.8M ops/sec
Lock-Free SPSC:   12.1M ops/sec  (3.2x faster)
```
Why: The game thread and I/O thread exchange messages every tick. A mutex means the game thread stalls waiting for the network thread to release the lock — causing frame hitches. Lock-free means zero contention.

**Buffer pool vs malloc/free** (500K allocation cycles, 4KB each):
```
malloc/free:       5.3M allocs/sec
Buffer Pool:       7.4M allocs/sec  (1.4x faster)
```
Why: Every received message needs a buffer. At 60K msgs/sec, that's 60K heap allocations per second. malloc traverses the free-list and may syscall. The pool just pops from a pre-allocated stack — O(1), no fragmentation.

**Event-loop I/O vs blocking I/O** (50 clients, 50K echo messages):
```
Event loop:        1 I/O thread, 14,700 msg/sec, all 50K messages delivered
Blocking:          Would require 100 threads (2 per connection)
```
Why: Blocking recv() wastes a thread sitting idle waiting for data. The event loop checks all 50 sockets with 1 syscall (poll/epoll_wait), processes all readable data, then flushes all writes in one pass.

**4-byte header vs 6-byte header** (bandwidth at 60K msgs/sec):
```
4-byte [u16 id][u16 size]:  234 KB/sec header overhead
6-byte [u16 id][u32 size]:  352 KB/sec header overhead
Savings: 412 MB/hour
```
Why: Smaller headers mean more messages fit per TCP segment (MSS ~1460 bytes). Fewer segments = fewer ACKs = lower latency. 64KB max payload covers all game packets (typical: 32-512 bytes).

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `NETFORGE_BUILD_TESTS` | ON | Build unit tests (fetches Catch2) |
| `NETFORGE_BUILD_EXAMPLES` | ON | Build example programs |
| `NETFORGE_ENABLE_TLS` | OFF | Enable TLS support (requires OpenSSL) |

### Running Tests

```bash
cd build

# Unit + integration tests (fast)
ctest --test-dir tests --build-config Debug

# Stress test with benchmarks
./tests/Debug/netforge_stress_test
```

## Quick Start

### Server

```cpp
#include <netforge/netforge.hpp>
#include <cstdio>
#include <thread>

int main() {
    netforge::net_init();

    netforge::Server server;

    server.on_connect([](netforge::ConnectionId id) {
        std::printf("client %llu connected\n", (unsigned long long)id);
    });

    server.on_message([&](netforge::ConnectionId id, netforge::Message msg) {
        server.send(id, msg); // echo back
    });

    server.start({.port = 9000});

    // Game loop
    while (true) {
        server.poll(); // fires callbacks on game thread
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    server.stop();
    netforge::net_cleanup();
}
```

### Client

```cpp
#include <netforge/netforge.hpp>
#include <cstdio>

int main() {
    netforge::net_init();

    netforge::Client client;

    client.on_connect([]() { std::printf("connected\n"); });
    client.on_message([](netforge::Message msg) {
        std::printf("received: %s\n", msg.as_string().c_str());
    });

    client.connect({.host = "127.0.0.1", .port = 9000});

    client.send(netforge::Message::from_string(1, "hello"));

    // Poll to receive responses
    while (true) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    client.disconnect();
    netforge::net_cleanup();
}
```

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                   Game Thread                         │
│  (game logic, AI, physics, calls server.poll())      │
└────────────────────────┬─────────────────────────────┘
                         │ Lock-free SPSC Queues
                         │ (incoming events / outgoing commands)
┌────────────────────────▼─────────────────────────────┐
│                   I/O Thread (1)                      │
│  Event Loop (WSAPoll / epoll)                        │
│  Handles: accept, read, write for ALL connections    │
│  Non-blocking sockets, write batching                │
├──────────────────────────────────────────────────────┤
│  Connection Pool         │  Buffer Pool (4KB chunks) │
│  (state structs, no      │  (pre-allocated, recycled │
│   threads per conn)      │   zero malloc on hot path)│
└──────────────────────────────────────────────────────┘
```

### Wire Format

```
[uint16_t message_id][uint16_t payload_size][payload bytes...]
 ◄──── 4 byte header ────►
```

Max payload: 65,535 bytes per message.

### Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Single I/O thread | Avoids context-switch overhead of thread-per-connection; scales to 10K+ connections |
| Lock-free SPSC queue | Zero mutex contention between game and network threads |
| Buffer pool | Eliminates malloc/free on send/receive hot path |
| Non-blocking + event loop | One syscall (poll/epoll_wait) checks all sockets at once |
| 4-byte compact header | Minimal overhead; 64KB max keeps memory predictable |
| `poll()` on game thread | Callbacks fire on the game thread — no synchronization needed in handlers |

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for full design documentation.

## Examples

- **echo_server / echo_client** — minimal echo service
- **chat_server / chat_client** — multi-user chat room with nick support

## Project Structure

```
NetForge/
├── include/netforge/     # Public API headers
│   ├── server.hpp        # Event-loop server
│   ├── client.hpp        # Poll-based client
│   ├── message.hpp       # 4-byte header message format
│   ├── connection.hpp    # Connection state struct
│   ├── event_loop.hpp    # Platform I/O multiplexer
│   ├── spsc_queue.hpp    # Lock-free ring buffer
│   ├── buffer_pool.hpp   # Fixed-size buffer recycling
│   └── transport.hpp     # Socket primitives
├── src/                  # Implementations
├── tests/                # Catch2 unit + stress tests
├── examples/             # Echo and chat demos
└── docs/                 # Architecture documentation
```

## License

MIT
