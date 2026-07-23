# DNS++: A Privacy-First, Dynamic, Location-Aware Name Resolution System

**UCL MSc Internet Engineering – Final Project (ELEC0054)**
**Student:** Shangqing Xu | **Supervisor:** Professor Miguel Rio

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![Language](https://img.shields.io/badge/C%2B%2B-17-blue)]()
[![Platform](https://img.shields.io/badge/platform-Linux-orange)]()

---

## Overview

DNS++ is a redesign of Internet name resolution that addresses three limitations of the traditional DNS: **privacy leakage** to intermediaries, **inability to keep up** with dynamic service changes, and **lack of location awareness** for replica selection.

This repository contains a **from-scratch C++ implementation** of the DNS++ broker architecture, originally proposed and evaluated only in a Java simulation. The goal of this project is to **build DNS++ as a real, running system on commodity hardware** and **empirically measure** its performance — particularly the overhead introduced by the homomorphic encryption (HE) privacy layer.

### Key Contributions

1. **First real-system implementation** of the DNS++ broker with `epoll`-based asynchronous I/O and a custom TLV binary protocol
2. **Proximity Routing (Algorithm 1)** — per-subscriber closest-replica caching, quadrant-based propagation brake, and query-mode cached responses
3. **Modular crypto interface** — designed for drop-in replacement of plaintext matching with modified Paillier homomorphic encryption (Phase 3)
4. **Empirical evaluation framework** — baseline-comparable benchmarks measuring latency, stretch, recall, and traffic ratio under real hardware constraints

---

## Current Status

| Component | Status | Description |
|-----------|--------|-------------|
| TLV binary protocol | ✅ Done | Extensible Type-Length-Value wire format with zero-copy parsing |
| `epoll` event loop | ✅ Done | Level-triggered, single-threaded with timeout-driven TTL cleanup |
| Pub/Sub routing | ✅ Done | String-based service names, per-subscriber closest cache |
| Proximity Routing (Algorithm 1) | ✅ Done | Closest-replica selection, quadrant brake, query_mode |
| Heartbeat & TTL | ✅ Done | 15-second idle expiry with automatic garbage collection |
| Async logger | ✅ Done | Producer-consumer with `mutex` + `condition_variable` |
| RAII socket management | ✅ Done | Automatic `close()` in destructor |
| Test client | ✅ Done | CLI tool for subscribe / publish / heartbeat |
| Hierarchical broker overlay | 🔄 Phase 2 | Multi-broker tree with MBH region propagation |
| Spatial Discovery (Algorithm 2) | 📋 Phase 2 | Region-based enumeration with FPR aggregation |
| Paillier HE privacy layer | 📋 Phase 3 | Modified Paillier blinding, Match/Cover operations |
| HEPS trusted service | 📋 Phase 3 | Centralized key distribution (decentralized design in thesis discussion) |
| Performance benchmarks | 📋 Phase 3 | Baseline vs HE comparison, statistical reporting |

---

## Architecture

```
                         ┌──────────────────────────────────┐
                         │           Broker (C++)            │
                         │  ┌────────────────────────────┐   │
   Publisher ──PUBLISH──►│  │    epoll Event Loop         │   │
                         │  │    (level-triggered)        │   │
   Subscriber ─SUB────►  │  │                             │   │
                         │  │  ┌───────────────────────┐  │   │
                         │  │  │  TLV Message Parser   │  │   │
                         │  │  │  (zero-copy)          │  │   │
                         │  │  └──────────┬────────────┘  │   │
                         │  │             │               │   │
                         │  │  ┌──────────▼────────────┐  │   │
                         │  │  │  Routing Engine        │  │   │
                         │  │  │  • Service name index  │  │   │
                         │  │  │  • Per-sub closest     │  │   │
                         │  │  │  • Quadrant brake      │  │   │
                         │  │  │  • Query-mode cache    │  │   │
                         │  │  └──────────┬────────────┘  │   │
                         │  │             │               │   │
                         │  │  ┌──────────▼────────────┐  │   │
                         │  │  │  ICryptoEngine (iface) │  │   │
                         │  │  │  [plaintext / Paillier]│  │   │
                         │  │  └───────────────────────┘  │   │
                         │  └────────────────────────────┘   │
                         │  ┌────────────────────────────┐   │
                         │  │  Async Logger Thread       │   │
                         │  │  (mutex + condvar)         │   │
                         │  └────────────────────────────┘   │
                         └──────────────────────────────────┘
```

---

## Protocol Design

The system uses a **TLV (Type-Length-Value)** binary protocol over UDP, designed for extensibility across all project phases.

### Wire Format

```
┌──────────┬──────────┬─────────────┬────────────┬─────────┐
│ msg_type │ num_tlvs │ payload_len │ TLV fields │ payload │
│  2 bytes │  2 bytes │   4 bytes   │  variable  │ variable│
└──────────┴──────────┴─────────────┴────────────┴─────────┘

Each TLV:
┌──────────┬────────┬─────────┐
│   type   │ length │  value  │
│  2 bytes │ 2 bytes │ length  │
└──────────┴────────┴─────────┘
```

### Message Types

| Type | Code | Description |
|------|------|-------------|
| `SUBSCRIBE` | 0x0001 | Client subscribes to a service name |
| `PUBLISH` | 0x0002 | Publisher announces a replica with coordinates |
| `HEARTBEAT` | 0x0003 | Keeps a subscription alive (TTL reset) |
| `HELLO` | 0x0004 | Broker-to-broker registration (Phase 2) |
| `BROKER_FORWARD` | 0x0005 | Inter-broker message forwarding (Phase 2) |
| `REGION_UPDATE` | 0x0006 | MBH region propagation (Phase 2) |

### TLV Field Types

| Type | Code | Size | Description |
|------|------|------|-------------|
| `COORDINATES` | 0x0001 | 8 bytes | `float lat, float lon` |
| `FLAGS` | 0x0002 | 4 bytes | Bitmask: `QUERY_MODE`, `FROM_CHILD`, `FROM_PARENT` |
| `SERVICE_NAME` | 0x0003 | Variable | UTF-8 string (any URI) |
| `BRAKE_LIMIT` | 0x0004 | 4 bytes | Per-quadrant publication limit |
| `REGION` | 0x0005 | 16 bytes | `4×float` MBH (Phase 2) |
| `BLINDED_VALUE` | 0x0010 | Variable | Paillier-blinded service name (Phase 3) |
| `BLINDED_VALUE_HI` | 0x0011 | Variable | Blinded `s+1` for equality check (Phase 3) |
| `BLINDED_COVER` | 0x0012 | Variable | Blinded cover value (Phase 3) |

---

## Quick Start

### Build

```bash
git clone https://github.com/Ethanxx2021/dns_plus_plus.git
cd dns_plus_plus
mkdir -p build && cd build
cmake ..
make
```

### Run Broker

```bash
# Usage: ./dns_broker [port] [brake_limit] [brake_window_sec]
./dns_broker 8080 2 10
```

### Test with CLI Client

```bash
# Terminal 1: Start broker
./dns_broker 8080 2 10

# Terminal 2: Subscriber in London (query_mode)
./test_client sub weather.example 51.5 -0.1 query

# Terminal 3: Subscriber in Berlin
./test_client sub weather.example 52.5 13.4

# Terminal 4: Publisher in Paris (closer to London)
./test_client pub weather.example 48.8 2.3 Paris-edge-1
# → Both subscribers receive it (first publication, closest=∞)

# Terminal 5: Publisher in Warsaw (closer to Berlin)
./test_client pub weather.example 52.2 21.0 Warsaw-edge-1
# → London subscriber does NOT receive (Warsaw is farther than Paris)
# → Berlin subscriber receives (Warsaw is closer than Paris)
```

---

## Project Structure

```
dns_plus_plus/
├── README.md
├── CMakeLists.txt
├── docs/
│   └── learning_log.md          # Development journal
├── src/
│   ├── broker/
│   │   ├── broker.h             # Broker class, routing tables, Algorithm 1
│   │   └── broker.cpp           # Event loop, message handlers, brake logic
│   ├── protocol/
│   │   ├── TlvMessage.h         # TLV protocol definition, parser, builder
│   │   └── TlvMessage.cpp       # Serialization/deserialization
│   ├── logger/
│   │   └── logger.h             # Thread-safe async logger
│   ├── crypto/
│   │   └── ICryptoEngine.h      # Crypto interface (Phase 3 placeholder)
│   ├── utils/
│   │   └── geo.h                # Region (MBH), distance, quadrant utilities
│   └── main.cpp                 # Entry point
├── clients/
│   └── test_client.cpp          # CLI test client (sub/pub/beat)
├── tests/                       # Unit tests (planned)
├── benchmarks/                  # Performance benchmarks (Phase 3)
└── external/                    # Third-party libraries (Paillier ref, GMP)
```

---

## Mapping to DNS++ Research Paper

| Paper Section | Component | Implementation Status |
|---------------|-----------|----------------------|
| §3.1 Overlay Creation | Hierarchical broker tree, MBH aggregation | 🔄 Phase 2 |
| §3.2 HE Privacy Primitives | Modified Paillier, Match/Cover, HEPS | 📋 Phase 3 |
| §3.3 Protocol Workflow | Binary tree index, IT[]/OT[] tables | ✅ (flat index, Phase 2 adds tree) |
| §3.4 Proximity Routing | Algorithm 1: closest cache, brake, query_mode | ✅ Single-broker complete |
| §3.5 Spatial Discovery | Algorithm 2: region containment, FPR aggregation | 📋 Phase 2 |
| §4.2 Proximity Evaluation | Stretch, recall, matching cost, traffic ratio | 📋 Phase 3 |
| §4.3 Spatial Evaluation | Table size, FPR trade-offs | 📋 Phase 3 |
| §4.4 GPU Acceleration | CUDA-accelerated Paillier matching | ❌ Out of scope (thesis discussion) |

---

## Development Roadmap

### Phase 1 – Spatial Routing Foundation (Weeks 1–4) ✅
- [x] TLV binary protocol with extensible field types
- [x] String-based service names (any URI)
- [x] Per-subscriber closest-replica caching (Algorithm 1, lines 7–9)
- [x] Quadrant-based propagation brake with sliding window
- [x] Query-mode cached response (Algorithm 1, lines 3–5)
- [x] Equirectangular distance approximation
- [x] CLI test client with subscribe/publish/heartbeat
- [x] RAII socket management, async logger, TTL cleanup

### Phase 2 – Hierarchical Topology (Weeks 5–8) 🔄
- [ ] Multi-broker tree with configuration-driven topology
- [ ] HELLO handshake protocol (child → parent registration)
- [ ] MBH region computation (bottom-up aggregation)
- [ ] Cross-broker subscription propagation (IT[]/OT[] tables)
- [ ] Cross-broker publication forwarding with quadrant cache filtering
- [ ] Spatial Discovery (Algorithm 2) with FPR-based aggregation
- [ ] End-to-end multi-broker testing and initial measurements

### Phase 3 – Cryptographic Integration (Weeks 9–12) 📋
- [ ] Modified Paillier cryptosystem (key generation, blinding, decryption)
- [ ] Match(pub, sub) and Cover(sub₁, sub₂) homomorphic operations
- [ ] HEPS trusted third-party service (centralized)
- [ ] Integration: replace plaintext matching with HE matching
- [ ] Baseline benchmarks: plaintext vs encrypted (latency, throughput, memory)
- [ ] Statistical reporting: median, p95, p99 over 30+ runs
- [ ] Comparison with paper's simulation results

### Thesis Write-Up (Weeks 12–14) 📋
- [ ] Full dissertation draft
- [ ] HEPS decentralization analysis (HAP/HEP/HIP trust models)
- [ ] Real-system vs simulation gap analysis
- [ ] Final submission

---

## Engineering Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Transport | UDP (`SOCK_DGRAM`) | Low-latency, connectionless — mirrors real DNS behavior |
| I/O model | `epoll` (level-triggered) | Efficient multiplexing; timeout doubles as TTL timer |
| Protocol format | TLV (Type-Length-Value) | Extensible across phases without breaking changes |
| Service name type | `std::string` | Paper supports any URI; not limited to numeric IDs |
| Distance function | Equirectangular approximation | Single `cos()` call; sufficient for routing decisions |
| Threading | Single-threaded main + async logger | Keeps routing logic lock-free; logger isolated |
| Crypto interface | `ICryptoEngine` abstract class | Decouples network routing from cryptographic implementation |
| Build system | CMake | Cross-platform, standard for C++ projects |

---

## Academic Context

### Research Gap

The DNS++ design [Rio et al.] promises privacy, dynamics, and locality in a single system — but has **only been evaluated in a Java simulation**. No real-system implementation exists to validate whether the design is practical on commodity hardware, or to measure the true cost of the homomorphic encryption privacy layer.

### Hypothesis

> If the DNS++ design is built as a real system, then it can resolve names privately, keep up with change, and steer users to the nearest copy on ordinary hardware — with the privacy layer adding only a bounded, measurable cost.

### Evaluation Plan

All measurements are compared against a **plaintext baseline** (same system with HE disabled):

| Metric | Definition |
|--------|------------|
| Lookup latency | Time from SUBSCRIBE to first PUBLISH received |
| Update delay | Time from PUBLISH to subscriber receipt |
| Privacy cost | Latency with HE vs without HE |
| Traffic ratio | Forwarding events per delivered update |
| Stretch | Actual delivery distance / optimal distance |
| Recall | Proportion of true closest publications delivered |

---

## Contact

**Shangqing Xu** – ethanxx2021@163.com

Project link: [https://github.com/Ethanxx2021/dns_plus_plus](https://github.com/Ethanxx2021/dns_plus_plus)

---

*Last updated: June 2025*
