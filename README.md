# DNS++: A Privacy-First, Dynamic, Location-Aware Name Resolution System

**UCL MSc Internet Engineering – Final Project (ELEC0054)**
**Student:** Shangqing Xu | **Supervisor:** Professor Miguel Rio

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![Language](https://img.shields.io/badge/C%2B%2B-17-blue)]()
[![Platform](https://img.shields.io/badge/platform-Linux-orange)]()
[![Crypto](https://img.shields.io/badge/Crypto-Paillier%202048--bit-red)]()

---

## Overview

DNS++ is a redesign of Internet name resolution that addresses three limitations of the traditional DNS: **privacy leakage** to intermediaries, **inability to keep up** with dynamic service changes, and **lack of location awareness** for replica selection.

This repository contains a **from-scratch C++ implementation** of the DNS++ broker architecture, originally proposed and evaluated only in a Java simulation. The goal of this project is to **build DNS++ as a real, running system on commodity hardware** and **empirically measure** its performance — particularly the overhead introduced by the homomorphic encryption (HE) privacy layer.

### Key Contributions

1. **First real-system implementation** of the DNS++ broker with `epoll`-based asynchronous I/O and a custom TLV binary protocol.
2. **Multi-broker hierarchical overlay** with dynamic MBH region aggregation, HELLO handshake, and cross-broker subscription/publication propagation.
3. **Proximity Routing (Algorithm 1)** — per-subscriber closest-replica caching, quadrant-based propagation brake, and query-mode cached responses.
4. **Modified Paillier Privacy Layer** — implemented the Nabeel 2012 blinding protocol with key reversal and the $n/2$ threshold Match mechanism, fixing a mathematical gap in the original DNS++ paper description.
5. **Empirical evaluation framework** — baseline-comparable benchmarks measuring latency, stretch, recall, traffic ratio, and cryptographic overhead under real hardware constraints.

---

## Current Status

| Component | Status | Description |
|-----------|--------|-------------|
| TLV binary protocol | ✅ Done | Extensible Type-Length-Value wire format with zero-copy parsing |
| `epoll` event loop | ✅ Done | Level-triggered, single-threaded with timeout-driven TTL cleanup |
| Algorithm 1 (Proximity Routing) | ✅ Done | Closest-replica selection, quadrant brake, query_mode |
| Multi-broker hierarchical overlay | ✅ Done | 3-broker tree with config-driven topology, HELLO handshake |
| Dynamic MBH region aggregation | ✅ Done | Bottom-up MBH computation with REGION_UPDATE propagation |
| Cross-broker routing | ✅ Done | Upward (brake-limited) + downward (quadrant-filtered) propagation |
| Modified Paillier (2048-bit) | ✅ Done | Key reversal, blinding, $n/2$ threshold Match, GMP library |
| HEPS trusted service | ✅ Done | Key generation and blinding parameter distribution |
| Encrypted Broker routing | ✅ Done | `executeMatch()` replaces plaintext string comparison |
| Plaintext vs Encrypted benchmarks | ✅ Done | 3.3x bounded latency overhead measured, 100% recall maintained |
| Scalability sweep | ✅ Done | Tested up to 1000 subscribers, 100% recall maintained |
| Unit tests | ✅ Done | TLV (8), Geo (7), Paillier (4), HEPS (2) |
| Integration & Benchmark scripts | ✅ Done | Single-broker, multi-broker, and crypto comparison scripts |

---

## Architecture

```
                         ┌──────────────────────────────────────┐
                         │           Broker (C++)                │
                         │  ┌──────────────────────────────────┐ │
   Publisher ──PUBLISH──►│  │    epoll Event Loop              │ │
                         │  │    (level-triggered)             │ │
   Subscriber ─SUB────►  │  │                                  │ │
                         │  │  ┌─────────────────────────────┐ │ │
                         │  │  │  TLV Message Parser         │ │ │
                         │  │  │  (zero-copy)                │ │ │
                         │  │  └──────────┬──────────────────┘ │ │
                         │  │             │                    │ │
                         │  │  ┌──────────▼──────────────────┐ │ │
                         │  │  │  Routing Engine              │ │ │
                         │  │  │  • Service name index        │ │ │
                         │  │  │  • Per-sub closest cache     │ │ │
                         │  │  │  • Quadrant brake            │ │ │
                         │  │  │  • MBH region pruning        │ │ │
                         │  │  │  • Cross-broker forwarding   │ │ │
                         │  │  │  • IT[]/OT[] tables          │ │ │
                         │  │  └──────────┬──────────────────┘ │ │
                         │  │             │                    │ │
                         │  │  ┌──────────▼──────────────────┐ │ │
                         │  │  │  Paillier Match Engine       │ │ │
                         │  │  │  (2048-bit, GMP)             │ │ │
                         │  │  └─────────────────────────────┘ │ │
                         │  └──────────────────────────────────┘ │
                         │  ┌──────────────────────────────────┐ │
                         │  │  Async Logger Thread             │ │
                         │  │  (mutex + condvar)               │ │
                         │  └──────────────────────────────────┘ │
                         └──────────────────────────────────────┘
```

### Multi-Broker Topology (Phase 2)

```
                    Root Broker (port 9000)
                    Region: MBH of all children
                   /                        \
          Leaf Broker 1               Leaf Broker 2
          (London, port 9001)         (Berlin, port 9002)
          Region: MBH of clients      Region: MBH of clients
              |                           |
     Subscribers/Publishers       Subscribers/Publishers
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
| `HELLO` | 0x0004 | Child broker registers with parent |
| `HELLO_ACK` | 0x0005 | Parent confirms child registration |
| `REGION_UPDATE` | 0x0006 | Child notifies parent of MBH region change |
| `STATS_REQUEST` | 0x0008 | Benchmark queries traffic statistics |
| `STATS_RESPONSE` | 0x0009 | Broker returns traffic counters |

### TLV Field Types

| Type | Code | Size | Description |
|------|------|------|-------------|
| `COORDINATES` | 0x0001 | 8 bytes | `float lat, float lon` |
| `FLAGS` | 0x0002 | 4 bytes | Bitmask: `QUERY_MODE`, `FROM_CHILD`, `FROM_PARENT` |
| `SERVICE_NAME` | 0x0003 | Variable | UTF-8 string (any URI) |
| `BRAKE_LIMIT` | 0x0004 | 4 bytes | Per-quadrant publication limit |
| `REGION` | 0x0005 | 16 bytes | `4×float` MBH (min/max lat/lon) |
| `STATS_DATA` | 0x0006 | 32 bytes | `4×uint64_t` traffic counters |
| `BLINDED_VALUE` | 0x0010 | Variable | Paillier-blinded notification (Phase 3) |
| `BLINDED_VALUE_HI` | 0x0011 | Variable | Paillier-blinded subscription `v+1` (Phase 3) |

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

### Run Encrypted Single-Broker (Phase 3)

```bash
# Terminal 1: Start broker (generates /tmp/dnspp_heps_full.key)
./dns_broker 8080 4 10
# Terminal 1: Start broker (dns_broker takes a config file, not positional args)
./dns_broker ../configs/single.conf

# Terminal 2: Subscriber in London
./test_client sub 127.0.0.1 8080 weather.example 51.5 -0.1

# Terminal 3: Publisher in Paris
./test_client pub 127.0.0.1 8080 weather.example 48.8 2.3 Paris-edge-1
# → London subscriber receives publication via encrypted Match!
```

`test_client` blinds its messages by default, which requires the root broker to have
already written `/tmp/dnspp_heps_full.key`. Append `--plaintext` (alias `--no-encrypt`)
to either command to skip blinding and talk to the broker in plaintext.

### Run Multi-Broker Tree (Phase 2)

```bash
# Terminal 1: Root broker
./dns_broker ../configs/root.conf

# Terminal 2: Leaf broker 1 (London)
./dns_broker ../configs/leaf1.conf

# Terminal 3: Leaf broker 2 (Berlin)
./dns_broker ../configs/leaf2.conf

# Terminal 4: Subscribe via London leaf
./test_client sub 127.0.0.1 9001 weather.example 51.5 -0.1

# Terminal 5: Publish via Berlin leaf
./test_client pub 127.0.0.1 9002 weather.example 52.2 13.0 Berlin-edge-1
# → London subscriber receives publication via cross-broker routing
```

### Run Benchmarks

```bash
# Plaintext vs Encrypted benchmark
./dns_broker 8080 4 10 &
sleep 1
./bench_broker 127.0.0.1 8080 10 50 4 5 42 0 > plain.csv 2>plain.log
./bench_broker 127.0.0.1 8080 10 50 4 5 42 1 > encrypted.csv 2>encrypted.log
# Single-broker brake sweep
./dns_broker ../configs/single.conf &
./bench_broker 127.0.0.1 8080 10 50 2 5 42 > results.csv 2>summary.log
kill %1
# NOTE: bench_broker's brake_limit argument is only recorded in the output CSV;
# the broker's own brake_limit comes from its config file. See docs/audit_2026-08.md Q1.

# Generate charts
source ../venv/bin/activate
python3 ../scripts/plot_crypto.py
```

---

## Project Structure

```
dns_plus_plus/
├── README.md
├── CMakeLists.txt
├── configs/
│   ├── root.conf              # Root broker configuration
│   ├── leaf1.conf             # Leaf broker 1 (London)
│   └── leaf2.conf             # Leaf broker 2 (Berlin)
├── docs/
│   └── learning_log.md        # Development journal
├── src/
│   ├── broker/
│   │   ├── broker.h           # Broker class, routing tables, Algorithm 1
│   │   └── broker.cpp         # Event loop, message handlers, cross-broker routing
│   ├── protocol/
│   │   ├── TlvMessage.h       # TLV protocol definition, parser, builder
│   │   └── TlvMessage.cpp     # Serialization/deserialization
│   ├── crypto/
│   │   ├── Paillier.h         # Modified Paillier cryptosystem definition
│   │   ├── Paillier.cpp       # 2048-bit keygen, blinding, n/2 threshold Match
│   │   ├── Heps.h             # HEPS trusted service interface
│   │   └── Heps.cpp           # Key distribution and blinding operations
│   ├── logger/
│   │   └── logger.h           # Thread-safe async logger
│   └── utils/
│       └── geo.h              # Region (MBH), distance, quadrant utilities
├── clients/
│   └── test_client.cpp        # CLI test client (sub/pub/beat with HEPS)
├── tests/
│   ├── test_tlv.cpp           # TLV protocol unit tests
│   ├── test_geo.cpp           # Geo function unit tests
│   ├── test_paillier.cpp      # Paillier crypto unit tests
│   └── test_heps.cpp          # HEPS end-to-end match tests
├── benchmarks/
│   ├── bench_broker.cpp       # Single-broker benchmark (plain/encrypted)
│   └── bench_multi_broker.cpp # Multi-broker benchmark (auto-fork)
├── scripts/
│   ├── plot_results.py        # Phase 1 chart generator
│   ├── plot_multi_broker.py   # Phase 2 chart generator
│   └── plot_crypto.py         # Phase 3 chart generator
└── results/
    ├── phase1/                # Phase 1 CSV data + charts
    ├── phase2/                # Phase 2 CSV data + charts
    └── phase3/                # Phase 3 CSV data + charts
```

---

## Mapping to DNS++ Research Paper

| Paper Section | Component | Implementation Status |
|---------------|-----------|----------------------|
| §3.1 Overlay Creation | Hierarchical broker tree, MBH aggregation | ✅ Multi-broker tree with dynamic MBH |
| §3.2 HE Privacy Primitives | Modified Paillier, Match/Cover, HEPS | ✅ 2048-bit Paillier, n/2 threshold Match, HEPS |
| §3.3 Protocol Workflow | Binary tree index, IT[]/OT[] tables | ✅ Flat index + cross-broker forwarding tables |
| §3.4 Proximity Routing | Algorithm 1: closest cache, brake, query_mode | ✅ Single + multi-broker complete |
| §3.5 Spatial Discovery | Algorithm 2: region containment, FPR aggregation | 📋 Future work |
| §4.2 Proximity Evaluation | Stretch, recall, matching cost, traffic ratio | ✅ Phase 1 + Phase 2 + Phase 3 data |
| §4.3 Spatial Evaluation | Table size, FPR trade-offs | 📋 Not applicable (Alg 2 deferred) |
| §4.4 GPU Acceleration | CUDA-accelerated Paillier matching | ❌ Out of scope (thesis discussion) |

---

## Evaluation Results

### Phase 1: Single-Broker Brake Sweep

10 publishers, 50 subscribers, 5 trials per configuration, globally random placement.

| Brake Limit | Recall (mean ± std) | Stretch (mean ± std) |
|-------------|---------------------|----------------------|
| 1           | 0.384 ± 0.165       | 2.546 ± 1.043        |
| 2           | 0.724 ± 0.141       | 1.373 ± 0.233        |
| 4           | 1.000 ± 0.000       | 1.000 ± 0.000        |
| ∞           | 1.000 ± 0.000       | 1.000 ± 0.000        |

**Key finding:** Brake=4 achieves 100% recall with optimal stretch at this scale. Real-system variance (std up to 0.165 at brake=1) is absent from the Java simulation.

### Phase 2: Multi-Broker Hierarchical Routing

3-broker tree (1 root + 2 leaves), 20 publishers, 50 subscribers, 5 trials, globally random placement.

| Brake Limit | Recall (mean ± std) | Stretch (mean ± std) | Traffic Ratio (mean ± std) |
|-------------|---------------------|----------------------|---------------------------|
| 1           | 0.94 ± 0.07         | 1.05 ± 0.06          | 1.06 ± 0.01               |
| 2           | 0.95 ± 0.07         | 1.05 ± 0.05          | 1.09 ± 0.01               |
| 4           | 0.96 ± 0.06         | 1.05 ± 0.04          | 1.14 ± 0.01               |
| ∞           | 0.96 ± 0.06         | 1.05 ± 0.05          | 1.18 ± 0.01               |

**Key findings:**
1. Brake reduces traffic ratio by ~10% (1.06 vs 1.18) with only 2% recall cost
2. Quadrant cache filtering is the dominant traffic reducer (effective even at brake=∞)
3. Real-system variance is present but smaller than Phase 1 due to cross-broker multiplexing

### Phase 3: Privacy Layer Overhead

10 publishers, 50 subscribers, brake=4, 5 trials (N=250).

| Mode | Recall | Stretch | Latency (mean ± std) |
|------|--------|---------|----------------------|
| Plaintext | 1.000 | 1.000 | 109.18 ± 2.33 ms |
| Encrypted (Paillier) | 1.000 | 1.000 | 358.33 ± 8.39 ms |

**Key finding:** Homomorphic encryption adds a bounded 3.3x latency overhead while maintaining 100% routing accuracy.

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
- [x] Unit tests (TLV: 8 tests, Geo: 7 tests)
- [x] Integration test script
- [x] Benchmark framework with CSV output
- [x] Phase 1 brake sweep results and charts

### Phase 2 – Hierarchical Topology (Weeks 5–8) ✅
- [x] Multi-broker tree with configuration-driven topology
- [x] HELLO handshake protocol (child → parent registration)
- [x] MBH region computation (bottom-up aggregation)
- [x] Dynamic MBH updates (client registration triggers region propagation)
- [x] Cross-broker subscription propagation (IT[]/OT[] tables)
- [x] Cross-broker publication forwarding with quadrant cache filtering
- [x] Traffic statistics interface (STATS_REQUEST/RESPONSE)
- [x] Multi-broker benchmark framework (auto-fork 3 brokers)
- [x] Phase 2 multi-broker results and charts

### Phase 3 – Cryptographic Integration (Weeks 9–12) ✅
- [x] Integrated GMP library for 2048-bit large integer arithmetic
- [x] Implemented standard Paillier cryptosystem (keygen, encrypt, decrypt, homomorphic add)
- [x] Discovered and resolved mathematical gap in DNS++ paper Section 3.2
- [x] Implemented modified Paillier: key reversal, blinding parameters (e_m, d_m, r_m)
- [x] Implemented n/2 threshold Match protocol for inequality checking
- [x] Built HEPS service for key generation and parameter distribution
- [x] Extended TLV protocol with BLINDED_VALUE fields
- [x] Integrated executeMatch() into Broker routing path
- [x] End-to-end encrypted routing verified
- [x] Phase 3 benchmarks: 3.3x bounded latency overhead, 100% recall maintained

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
| Crypto library | GMP (GNU Multiple Precision) | Industry standard for 2048-bit modular arithmetic |
| Crypto integration | `executeMatch()` in Broker | Decouples network routing from cryptographic implementation |
| Topology config | `key=value` files | No third-party YAML dependency; trivial to parse |
| Broker spawning | `fork()` in benchmark | Real process isolation; same code paths as production |
| Sanitizer support | ASan/TSan via CMake options | Industrial-grade memory and thread safety validation |

---

## Academic Context

### Research Gap

The DNS++ design [Rio et al.] promises privacy, dynamics, and locality in a single system — but has **only been evaluated in a Java simulation**. No real-system implementation exists to validate whether the design is practical on commodity hardware, or to measure the true cost of the homomorphic encryption privacy layer.

### Hypothesis

> If the DNS++ design is built as a real system, then it can resolve names privately, keep up with change, and steer users to the nearest copy on ordinary hardware — with the privacy layer adding only a bounded, measurable cost.

**Validation Status:**
- ✅ "steer users to the nearest copy" → 100% recall, 1.000 stretch (brake=4)
- ✅ "resolve names privately" → Modified Paillier Match implemented and verified
- ✅ "bounded, measurable cost" → 3.3x latency overhead measured (109ms → 358ms)

---

## Contact

**Shangqing Xu** – ethanxx2021@163.com

Project link: [https://github.com/Ethanxx2021/dns_plus_plus](https://github.com/Ethanxx2021/dns_plus_plus)

---

*Last updated: July 2025*