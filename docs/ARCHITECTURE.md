# NetForge Architecture

## Overview

NetForge is a game/MMO networking library with a 3-thread architecture: an I/O thread for pure socket operations, a processing thread for message parsing and deserialization, and the game thread for application logic. All communication between threads uses lock-free SPSC queues with zero shared mutable state.

## Threading Model

NetForge does not own any threads. It exposes three tick functions that the user calls from whatever threads they choose:

```cpp
server.start(config);          // setup only, no threads spawned

server.tick_io(timeout_ms);    // call from your I/O thread
server.tick_process();         // call from your processing thread
server.poll();                 // call from your game thread
```

This lets you:
- Run all three on a single thread (simple games, debugging)
- Split I/O and processing across two threads
- Dedicate a thread per function with custom affinity/priority
- Integrate into an existing thread pool or job system

A convenience method `start_threaded()` spawns I/O and processing threads internally for simple use cases.

### Data Flow

```
Game Thread              I/O Thread              Processing Thread
(poll, send,             (tick_io)               (tick_process)
 broadcast)
     |                        |                        |
     |-- outgoing_queue_ --->|                        |
     |   (serialized data)   |--- raw_queue_ ------->|
     |                        |   (conn_id + bytes)   |
     |<----------------- incoming_queue_ -------------|
         (parsed Messages)
```

### tick_io()

Pure socket operations — no parsing, no deserialization. Stays as lean as possible to minimize latency between the kernel and userspace.

- `event_loop_.poll()` — check all sockets for readability/writability (WSAPoll/epoll)
- `handle_read()` — `recv()` raw bytes into a temporary buffer, push to `raw_queue_`
- `handle_write()` — `send()` from per-connection write queues
- `handle_accept()` — accept new connections, register with event loop
- `drain_outgoing()` — pop serialized data from `outgoing_queue_`, enqueue to write queues
- `disconnect()` — close socket, signal processing thread

### tick_process()

Owns per-connection read buffers. Handles the heavy work that would otherwise block the I/O thread.

- Pop `RawEvent` from `raw_queue_`
- Maintain `ReadBuffer` per connection (accumulate partial data, track parse position)
- Scan for complete messages (4-byte header check)
- Deserialize payloads into `Message` objects
- Push parsed `IncomingEvent` to `incoming_queue_`
- Flush remaining data on disconnect
- Future: encryption/decryption, compression, validation

### poll()

Game thread entry point. Pops parsed events and fires user callbacks.

- `server.poll()` — pops from `incoming_queue_`, fires `on_connect`, `on_message`, `on_disconnect`
- `server.send(id, msg)` — serializes and pushes to `outgoing_queue_`
- `server.broadcast(msg)` — same, I/O thread fans out to all connections

## Thread Ownership

| Thread | Owns |
|--------|------|
| I/O Thread | EventLoop, Connection map (sockets + write queues), listen socket |
| Processing Thread | ReadBuffer map (per-connection read state), deserialization |
| Game Thread | Callbacks, game state, send()/broadcast() API |

## SPSC Queues

All inter-thread communication uses lock-free single-producer single-consumer ring buffers. No mutexes on the hot path.

| Queue | Producer | Consumer | Carries |
|-------|----------|----------|---------|
| `outgoing_queue_` | Game Thread | I/O Thread | Serialized bytes (send/broadcast commands) |
| `raw_queue_` | I/O Thread | Processing Thread | Raw received bytes + connect/disconnect signals |
| `incoming_queue_` | Processing Thread | Game Thread | Parsed Messages + connect/disconnect events |

### Why SPSC

Each queue has exactly one writer and one reader. SPSC is simpler, faster, and provably correct without CAS loops. Power-of-two capacity enables fast modulo via bitmask. Head and tail are on separate cache lines (64-byte aligned) to prevent false sharing.

### Memory Ordering

- `memory_order_release` on writes ensures data is visible before the index update
- `memory_order_acquire` on reads ensures we see the latest data
- No stronger ordering needed because each queue has exactly one producer and one consumer

## Platform Differences

### Linux (epoll)
- Edge-triggered (`EPOLLET`): kernel only notifies on state *changes*
- Must drain all available data when notified (loop until `EAGAIN`)
- Single `epoll_wait()` call returns all ready file descriptors

### Windows (WSAPoll)
- Level-triggered: notifies whenever socket is ready
- Socket entries stored in a hash map for O(1) lookup on `set_writable()`
- Cached WSAPOLLFD array rebuilt only on add/remove, events updated inline

The `EventLoop` abstraction hides these differences:
- `add(socket, user_data)` — register interest
- `set_writable(socket, bool)` — toggle write notifications
- `poll(events, timeout)` — wait for I/O readiness

## Buffer Pool

Pre-allocated 4KB buffers eliminate malloc/free on the hot path:

