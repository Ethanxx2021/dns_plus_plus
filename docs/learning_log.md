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

---

### Day 2 — Structs & UDP Receiver

**Key Learnings:**
- **Protocol headers**: Defined a 12-byte `DnsHeader` struct using `uint16_t` fixed-width types
- **Hash tables**: `std::unordered_map::find()` / `end()` — foundation for O(1) routing table lookups
- **UDP three-step pattern**: `socket()` → `bind()` → `recvfrom()`

---

### Day 3 — OOP & RAII

**Key Learnings:**
- **RAII (critical)**: Socket allocated in constructor, `close(fd)` in destructor — eliminates port/memory leaks by design
- **Protocol parsing foundation**: `std::stack` for bracket matching — underpins structured binary message boundary logic

---

### Day 4 — Binary Protocol Parsing

**Key Learnings:**
- **`ntohs()` on receive**: Must convert network byte-order back to host order
- **Zero-copy parsing**: `(DnsHeader*)buffer` reinterprets raw bytes as a structured header — most efficient C/C++ protocol parsing technique

---

### Day 5 — Pub/Sub Routing

**Key Learnings:**
- **Pub/Sub architecture**: Subscribe stores `Topic ID → client sockaddr_in`; Publish looks up and forwards
- **Memory alignment**: `#pragma pack(1)` forces 1-byte struct alignment — essential for binary protocol parsing

---

### Day 6 — Multicast Routing

**Key Learnings:**
- **Multi-subscriber routing**: `unordered_map<uint16_t, vector<sockaddr_in>>` — `push_back` to subscribe, iterate + `sendto` for multicast

---

### Day 7 — TTL & Heartbeat

**Key Learnings:**
- **TTL + Heartbeat**: Client sends periodic heartbeat; broker scans and removes entries older than TTL (15s)
- **XOR properties** (LeetCode 136): `a ^ a = 0` — foundation for understanding Paillier blinding cancellation (Phase 3)

---

### Day 8 — Refactor & Formatting

**Key Learnings:**
- **String formatting**: `stringstream` unifies `<<` chained output for logging
- **Message queue**: FIFO `std::queue` decouples network I/O from business logic

---

### Day 9 — Multithreaded Logging

**Key Learnings:**
- **Producer-consumer model**: Main thread produces log strings, background thread consumes. `std::mutex` + `std::condition_variable` for safe coordination
- **Two-stack queue** (LeetCode 232): Amortized O(1) FIFO using LIFO primitives

---

## Phase 1: TLV Protocol & Spatial Routing (June 2025)

### Week 1 — Protocol Upgrade & Algorithm 1 Implementation

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

**Implementation Details:**
- `TlvMessage` class: Zero-copy parser wrapping a raw `recvfrom` buffer; traverses TLV fields by pointer arithmetic
- `TlvMessageBuilder` class: Constructs wire-format packets with `addCoordinates()`, `addServiceName()`, `addFlags()`, `setPayload()`
- `geo.h`: `Region` struct with `contains()`, `overlaps()`, `merge()`, `quadrantCenters()`, `quadrantOf()` — ready for Phase 2
- `brakeAllows(dir, ...)`: Per-service, per-quadrant sliding window counter — configurable limit and window. **T3:** takes a direction (upward vs local) with an independent window each, so `brake_scope=both` gates both directions without one consuming the other's quota.
- `handleSubscribe()`: Inserts into `subscribers[name]`, initializes `cached_closest_dist = ∞`, returns cached pub if `QUERY_MODE` flag set
- `handlePublish()`: Cache publication → (upward brake, if `brake_scope∈{upward,both}`) → downward propagation → (local brake, if `brake_scope∈{local,both}`) → iterate subscribers, forward to those for whom `dist < cached_closest_dist`. The local brake sits **after** the pub_cache write so `QUERY_MODE` still sees braked publications.

---

### Week 2 — Subscription & Query Mode Verification

