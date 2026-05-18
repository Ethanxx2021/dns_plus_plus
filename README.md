# DNS++: A Privacy-First, Dynamic, Location-Aware Name Resolution System

**UCL MSc Computer Science – Final Project**
**Student:** Shangqing Xu | **Supervisor:** Professor Miguel Rio

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()

---

## 📌 Project Status (May 2025)

This repository contains a **fully functional C++ broker** implementing the core Pub/Sub and network layer of the DNS++ name resolution architecture. The system runs an `epoll`-based event loop, uses a custom binary protocol, and supports multicast routing with heartbeat/cleanup logic. All code is modular, CMake-built, and ready for Phase 1 extensions.

### Current Prototype Capabilities

| Component | Status | Description |
|-----------|--------|-------------|
| `epoll` event loop (level‑triggered) | ✅ | Single‑threaded, timeout‑driven periodic TTL cleanup |
| Binary wire protocol (`PubSubMsg`) | ✅ | Fixed 12‑byte header, zero‑copy parsing |
| Pub/Sub routing & multicast | ✅ | `unordered_map<topic, vector<client_addr>>` |
| Heartbeat & TTL garbage collection | ✅ | Default 15‑s idle expiry, automatic removal |
| Asynchronous logger thread | ✅ | Producer‑consumer with `std::mutex` + `std::condition_variable` |
| RAII socket management | ✅ | Socket closed in destructor, no manual `close()` |
| Modular directory structure | ✅ | `src/broker/`, `src/protocol/`, `src/logger/`, `CMake` build |

---

## 🚀 Quick Start

```bash
# clone
git clone https://github.com/Ethanxx2021/dns_plus_plus.git
cd dns_plus_plus

# build
mkdir -p build && cd build
cmake ..
make

# run broker
./dns_broker
```

Test with a simulated publisher (send binary message to subscribed topic):
```bash
printf '\x00\x01\x22\xb8\x00\x00\x00\x00\x00\x00\x00\x00' | nc -u 127.0.0.1 8080
```

---

## 📁 Project Structure

```
dns_plus_plus/
├── README.md
├── CMakeLists.txt
├── .gitignore
├── docs/                    # Learning log, design notes
├── src/
│   ├── broker/              # Broker class, event loop
│   ├── protocol/            # PubSubMsg header, byte-order helpers
│   ├── logger/              # Thread-safe logger
│   ├── crypto/              # ICryptoEngine interface (future)
│   ├── routing/             # Routing table + spatial index (Phase 1)
│   ├── utils/               # Network utility functions
│   └── main.cpp             # Entry point
├── clients/                 # Test clients
├── tests/                   # Unit tests
├── benchmarks/              # Performance benchmarks
└── external/                # Third-party libraries (e.g., Paillier ref)
```

---

## 🧠 Mapping to DNS++ Research Paper

| Paper Contribution | Implementation Status | Next Steps |
|-------------------|----------------------|------------|
| Hybrid Pub/Sub model | Subscribe/Publish implemented; Query type reserved | Add query–response caching |
| Privacy‑preserving matching (HE) | `ICryptoEngine` interface designed; currently plaintext | Implement modified Paillier from paper (Phase 3) |
| Location‑aware routing | Protocol header ready for `lat/lon` fields | Add coordinates + Closest algorithm (Phase 1) |
| Hierarchical broker overlay | Single broker running | Build 3‑node tree + propagation brake (Phase 2) |
| HEPS as trusted third party | Simulated as a placeholder | Distributed HEPS design in thesis discussion |

---

## 🗓️ Development Roadmap

### Phase 1 – Spatial Routing (Weeks 1–4)
- Extend `PubSubMsg` with `float lat, lon`
- Implement **Algorithm 1 (Closest routing)** – brute‑force distance scan
- Validate on single broker with multiple simulated publishers

### Phase 2 – Hierarchical Topology (Weeks 5–8)
- Deploy 3 broker instances (1 parent, 2 children)
- Implement **propagation brake** and **MBH pruning**
- Measure traffic reduction and stretch metrics

### Phase 3 – Cryptographic Integration (Weeks 9–12)
- Implement **modified Paillier cryptosystem** from paper Section 3.2
- Replace plaintext topic matching with blinded ciphertext inequality checks
- Benchmark latency/throughput (QPS) with and without encryption
- Analyse performance trade‑offs for final thesis

---

## ⚙️ Engineering Innovations

- **Zero‑copy parsing**: `(PubSubMsg*)buffer` after `recvfrom()` avoids `memcpy`
- **Byte‑order safety**: `htons`/`ntohs` on all integer fields
- **Timeout‑based TTL**: `epoll_wait` timeout eliminates extra timer FDs
- **Modular crypto interface**: `ICryptoEngine` decouples network routing from cipher
- **Thread‑safe logging**: dedicated logger thread keeps main loop responsive

---

## 🔬 Academic Contribution

This thesis provides the **first real‑system C++ implementation and empirical evaluation** of the DNS++ broker architecture (previously simulated in Java). Specific contributions:

1. **Real‑network validation** – demonstrates low‑latency Pub/Sub on commodity Linux with `epoll`
2. **Concurrency analysis** – quantifies impact of thread contention, kernel buffer limits, and polling overhead on the paper’s propagation brake / FPR trade‑offs
3. **Seamless crypto integration** – designs a modular interface that isolates homomorphic matching, enabling independent benchmarking of privacy vs performance

---

## 📧 Contact

Shangqing Xu – ethanxx2021@163.com
Project link: [https://github.com/Ethanxx2021/dns_plus_plus](https://github.com/Ethanxx2021/dns_plus_plus)

---

<<<<<<< Updated upstream
*Last updated: May 2025*
=======
*Last updated: May 2025*
>>>>>>> Stashed changes
