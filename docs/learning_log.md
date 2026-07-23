# DNS++ Development Log

A chronological record of technical learning, design decisions, and implementation milestones throughout the project.

---

## Phase 0: Foundation (May 2025)

### Day 1 — Project Kickoff & Environment Setup

**Goals:**
- Set up Ubuntu + VSCode C++ development environment
- Learn C++ fundamentals (references, containers)
- Select DNS++ transport protocol

**Key Learnings:**
- **Transport selection**: DNS++ uses **UDP (`SOCK_DGRAM`)** — connectionless, low-latency, mirroring real DNS behavior
- **Byte-order**: All network integers must use **big-endian** via `htons()`/`ntohs()`
- **Algorithmic thinking**: LeetCode 206 (reverse linked list) maps directly to DNS recursive query path reversal

**Code:**
- Compiled and ran first `dns_node_init.cpp`
- Understood `std::vector` traversal and `&` reference semantics for avoiding copies

---

### Day 2 — Structs & UDP Receiver

**Goals:**
- Master C++ structs and memory layout
- Build first UDP receiving program

**Key Learnings:**
- **Protocol headers**: Defined a 12-byte `DnsHeader` struct using `uint16_t` fixed-width types for cross-platform consistency
- **Hash tables**: `std::unordered_map::find()` / `end()` — the foundation for future O(1) routing table lookups
- **UDP three-step pattern**: `socket()` → `bind()` → `recvfrom()` with `sockaddr_in` for sender address capture

**Code:**
- Wrote `udp_server.cpp` from scratch
- Tested with `nc -u 127.0.0.1 8080` — server successfully printed received strings

---

### Day 3 — OOP & RAII

**Goals:**
- Master C++ OOP and RAII resource management
- Implement full-duplex UDP (Echo Server)

**Key Learnings:**
- **RAII (critical)**: Socket allocated in constructor, `close(fd)` in destructor — eliminates port/memory leaks by design
- **Protocol parsing foundation**: `std::stack` for bracket matching (LeetCode 20) — underpins structured binary message boundary logic

**Code:**
- Refactored procedural UDP code into a `UdpServer` class
- Implemented `sendto()` echo reply using the `sockaddr_in` captured by `recvfrom()`
- Verified round-trip: `nc` terminal received `DNS++ ACK:` response

---

### Day 4 — Binary Protocol Parsing

**Goals:**
- Master pointer casting for binary deserialization
- Extract source IP and port from UDP packets
- Test with raw hex binary streams

**Key Learnings:**
- **`ntohs()` on receive**: Must convert network byte-order back to host order, otherwise integers decode as garbage
- **Client info extraction**: `recvfrom` populates `sockaddr_in` — `inet_ntoa()` for IP string, `ntohs()` for port
- **Zero-copy parsing**: `(DnsHeader*)buffer` reinterprets raw bytes as a structured header — the most efficient C/C++ protocol parsing technique

**Code:**
- Wrote `dns_server_v2.cpp` — successfully parsed 12-byte DNS header from raw binary
- Learned `printf '\x12\x34...' | nc -u` for sending crafted binary packets

---

### Day 5 — Pub/Sub Routing

**Goals:**
- Master `std::unordered_map` for in-memory routing tables
- Implement DNS++ broker core: subscribe + publish forwarding

**Key Learnings:**
- **Pub/Sub architecture**:
  - *Subscribe*: Broker stores `Topic ID → client sockaddr_in` in `unordered_map`
  - *Publish*: Broker looks up Topic ID, calls `sendto()` to forward to matched client
- **Memory alignment**: `#pragma pack(1)` forces 1-byte struct alignment, preventing compiler padding — essential for correct binary protocol parsing
- **Fast/slow pointers** (LeetCode 141): Conceptual foundation for cycle detection in linked routing paths

**Code:**
- Wrote `dns_broker_v1.cpp` — first working pub/sub middleware over UDP
- 3-terminal test: Client A subscribes, Client B publishes, Broker routes successfully

---

### Day 6 — Multicast Routing

**Goals:**
- Upgrade routing table to support multiple subscribers per topic
- Implement multicast forwarding