**Bug Found: Publication not cached before subscriber check**
- **Symptom**: Query mode returned no results when PUBLISH was sent before SUBSCRIBE
- **Root cause**: `handlePublish` cached the publication AFTER the "no subscribers" early return
- **Fix**: Moved cache logic before subscriber check — classic Pub/Sub ordering trap
- **Lesson**: In Pub/Sub systems, caching must happen regardless of current subscriber state

**Bug Found: test_client double-send port mismatch**
- **Symptom**: Subscriber never received publications despite broker logging successful delivery
- **Root cause**: `test_client` sent SUBSCRIBE from a temporary socket (which closed), then listened on a different persistent socket. Broker recorded the wrong port.
- **Fix**: Refactored `test_client` to use a single persistent socket for both sending and receiving

---

### Week 3 — Per-Subscriber Closest Cache Verification

**Test Scenario:**
```
Subscriber A: London (51.5, -0.1)
Subscriber B: Berlin (52.5, 13.4)
Publisher 1:  Paris  (48.8, 2.3)   → closer to London
Publisher 2:  Warsaw (52.2, 21.0)  → closer to Berlin
```

**Result:**
- Paris → Both subscribers received (first publication, closest=∞)
- Warsaw → Only Berlin received (Warsaw is closer to Berlin than Paris; London filtered it out)

This validated Algorithm 1's per-subscriber closest filtering: each subscriber independently tracks their nearest replica.

---

### Week 4 — Testing Framework & Phase 1 Benchmarks

**Unit Tests:**
- `test_tlv.cpp`: 8 tests covering round-trip serialization, empty messages, invalid buffers, float byte-order, duplicate TLVs, long service names
- `test_geo.cpp`: 7 tests covering distance calculation, Region contains/overlaps/merge, quadrant centers, negative coordinates

**Bug Found: Distance function asymmetry**
- **Symptom**: `geoDistance(A, B) != geoDistance(B, A)` in unit test
- **Root cause**: Equirectangular formula used only `lat1` for `cos()` scaling
- **Fix**: Changed to use mean latitude: `cos((lat1 + lat2) / 2)`

**Benchmark Results (10 pubs, 50 subs, 5 trials):**

> ⚠️ **Table withdrawn — see `docs/audit_2026-08.md` Q1.** The recall-vs-brake_limit
> sweep once printed here (0.384 → 1.000) was produced under an earlier brake
> semantics where the brake gated **only upward forwarding**. A single broker has
> no parent, so that brake never fired — the single-broker sweep it describes is
> not reproducible with the code that shipped after the Phase 2 refactor.
>
> **T3 fix (this change):** the brake now honours a `brake_scope` config field
> (`upward` | `local` | `both`, default `both`). With `both`/`local` a single
> broker rate-limits local delivery again, restoring the paper's Algorithm 1
> intent (brake = general propagation limiter). New single-broker sweep data must
> be re-collected on the current code before any table is restored. No fabricated
> numbers are kept in the meantime.

---

## Phase 2: Multi-Broker Hierarchical Overlay (July 2025)

### Week 5 — Configuration & Topology

**Design:**
- Created `key=value` configuration files for each broker (root.conf, leaf1.conf, leaf2.conf)
- Each config specifies: broker_id, listen_port, parent_addr, coords, brake_limit, brake_window
- `main.cpp` parses config file and constructs `BrokerConfig` struct

**Implementation:**
- `broker.h` extended with `BrokerConfig`, `ChildBroker` struct, `children_` map, `parent_addr_`, `my_region_`
- Constructor binds UDP socket, parses parent address, initializes own region as single point

---

### Week 6 — HELLO Handshake & MBH Aggregation

**Protocol Additions:**
- `HELLO` (0x0004): Child → Parent, carries child's ID and coordinates
- `HELLO_ACK` (0x0005): Parent → Child, confirms registration

**Implementation:**
- Child broker sends HELLO on startup if `has_parent_` is true
- Parent's `handleHello()`: records child in `children_` map, updates own MBH region via `updateMyRegion()`, replies with HELLO_ACK
- `updateMyRegion()`: starts with own coordinates, expands to include all children's regions via `Region::merge()`

