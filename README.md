# DNS++: A Privacy-First, Dynamic, Location-Aware Name Resolution System
**UCL MSc Computer Science – Final Project**
**Student:** Shangqing Xu
**Supervisor:** Professor Miguel Rio

---

## 📌 Project Status (May 2025)

### Current Prototype
A fully functional C++ broker implementing the core Pub/Sub and network layer of DNS++.

| Component               | Status | Description |
|--------------------------|--------|-------------|
| `epoll` event loop       | ✅     | Single-threaded, level-triggered; uses `epoll_wait` timeout for periodic TTL sweep. |
| Binary wire protocol     | ✅     | Fixed 12-byte `PubSubMsg` header (`msg_type`, `topic_id`, `payload`); zero-copy parsing via `reinterpret_cast`. |
| Pub/Sub routing          | ✅     | `unordered_map<topic, vector<client_addr>>` supporting multiple subscribers per topic. |
| Multicast forwarding     | ✅     | `sendto()` to all subscribers on publish. |
| Heartbeat & TTL cleanup  | ✅     | Clients send `type=3` heartbeat; broker tracks `topic_last_active`; topics idle >15s removed. |
| Logger thread            | ✅     | Producer-consumer with `std::mutex` + `std::condition_variable`; decoupled from main loop. |
| RAII resource management | ✅     | Socket closed in destructor; no manual `close()` leaks. |

---

## 🧠 Mapping to Research Paper (Rio et al.)
| Paper Contribution              | Implementation Status         | Next Steps |
|---------------------------------|-------------------------------|------------|
| Hybrid Pub/Sub                  | Subscribe/Publish done; Query type reserved | Add query-response caching |
| Privacy-preserving matching (HE)| `ICryptoEngine` interface designed; plaintext matching currently | Integrate UCL Paillier library (Phase 3) |
| Location‑aware routing          | Protocol header ready for `lat/lon` fields; unimplemented | Add coordinates + Closest algorithm (Phase 1) |
| Hierarchical overlay            | Single broker running         | Build 3‑node tree + propagation brake (Phase 2) |

---

## 🚀 Three-Month Plan

### Phase 1 – Spatial Routing (Weeks 1–4)
- Extend `PubSubMsg` with `float lat, lon` (or fixed-point representation).
- Implement paper’s **Algorithm 1 (Closest routing)** – brute-force distance scan.
- Validate on a single broker with multiple publishers at simulated coordinates.
- **Pre‑defense demo will show this phase complete.**

### Phase 2 – Hierarchical Topology (Weeks 5–8)
- Deploy 3 broker instances on different ports (1 parent, 2 children).
- Implement **propagation brake** and **MBH pruning**.
- Measure pruning effectiveness under synthetic workloads.

### Phase 3 – Cryptographic Integration (Weeks 9–12)
- Integrate UCL Paillier library via the `ICryptoEngine` interface.
- Replace plaintext topic matching with blinded ciphertext inequality checks.
- Benchmark end‑to‑end latency and throughput (QPS) with and without encryption.
- Document performance trade‑offs for final thesis.

---

## 🛠 Technical Highlights (Engineering Innovations)

| Feature                     | Description |
|-----------------------------|-------------|
| Zero‑copy parsing           | `(PubSubMsg*)buffer` – no `memcpy`, single pointer cast after `recvfrom`. |
| Network byte‑order safety   | All integers converted with `htons`/`ntohs` on send, `ntohs` on receive. |
| TTL via `epoll_wait` timeout| No additional timer FD; cleanup sweep runs every 10 s inside the event loop. |
| Modular crypto interface    | `ICryptoEngine` with `blind()` and `match()` – network layer agnostic to concrete cipher. |
| Thread‑safe logging         | Dedicated logger thread avoids blocking the `epoll` loop. |

---

## 🔬 Academic Contribution (Thesis Scope)

This project provides the **first real‑system implementation and performance evaluation** of the DNS++ broker architecture (originally simulated in Java).
Specifically it contributes:

1. **Real‑network validation** – demonstrates low‑latency Pub/Sub routing with `epoll` on commodity Linux.
2. **Concurrency analysis** – quantifies the impact of thread contention, kernel buffer limits, and polling overhead on the paper’s propagation brake and FPR trade‑offs.
3. **Seamless crypto integration** – designs a modular interface that decouples spatial routing from homomorphic matching, enabling independent benchmarking of the privacy‑performance trade‑off.