**Key Learnings:**
- **Multi-subscriber routing**: `unordered_map<uint16_t, vector<sockaddr_in>>` — `push_back` to subscribe, iterate `vector` + `sendto` for multicast
- **Intersecting linked lists** (LeetCode 160): Dual-pointer traversal — "you walk my path, I walk yours" — conceptually mirrors bidirectional broker forwarding

**Code:**
- Wrote `dns_broker_v2_multicast.cpp` — one publisher broadcasting to two subscribers simultaneously

---

### Day 7 — TTL & Heartbeat

**Goals:**
- Write first C++ network client
- Implement TTL-based subscription expiry

**Key Learnings:**
- **TTL + Heartbeat mechanism**:
  - Client sends periodic heartbeat (`msg_type=3`) to signal liveness
  - Broker maintains `topic_last_active` timestamp map
  - Periodic scan removes entries older than TTL (15s) — prevents sending to dead addresses
- **Socket receive timeout**: `setsockopt(SO_RCVTIMEO)` for non-blocking client receives
- **XOR properties** (LeetCode 136): `a ^ a = 0`, `a ^ 0 = a` — foundation for understanding Paillier blinding cancellation (Phase 3)

**Code:**
- Wrote `dns_client.cpp` with `SO_RCVTIMEO` and `std::this_thread::sleep_for`
- Full TTL lifecycle: heartbeat → update → expire → cleanup

---

### Day 8 — Refactor & Formatting

**Goals:**
- Master `std::stringstream` for structured output
- Understand `std::queue` for message buffering
- Refactor broker for readability

**Key Learnings:**
- **String formatting**: `stringstream` unifies `<<` chained output into a single string object — clean for logging and monitoring
- **Message queue**: FIFO `std::queue` decouples network I/O from business logic processing
- **Min-stack design** (LeetCode 155): Auxiliary stack tracking global minimum — "space trade for time" principle applicable to routing table optimization

**Code:**
- Refactored to `dns_broker_v4_refactor.cpp` — all log output unified through `stringstream`

---

### Day 9 — Multithreaded Logging

**Goals:**
- Master `std::thread`, `join`, `detach`, `mutex`
- Implement background logger thread

**Key Learnings:**
- **Producer-consumer model**: Main thread produces log strings (`pushLog`), background thread consumes and writes (`process`). `std::mutex` + `std::condition_variable` for safe coordination
- **Two-stack queue** (LeetCode 232): `inStack` for enqueue, `outStack` for dequeue — amortized O(1) FIFO using LIFO primitives

**Code:**
- Integrated dedicated logger thread into broker — main loop stays responsive

---

## Phase 1: TLV Protocol & Spatial Routing (June 2025)

### Week 1 — Protocol Upgrade & Algorithm 1 Implementation

**Goals:**
- Replace fixed-header protocol with extensible TLV format
- Implement Algorithm 1 (Proximity Routing) per paper §3.4
- Add string-based service names, coordinates, query_mode, per-subscriber closest cache

**Design Decisions:**

| Decision | Rationale |
|----------|-----------|
| TLV (Type-Length-Value) format | Phase 3 adds blinded cryptographic values without protocol changes |
| `std::string` service names | Paper supports any URI as a name, not just numeric IDs |
| Per-subscriber `cached_closest_dist` | Paper Algorithm 1 lines 7–9: each subscriber tracks their own closest publication |
| Equirectangular distance | Single `cos()` call; sufficient accuracy for routing (not navigation) |
| `Region` struct with MBH operations | Foundation for Phase 2 hierarchical overlay and quadrant-based brake |

**Key Changes from Phase 0:**

1. **Protocol**: 12/20-byte fixed headers → 8-byte fixed header + variable TLV fields + variable payload
2. **Routing**: Single-nearest-subscriber delivery → per-subscriber closest cache (Algorithm 1 compliant)
3. **Topic key**: `uint16_t topic_id` → `std::string service_name`
4. **Query mode**: Not implemented → returns cached nearest publication immediately
5. **Distance**: Raw Euclidean → Equirectangular approximation (accounts for longitude convergence)
6. **Publication cache**: None → `pub_cache[service_name]` stores recent publications for query_mode