**Verification:**
- Root correctly aggregated two children's coordinates: `[0, 52.5]×[-0.1, 13.4]`
- Both leaves received HELLO_ACK and logged "Registration complete"

---

### Week 7 — Cross-Broker Subscription & Publication Routing

**Subscription Propagation:**
- `handleSubscribe()` extended: if `FROM_CHILD` flag set, marks child as active in `child_active_[service]`
- If broker hasn't registered with parent for this service, forwards SUBSCRIBE upward with `FROM_CHILD` flag
- `ot_parent_` set tracks which services have been propagated to parent

**Publication Routing (Algorithm 1, lines 1–9):**
- `handlePublish()` restructured into 4 stages:
  1. Cache publication (for query_mode)
  2. Forward UP to parent (if not from parent, subject to brake)
  3. Forward DOWN to active children (subject to quadrant cache filter)
  4. Deliver LOCALLY to subscribers (subject to per-subscriber closest filter)

**Bug Found: Publication not forwarded upward**
- **Symptom**: Leaf2 received publication but Root never got it; Leaf1 subscriber received nothing
- **Root cause**: `handlePublish` had an early return when no local subscribers existed, skipping the upward forwarding logic
- **Fix**: Moved upward forwarding before the local subscriber check
- **Lesson**: In hierarchical Pub/Sub, forwarding decisions must be independent of local subscriber state

---

### Week 8 — Dynamic MBH & Multi-Broker Benchmarks

**Dynamic MBH Implementation:**
- Leaf brokers update their own Region when clients subscribe (expand MBH to include client coordinates)
- `REGION_UPDATE` message (0x0006) sent to parent when Region changes
- Parent's `handleRegionUpdate()`: updates child's Region, recalculates own MBH, propagates further up if changed

**Why Dynamic MBH Matters:**
- Without it, Root uses leaf's single-point coordinate for quadrant filtering
- All publications from the same area appear to be in the same quadrant → only the first is forwarded
- With dynamic MBH, Root sees the leaf's true coverage area → publications distribute across quadrants → more are forwarded

**Traffic Statistics Interface:**
- Added 4 counters in Broker: `stat_forward_up`, `stat_forward_down`, `stat_delivered_local`, `stat_braked` (T3 split into `stat_braked_up` + `stat_braked_local`; legacy `braked` field now reports their sum)
- `STATS_REQUEST` (0x0008) / `STATS_RESPONSE` (0x0009) protocol for benchmark to query counters
- `STATS_DATA` TLV (0x0006): 32 bytes containing 4× `uint64_t` in big-endian

**Multi-Broker Benchmark Results (3-broker tree, 20 pubs, 50 subs, 5 trials):**

| Brake Limit | Recall | Stretch | Traffic Ratio |
|-------------|--------|---------|---------------|
| 1           | 0.94   | 1.05    | 1.06          |
| 2           | 0.95   | 1.05    | 1.09          |
| 4           | 0.96   | 1.05    | 1.14          |
| ∞           | 0.96   | 1.05    | 1.18          |

**Key Findings:**
1. **Brake reduces traffic by ~10%** (1.06 vs 1.18) with only 2% recall cost
2. **Quadrant cache is the dominant filter**: even at brake=∞, traffic ratio is only 1.18 because downward propagation only forwards publications that improve per-quadrant closest distance
3. **Dual filtering mechanism**: Brake limits upward traffic; quadrant cache limits downward traffic. In a 2-level tree, quadrant cache alone provides sufficient reduction. Brake becomes more valuable in deeper trees (3+ levels).
4. **Real-system variance** is present but smaller than Phase 1 due to cross-broker multiplexing effects

---

## Phase 3: Paillier Homomorphic Encryption (July 2025)

### Week 9 — GMP Integration & Original Paillier

**Implementation:**
- Integrated GMP (GNU Multiple Precision Arithmetic) library for 2048-bit integer operations.
- Implemented standard Paillier cryptosystem (keygen, encrypt, decrypt, homomorphic addition).
- Verified homomorphic properties: `D(E(m1)*E(m2)) = m1 + m2`.

### Week 10 — Modified Paillier & Mathematical Gap Discovery