```
BufferPool
  |-- blocks_: vector<unique_ptr<Buffer[]>>  (owns memory)
  |-- free_list_: vector<Buffer*>            (available buffers)
  |
  acquire() -> Buffer*  (pop from free_list, grow if empty)
  release(Buffer*)      (push back to free_list)
```

- Fixed 4KB size matches page size and common MTU multiples
- Grows in chunks of 64, never shrinks
- `reset()` clears the used counter without zeroing memory

## Write Batching

When the game thread calls `send()` or `broadcast()`, messages are not sent immediately:

1. Message is serialized and pushed to `outgoing_queue_`
2. I/O thread drains all queued commands in `drain_outgoing()`
3. Messages are appended to each connection's write queue
4. The event loop flushes write queues when the socket is writable

Broadcast uses `SharedData` (`shared_ptr<vector<uint8_t>>`) so one serialized copy is shared across all connection write queues instead of being copied N times.

## Backpressure

Slow clients (write queue exceeding 256 entries) are skipped during broadcast. This prevents one slow connection from causing unbounded memory growth across the server.

## Compact Message Format

```
 0      1      2      3      4 ... N
[  msg_id  ][ payload_size ][ payload ]
  uint16      uint16          N bytes
```

- **4 bytes total header** — minimal overhead for game messages
- **Max 64KB payload** — sufficient for all game packets; larger data should be chunked
- **Native byte order** — both sides assumed same architecture (games control client and server)

## Encryption

### Why Not TLS

TLS is designed for HTTP — it adds 2-3 round-trip handshakes, requires certificate infrastructure, encrypts the entire TCP stream (no selective encryption), and adds per-record overhead that's unnecessary for game packets on trusted infrastructure. Games need fast key exchange and per-packet authenticated encryption.

### Handshake: X25519 Key Exchange

One round-trip. No certificates. Both sides generate ephemeral Curve25519 keypairs and exchange public keys. The shared secret is derived via X25519 Diffie-Hellman and then hashed through BLAKE2b to produce a uniform 32-byte session key.

```
Client                          Server
  |                                |
  |--- ClientHello [01][pubkey] -->|
  |                                | generate keypair
  |                                | derive shared key = BLAKE2b(X25519(server_sk, client_pk))
  |<-- ServerHello [02][pubkey] ---|
  |                                |
  | derive shared key = BLAKE2b(X25519(client_sk, server_pk))
  |                                |
  Both have the same 32-byte session key.
```

Message format: `[1 byte type][32 bytes public key]` = 33 bytes per handshake message.

### Packet Encryption: ChaCha20-Poly1305

Each message is encrypted independently using AEAD (Authenticated Encryption with Associated Data). No dependency chain between packets — the server can decrypt any message without state from previous messages.

```
Encrypted packet format:
[8 bytes nonce counter][16 bytes Poly1305 MAC][N bytes ciphertext]
```

- **ChaCha20** stream cipher for encryption — fast on all hardware including ARM/mobile
- **Poly1305** MAC for authentication — detects any tampering
- **Monotonic nonce counter** — prevents replay attacks (out-of-order packets rejected)
- **24 bytes overhead** per packet (8 nonce + 16 MAC)
- **Constant-time operations** — no timing side-channels

### Why ChaCha20 Over AES

- Faster than AES on hardware without AES-NI instructions (mobile, older CPUs, ARM)
- Constant-time without hardware support (AES lookup tables leak timing information)
- Simpler implementation, fewer attack vectors
- Used by WireGuard, SSH, Google QUIC, TLS 1.3

### Crypto Library: Monocypher

Monocypher is vendored as a single .c/.h file (public domain). No external dependency, no build complexity. It provides X25519, ChaCha20-Poly1305, BLAKE2b, and secure memory wiping — exactly what we need, nothing more.

## Connection Lifecycle

1. I/O thread accepts socket, creates `Connection` (socket + write queue), pushes `RawEvent::Connect` to `raw_queue_`
2. Processing thread creates `ReadBuffer`, pushes `IncomingEvent::Connect` to `incoming_queue_`
3. Game thread receives `on_connect(id)` callback
4. Data flows: I/O recv -> raw_queue -> processing parse -> incoming_queue -> game callback
5. On disconnect: I/O closes socket, pushes `RawEvent::Disconnect`, processing flushes remaining data, pushes `IncomingEvent::Disconnect`, game receives `on_disconnect(id)`
6. I/O thread erases closed connections from its map each tick

## Performance Characteristics

| Metric | Expected Range |
|--------|---------------|
| Connections | 10,000+ on single I/O thread |
| Throughput | 10,000+ msgs/sec (echo, localhost) |
| Latency | Sub-millisecond on localhost |
| Memory per connection | Write queue + ReadBuffer (~4KB) |
| Syscalls per tick | 1 poll + N send/recv (batched) |
| Lock contention | Zero (SPSC queues are lock-free) |
| Threads | 3 total (I/O, processing, game) |