**Implementation Details:**

- `TlvMessage` class: Zero-copy parser wrapping a raw `recvfrom` buffer; traverses TLV fields by pointer arithmetic
- `TlvMessageBuilder` class: Constructs wire-format packets with `addCoordinates()`, `addServiceName()`, `addFlags()`, `setPayload()`
- `geo.h`: `Region` struct with `contains()`, `overlaps()`, `merge()`, `quadrantCenters()`, `quadrantOf()` — ready for Phase 2
- `brakeAllows()`: Per-service, per-quadrant sliding window counter — configurable limit and window
- `handleSubscribe()`: Inserts into `subscribers[name]`, initializes `cached_closest_dist = ∞`, returns cached pub if `QUERY_MODE` flag set
- `handlePublish()`: Brake check → cache publication → iterate all subscribers, forward to those for whom `dist < cached_closest_dist`

**Test Scenario:**

```
Broker on port 8080, brake_limit=2, window=10s

Subscriber A: London (51.5, -0.1)
Subscriber B: Berlin (52.5, 13.4)

Publisher 1: Paris (48.8, 2.3)  →  Both receive (first pub, closest=∞)
Publisher 2: Warsaw (52.2, 21.0) →  Only Berlin receives (Warsaw closer to Berlin than Paris)
                                      London does NOT receive (Warsaw farther than Paris)
```

This validates Algorithm 1's per-subscriber closest filtering: each subscriber independently tracks their nearest replica.

**Files Created/Modified:**

| File | Action | Description |
|------|--------|-------------|
| `src/protocol/TlvMessage.h` | Created | TLV protocol definition, parser, builder |
| `src/protocol/TlvMessage.cpp` | Created | Serialization/deserialization implementation |
| `src/utils/geo.h` | Created | Region (MBH), distance, quadrant utilities |
| `src/broker/broker.h` | Rewritten | Algorithm 1 data structures, string service names |
| `src/broker/broker.cpp` | Rewritten | TLV message handling, per-subscriber closest, brake, query_mode |
| `src/main.cpp` | Updated | CLI args for port, brake_limit, brake_window |
| `clients/test_client.cpp` | Created | CLI test client (sub/pub/beat modes) |
| `CMakeLists.txt` | Updated | Added test_client target, TlvMessage source |

---

### Next Steps (Week 2)

- [ ] Unit tests for TLV serialization round-trip
- [ ] Unit tests for distance and quadrant calculations
- [ ] Multi-publisher convergence test (5+ publishers, verify closest stabilizes)
- [ ] Brake parameter sweep test (limit=1 vs 2 vs 4 vs ∞, measure recall)
- [ ] Begin Phase 2 design: broker configuration file format, HELLO handshake

---

## Technical Glossary

| Term | Definition |
|------|------------|
| **Broker** | DNS++ overlay node that routes subscriptions and publications |
| **MBH** | Minimum Bounding Hyperrectangle — the spatial region managed by a broker |
| **Brake** | Per-quadrant rate limiter on upward publication propagation (Algorithm 1) |
| **Closest cache** | Per-subscriber state tracking the distance of the nearest received publication |
| **Query mode** | Subscription flag requesting immediate return of cached publications |
| **HEPS** | Homomorphic Encryption Parameter Service — trusted key distribution authority |
| **Paillier** | Partially homomorphic encryption scheme supporting addition on ciphertexts |
| **Blinding** | Encryption operation producing semantically secure, non-reversible ciphertexts |
| **Match** | Homomorphic operation comparing a blinded publication against a blinded subscription |
| **Cover** | Homomorphic operation comparing two blinded subscriptions for routing table construction |
| **FPR** | False Positive Ratio — threshold controlling subscription region aggregation (Algorithm 2) |
| **Stretch** | Ratio of actual delivery distance to optimal (ground-truth nearest) distance |
| **Recall** | Proportion of true closest publications successfully delivered to subscribers |

---

*This log is updated continuously as development progresses.*