**Critical Discovery:**
While implementing the modified Paillier (Nabeel 2012 protocol), I discovered a mathematical gap in the DNS++ paper's description. The paper states that multiplying two blinded values yields a "randomized difference," but omits how the broker determines the inequality sign from this masked value.

I traced this back to Nabeel 2012 and found the missing piece: the $n/2$ threshold mechanism. By restricting the value domain ($x, v \in [0, 2^l]$) and using specific blinding parameters ($r_m, r$), the protocol ensures that positive differences fall in $[0, n/2)$ and negative differences fall in $(n/2, n)$. The broker can thus determine the sign by comparing the decrypted difference against $n/2$.

**Implementation:**
- Implemented key reversal: private key $\lambda$ used for blinding, public key $(n, g, \mu)$ for unblinding.
- Implemented Nabeel 2012 blinding parameters: $e_m, d_m$ (where $e_m + d_m \equiv 0 \pmod{n}$), $r_m$ (amplification factor).
- Implemented $n/2$ threshold Match protocol.
- Verified semantic security: same plaintext produces different blinded values.

### Week 11 — HEPS Service & Broker Integration

**Implementation:**
- Built HEPS (Homomorphic Encryption Parameter Service) to generate and distribute keys.
- Extended TLV protocol with `BLINDED_VALUE` and `BLINDED_VALUE_HI` fields.
- Integrated `executeMatch()` into Broker's routing path, replacing plaintext string comparison.
- Root broker writes public key to `/tmp/dnspp_heps.key` and full state to `/tmp/dnspp_heps_full.key` for clients.

**Testing:**
- End-to-end encrypted routing verified: Publisher and Subscriber use HEPS to blind names, Broker successfully matches and routes without learning plaintext.

### Week 12 — Privacy Layer Benchmarks

**Results (10 pubs, 50 subs, brake=4, 5 trials, N=250):**
- Plaintext latency: 109.18 ± 2.33 ms
- Encrypted latency: 358.33 ± 8.39 ms
- Overhead: 3.3x (bounded and measurable)
- Recall and Stretch: 1.000 for both modes (routing accuracy preserved)

**Conclusion:**
The core hypothesis is validated: DNS++ can resolve names privately and steer users to the nearest copy on commodity hardware, with the privacy layer adding a bounded, measurable cost of 3.3x latency.

---

## Technical Glossary

| Term | Definition |
|------|------------|
| **Broker** | DNS++ overlay node that routes subscriptions and publications |
| **MBH** | Minimum Bounding Hyperrectangle — the spatial region managed by a broker |
| **Brake** | Per-quadrant rate limiter on publication propagation. `brake_scope` (`upward`/`local`/`both`, default `both`) selects which direction(s) it gates; each direction keeps an independent sliding window. Reported split as `braked_up` / `braked_local` in `STATS_DATA_EXT`. |
| **Quadrant cache** | Per-child, per-quadrant closest-distance cache for downward propagation filtering |
| **Closest cache** | Per-subscriber state tracking the distance of the nearest received publication |
| **Query mode** | Subscription flag requesting immediate return of cached publications |
| **IT[]** | Input Table — records subscriptions received from neighbors (per service name) |
| **OT[]** | Output Table — records subscriptions forwarded to other brokers (per service name) |
| **HEPS** | Homomorphic Encryption Parameter Service — trusted key distribution authority |
| **Paillier** | Partially homomorphic encryption scheme supporting addition on ciphertexts |
| **Blinding** | Encryption operation producing semantically secure, non-reversible ciphertexts |
| **Match** | Homomorphic operation comparing a blinded publication against a blinded subscription |
| **Cover** | Homomorphic operation comparing two blinded subscriptions for routing table construction |
| **$n/2$ Threshold** | The mechanism by which a broker determines the sign of a masked difference |
| **Stretch** | Ratio of actual delivery distance to optimal (ground-truth nearest) distance |
| **Recall** | Proportion of true closest publications successfully delivered to subscribers |
| **Traffic Ratio** | Total forwarding events divided by successful local deliveries |

---

*This log is updated continuously as development progresses.*