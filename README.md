# NetForge

High-performance event-loop networking library for game servers, built with modern C++20.

Designed for MMO/game server workloads: one I/O thread handles thousands of connections with lock-free message passing between the network layer and game logic.

## Features

- **Event-loop architecture** — single I/O thread serves all connections (no thread-per-connection)
- **User-driven threading** — tick functions you call from your own threads, or auto-threaded convenience mode
- **Lock-free SPSC queues** — zero-mutex communication between I/O, processing, and game threads
- **Game-oriented encryption** — X25519 key exchange + ChaCha20-Poly1305 per-packet AEAD (no TLS overhead)
- **Buffer pooling** — pre-allocated 4KB buffers, no allocation on hot path
- **Compact wire format** — 4-byte header (`[u16 id][u16 size]`), max 64KB payload
- **Non-blocking I/O** — all sockets non-blocking, driven by platform event loop
- **Cross-platform** — Windows (WSAPoll) and Linux (epoll)
- **Minimal dependencies** — only [Monocypher](https://monocypher.org/) (vendored, single .c file, public domain)

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

### Server (auto-threaded)

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
        server.send(id, msg);
    });

    server.start_threaded({.port = 9000}); // spawns I/O + processing threads

    while (true) {
        server.poll(); // fires callbacks on game thread
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
```

### Server (manual threading)

```cpp
netforge::Server server;
// ... set callbacks ...
server.start({.port = 9000}); // setup only, no threads

// You control the threads:
std::thread io([&] { while (running) server.tick_io(); });
std::thread proc([&] { while (running) server.tick_process(); });

// Game loop on main thread:
while (running) {
    server.poll();
    game_update();
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

Three tick functions, three SPSC queues, zero shared mutable state. You choose the threading.

```
server.tick_io()         server.tick_process()       server.poll()
(pure socket I/O,        (message parsing,           (fire callbacks,
 event loop)              deserialization)             game logic)
     |                        |                        |
     |-- outgoing_queue_ <---|<------------------------|
     |--- raw_queue_ ------->|                         |
     |                        |--- incoming_queue_ --->|
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
| User-driven threading (tick functions) | No hidden threads. You call `tick_io()`, `tick_process()`, `poll()` from whatever threads you want. Integrate into any architecture |
| Separate I/O and processing | I/O stays lean (pure syscalls). Processing handles deserialization, encryption/decryption |
| X25519 + ChaCha20-Poly1305 | Game-oriented crypto: 1 round-trip handshake, per-packet AEAD, no certificates. TLS is too heavy for game traffic |
| Lock-free SPSC queues | Zero mutex contention between all three threads |
| Buffer pool | Eliminates malloc/free on send/receive hot path |
| Non-blocking + event loop | One syscall (poll/epoll_wait) checks all sockets at once |
| 4-byte compact header | Minimal overhead; 64KB max keeps memory predictable |
| `poll()` on game thread | Callbacks fire on the game thread — no synchronization needed in handlers |
| SharedData for broadcast | One serialized copy shared across all write queues, not N copies |

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for full design documentation.

## Encryption

Game-oriented cryptography — no TLS, no certificates, no multi-round-trip handshakes.

**Handshake (1 round-trip):**
```
Client                          Server
  |--- ClientHello (pubkey) --->|
  |<-- ServerHello (pubkey) ----|
  Both derive shared key via X25519 + BLAKE2b
```

**Per-packet encryption:**
```
[8 bytes nonce counter][16 bytes MAC][N bytes ciphertext]
```

- **X25519** Diffie-Hellman key exchange — ephemeral keys, no certificates
- **ChaCha20-Poly1305** AEAD — authenticated encryption, tamper-proof
- **Monotonic nonce counter** — prevents replay attacks
- **24 bytes overhead** per encrypted packet (8 nonce + 16 MAC)
- **Powered by Monocypher** — single .c file, public domain, constant-time

Why not TLS: TLS adds 2-3 round-trip handshakes, requires certificate infrastructure, encrypts the entire TCP stream (can't selectively encrypt), and adds unnecessary latency for game packets on trusted server infrastructure.

## Examples

- **echo_server / echo_client** — minimal echo service
- **chat_server / chat_client** — multi-user chat room with nick support

## Project Structure

```
NetForge/
├── include/netforge/        # Public API headers
│   ├── server.hpp           # Event-loop server with tick functions
│   ├── client.hpp           # Poll-based client
│   ├── message.hpp          # 4-byte header message format
│   ├── connection.hpp       # Connection state struct
│   ├── event_loop.hpp       # Platform I/O multiplexer
│   ├── spsc_queue.hpp       # Lock-free ring buffer
│   ├── buffer_pool.hpp      # Fixed-size buffer recycling
│   ├── transport.hpp        # Socket primitives
│   └── crypto/
│       ├── key_exchange.hpp # X25519 keypair generation and shared key derivation
│       ├── packet_cipher.hpp# ChaCha20-Poly1305 per-packet AEAD
│       └── handshake.hpp    # 1 round-trip key exchange protocol
├── third_party/monocypher/  # Vendored crypto primitives (public domain)
├── src/                     # Implementations
├── tests/                   # Catch2 unit + stress + crypto tests
├── examples/                # Echo and chat demos
└── docs/                    # Architecture documentation
```

## License

MIT
