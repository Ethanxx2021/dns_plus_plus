# DNS++: A Privacy-First, Dynamic, Location-Aware Name Resolution System

**UCL MSc Internet Engineering – Final Project (ELEC0054)**
**Student:** Shangqing Xu | **Supervisor:** Professor Miguel Rio

[![Language](https://img.shields.io/badge/C%2B%2B-17-blue)]()
[![Platform](https://img.shields.io/badge/platform-Linux-orange)]()
[![Crypto](https://img.shields.io/badge/Crypto-Paillier%202048--bit-red)]()
[![Trials](https://img.shields.io/badge/eval-30%20trials%2C%2095%25%20bootstrap%20CI-brightgreen)]()

---

## Overview

DNS++ is a redesign of Internet name resolution that addresses three limitations of the traditional DNS: **privacy leakage** to intermediaries, **inability to keep up** with dynamic service changes, and **lack of location awareness** for replica selection.

This repository contains a **from-scratch C++17 implementation** of the DNS++ broker architecture, originally proposed and evaluated only in a Java simulation. It is the code artefact behind the thesis, and every number in the thesis is produced by this code and reproducible from `results/final/`.

The headline result is a correction of the design's claims, measured on real hardware: the propagation brake is an **overload valve**, not an efficiency knob; homomorphic matching adds **no measurable broker-side latency**; and push-only convergence is **directional** — sub-millisecond when a replica moves closer, absent when it moves away.

### Key Findings

1. **The brake is a poor bargain in a tree.** In a three-broker tree, tightening `brake_limit` from 1000 to 1 cuts the traffic ratio only 1.168 → 1.087 (6.9%) while destroying 77% of correct nearest-replica resolutions (recall 0.981 → 0.227). The **quadrant cache** — which suppresses *selectively* rather than *by rate* — is the mechanism that does the useful filtering; alone it reaches a traffic ratio of 1.168 at 0.981 recall.
2. **Encryption is free on the broker critical path.** A 30-trial paired comparison finds no detectable per-publication latency difference between plaintext and 2048-bit encrypted matching (Wilcoxon $p = 0.299$, median paired difference $+0.38$ ms). A single homomorphic `executeMatch` costs 0.0127 ms against 23.5–59 ms of endpoint blinding, bounding broker overhead at $S \times 0.0127$ ms in the number of subscription groups $S$.
3. **Convergence is directional.** After a replica migration, *closer* moves converge in a median of 0.42 ms (96.3% of 1036 cases) — five orders of magnitude faster than a DNS TTL — but *farther* moves never converge (0 of 1391 cases): the push-only, strictly-improving monotone filter keeps stale state when a replica moves away.

These findings, and the plaintext exposure introduced by hash-based subscription grouping, are reported as the principal limitations of the design as implemented.

---

## Current Status

| Component | Status | Description |
|-----------|--------|-------------|
| TLV binary protocol | ✅ Done | Extensible Type-Length-Value wire format with zero-copy parsing |
| `epoll` event loop | ✅ Done | Level-triggered, single-threaded with timeout-driven TTL cleanup |
| Algorithm 1 (Proximity Routing) | ✅ Done | Closest-replica cache, quadrant brake, query_mode, Match gate |
| Multi-broker hierarchical overlay | ✅ Done | 3-broker tree with config-driven topology, HELLO handshake |
| Dynamic MBH region aggregation | ✅ Done | Bottom-up MBH computation with REGION_UPDATE propagation |
| Cross-broker routing | ✅ Done | Upward (brake-limited) + downward (quadrant-filtered) propagation |
| Modified Paillier (2048-bit) | ✅ Done | Key reversal, blinding, $n/2$ threshold Match, GMP |
| HEPS trusted service | ✅ Done | Key generation and blinding parameter distribution |
| Encrypted broker routing | ✅ Done | `executeMatch()` re-checks hash-based grouping (see limitations) |
| Brake-sweep benchmark | ✅ Done | Single- and multi-broker, 30 trials, bootstrap CIs |
| Scalability sweep | ✅ Done | 10–1000 subscribers; recall degrades beyond 200 via UDP drops |
| Plaintext-vs-encrypted benchmark | ✅ Done | Paired Wilcoxon: no detectable difference ($p=0.299$) |
| Dynamics benchmark | ✅ Done | Replica-migration convergence (closer 96.3%, farther 0%) |
| Crypto micro-benchmark | ✅ Done | `executeMatch` 0.0127 ms/op; keygen 20/20 at 2048 bits |
| Unit tests | ✅ Done | TLV, Geo, Paillier, HEPS, Brake, crypto-microbench, encrypted-cross-broker |
| Statistics & figures | ✅ Done | Bootstrap CI, Wilcoxon, tables.tex/numbers.tex, 8 figures |
| Thesis / report | ✅ Done | See `DNS++.pdf` (and `main_fixed.tex` source) |

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

### Multi-Broker Topology

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

The system uses a **TLV (Type-Length-Value)** binary protocol over UDP.

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
| `STATS_DATA` | 0x0006 | 32 bytes | `4×uint64_t` traffic counters (legacy) |
| `STATS_DATA_EXT` | 0x0007 | 96 bytes | `12×uint64_t`: forward_up, forward_down, delivered_local, braked, match_calls, match_hits, pub_received, sub_received, sub_groups, he_mode, braked_up, braked_local |
| `BLINDED_VALUE` | 0x0010 | Variable | Paillier-blinded notification |
| `BLINDED_VALUE_HI` | 0x0011 | Variable | Paillier-blinded subscription `v+1` |

---

## Quick Start

### Build

```bash
git clone https://github.com/Ethanxx2021/dns_plus_plus.git
cd dns_plus_plus
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Run Tests

```bash
./build/test_tlv && ./build/test_geo && ./build/test_paillier && ./build/test_heps
./build/test_brake ./build/dns_broker        # forks a real broker
./build/test_crypto_microbench
./build/test_encrypted_cross_broker
```

### Run Encrypted Single-Broker

```bash
# Terminal 1: broker (writes /tmp/dnspp_heps_full.key)
./build/dns_broker configs/single.conf

# Terminal 2: subscriber in London
./build/test_client sub 127.0.0.1 8080 weather.example 51.5 -0.1

# Terminal 3: publisher in Paris
./build/test_client pub 127.0.0.1 8080 weather.example 48.8 2.3 Paris-edge-1
```

`test_client` blinds its messages by default (requires the broker to have written
`/tmp/dnspp_heps_full.key`). Append `--plaintext` (alias `--no-encrypt`) to skip blinding.

### Run Multi-Broker Tree

```bash
./build/dns_broker configs/root.conf    # root (port 9000)
./build/dns_broker configs/leaf1.conf   # leaf 1, London (9001)
./build/dns_broker configs/leaf2.conf   # leaf 2, Berlin (9002)

./build/test_client sub 127.0.0.1 9001 weather.example 51.5 -0.1
./build/test_client pub 127.0.0.1 9002 weather.example 52.2 13.0 Berlin-edge-1
```

### Reproduce the Thesis Results

Every table and figure in the thesis is produced by the driver below against a **fresh
broker per configuration**, 30 trials per cell, 3 warm-up rounds, and a per-trial derived
seed (`seed + trial_index`). It skips outputs that already exist, so it resumes after
interruption:

```bash
bash scripts/run_full_eval.sh --trials=30 --out=results/final
```

The authoritative numbers are regenerated from the CSVs by:

```bash
python3 scripts/aggregate_results.py   # per-cell stats + summary.txt
python3 scripts/bench_stats.py         # bootstrap CIs + Wilcoxon -> numbers.tex
python3 scripts/generate_tables.py     # tables.tex
python3 scripts/plot_all.py            # figures/
```

`results/final/summary.txt`, `numbers.tex` and `tables.tex` are committed and are the
single source of truth for the thesis numbers. `results/final/env.txt` records the
machine metadata (CPU model, core count, kernel, GMP version, git commit) at collection time.

> **Benchmarks must run against a fresh broker per configuration** (invariant I9): a
> single long-lived broker makes whichever mode runs second inherit a warm broker while
> the first suffers a cold-start outlier. `run_full_eval.sh` enforces this.

### Brake Sweep (single broker)

`brake_limit` lives in the broker config, not on the benchmark CLI. Sweep it by restarting
the broker per value (the driver does this automatically):

```bash
for L in 1 2 4 1000; do
  sed "s/^brake_limit=.*/brake_limit=$L/" configs/sweep.conf > /tmp/sweep_$L.conf
  ./build/dns_broker /tmp/sweep_$L.conf &
  BPID=$!; sleep 1
  ./build/bench_broker 127.0.0.1 8080 10 50 $L 5 42 0 --warmup=3 > brake_$L.csv 2> brake_$L.log
  kill $BPID; wait $BPID 2>/dev/null
done
```

### Dynamics Benchmark (replica-migration convergence)

```bash
# IMPORTANT: brake-free. A throttled publication is indistinguishable from a
# failure to converge, so use brake_limit=1000; the benchmark warns on braked>0.
sed 's/^brake_limit=.*/brake_limit=1000/' configs/single.conf > /tmp/dyn.conf
./build/dns_broker /tmp/dyn.conf &
sleep 1
./build/bench_dynamics 127.0.0.1 8080 20 50 20 30 42 0 --timeout-ms=1000 \
    > dynamics.csv 2> dynamics.log
kill %1
```

Two migration classes are distinguished (this matters for Algorithm 1's per-subscriber
closest filter, whose `cached_closest_dist` only ever decreases):

- `closer` — the migrated replica became the new closest; convergence time is measurable.
- `farther` — the previous closest replica moved away, so the true optimum shifts to a
  *different* replica that may never have been pushed. Under push-only Algorithm 1 these
  cases **never converge** — a design property, not a bug.

---

## Project Structure

```
dns_plus_plus/
├── README.md
├── AGENT.md / CLAUDE.md      # agent working conventions
├── CMakeLists.txt
├── main_fixed.tex            # thesis LaTeX source (TMLR template)
├── DNS++.pdf                 # compiled thesis/report
├── configs/
│   ├── root.conf             # root broker
│   ├── leaf1.conf            # leaf 1 (London)
│   ├── leaf2.conf            # leaf 2 (Berlin)
│   ├── single.conf           # single broker (port 8080)
│   └── sweep.conf            # sweep template (brake_scope=both)
├── docs/
│   ├── learning_log.md       # development journal
│   └── audit_2026-08.md      # brake_scope semantics audit (Q1) + fixes
├── src/
│   ├── broker/{broker.h,broker.cpp}     # event loop, handlers, Algorithm 1
│   ├── broker/main.cpp                  # config parsing & startup
│   ├── protocol/                        # TLV codec
│   ├── crypto/{Paillier,Heps}.{h,cpp}   # modified Paillier + HEPS
│   ├── common/geo.h                     # Region/MBH, distance, quadrant
│   └── logger/logger.h                  # async logger
├── clients/test_client.cpp              # CLI test client (sub/pub/beat)
├── tests/
│   ├── test_tlv.cpp, test_geo.cpp
│   ├── test_paillier.cpp, test_heps.cpp
│   ├── test_brake.cpp                   # brake_scope gating (spawns a broker)
│   ├── test_crypto_microbench.cpp       # per-op crypto timings
│   └── test_encrypted_cross_broker.cpp  # cross-broker double-hash regression
├── benchmarks/
│   ├── bench_broker.cpp                 # single-broker benchmark
│   ├── bench_multi_broker.cpp           # multi-broker benchmark (auto-fork)
│   ├── bench_dynamics.cpp               # migration-convergence benchmark
│   └── bench_common.h
├── scripts/
│   ├── run_full_eval.sh                 # full sweep driver (fresh broker per config)
│   ├── run_integration_test.sh
│   ├── aggregate_results.py             # summary.txt
│   ├── bench_stats.py                   # bootstrap CI + Wilcoxon -> numbers.tex
│   ├── generate_tables.py               # -> tables.tex
│   ├── generate_appendix_tables.py
│   ├── generate_evidence.py
│   └── plot_all.py, plot_results.py, plot_multi_broker.py,
│       plot_crypto.py, plot_sweep.py    # figures
└── results/
    ├── final/                 # authoritative thesis data (see below)
    ├── phase1/ phase2/ phase3/  # historical/withdrawn intermediate data
```

`results/final/` holds the committed, authoritative outputs: per-configuration CSVs and
logs (`single_brake_*.csv`, `multi_brake_*.csv`, `sweep_*.csv`, `plain.csv`,
`encrypted.csv`, `dynamics_plain.csv`, `dynamics_encrypted.csv`, `microbench_run*.txt`),
the summaries (`summary.txt`, `numbers.tex`, `tables.tex`, `REPORT.md`), the figures
(`figures/`), and the environment record (`env.txt`).

---

## Mapping to DNS++ Research Paper

| Paper Section | Component | Status |
|---------------|-----------|--------|
| §3.1 Overlay Creation | Hierarchical broker tree, MBH aggregation | ✅ Multi-broker tree + dynamic MBH |
| §3.2 HE Privacy Primitives | Modified Paillier, Match/Cover, HEPS | ✅ 2048-bit Paillier, $n/2$ Match, HEPS |
| §3.3 Protocol Workflow | IT[]/OT[] tables, cross-broker forwarding | ✅ Flat index + forwarding tables |
| §3.4 Proximity Routing | Algorithm 1: closest cache, brake, query_mode | ✅ Complete (incl. Match gate) |
| §3.5 Spatial Discovery | Algorithm 2: region containment, FPR aggregation | 📋 Future work |
| §4.2 Proximity Evaluation | Stretch, recall, traffic ratio | ✅ `results/final/` |
| §4.3 Spatial Evaluation | Table size, FPR trade-offs | 📋 N/A (Algorithm 2 deferred) |
| §4.4 GPU Acceleration | CUDA Paillier matching | ❌ Out of scope (discussed in thesis) |
| §5 Dynamics | Migration convergence | ✅ `bench_dynamics` (directional) |

---

## Evaluation Results

All results below: 30 independent trials, 3 warm-up rounds, 95% bootstrap confidence
intervals (10,000 resamples, percentile method). Collected on Ubuntu Linux, kernel
7.0.0-30-generic, AMD Ryzen 9 8945HS (2 cores), GMP 6.3.0, GCC 13.3, Release build.
Seed 42. Full environment record in `results/final/env.txt`.

### Single-Broker Brake Sweep (10 publishers, 50 subscribers)

| Brake limit | Recall [95% CI] | Stretch [95% CI] | Suppression |
|-------------|-----------------|------------------|-------------|
| 1    | 0.452 [0.405, 0.499] | 2.172 [1.970, 2.385] | 0.624 |
| 2    | 0.749 [0.699, 0.797] | 1.373 [1.261, 1.497] | 0.321 |
| 4    | 0.983 [0.965, 0.996] | 1.013 [1.001, 1.030] | 0.033 |
| 1000 (∞) | 1.000 [1.000, 1.000] | 1.000 [1.000, 1.000] | 0.000 |

Suppression is the *measured* `braked_local / pub_received` rate, not inferred from recall.

### Multi-Broker Brake Sweep (3-broker tree, 20 publishers, 50 subscribers)

| Brake limit | Recall [95% CI] | Stretch [95% CI] | Traffic ratio [95% CI] |
|-------------|-----------------|------------------|------------------------|
| 1    | 0.227 [0.201, 0.253] | 3.331 [3.038, 3.640] | 1.087 [1.082, 1.092] |
| 2    | 0.458 [0.424, 0.493] | 2.086 [1.885, 2.301] | 1.109 [1.104, 1.115] |
| 4    | 0.779 [0.751, 0.805] | 1.318 [1.237, 1.409] | 1.142 [1.136, 1.148] |
| 1000 (∞) | 0.981 [0.966, 0.992] | 1.013 [1.003, 1.027] | 1.168 [1.162, 1.174] |

Tightening from ∞ to 1 buys a 6.9% traffic-ratio reduction while recall collapses from
0.981 to 0.227 — the brake is an **overload valve**, not an efficiency knob. The quadrant
cache already provides the useful (information-aware) filtering.

### Subscriber-Count Scaling (brake = 4, 10 publishers)

| Subscribers | Recall [95% CI] | Stretch [95% CI] | UDP RcvbufErrors |
|-------------|-----------------|------------------|------------------|
| 10   | 0.983 [0.953, 1.000] | 1.007 [1.000, 1.021] | 0 |
| 50   | 0.983 [0.965, 0.996] | 1.013 [1.001, 1.030] | 0 |
| 200  | 0.982 [0.966, 0.995] | 1.014 [1.004, 1.026] | 0 |
| 500  | 0.879 [0.811, 0.939] | 1.017 [1.005, 1.033] | 1818 |
| 1000 | 0.833 [0.747, 0.904] | 1.017 [1.005, 1.031] | 4963 |

Recall holds at 0.98 up to 200 subscribers. Beyond that it degrades **while stretch does
not move**, which identifies the cause as kernel UDP receive-buffer overflow (a
single-threaded harness polling hundreds of sockets on two cores) rather than routing
error — a real-system effect a discrete-event simulation cannot exhibit.

### Privacy Layer Overhead (10 publishers, 50 subscribers, brake = 4)

| Mode | Recall [95% CI] | Stretch [95% CI] | Latency mean [95% CI] | Median |
|------|-----------------|------------------|-----------------------|--------|
| Plaintext | 0.983 [0.965, 0.996] | 1.013 [1.001, 1.030] | 63.6 [60.3, 67.2] ms | 62.6 ms |
| Encrypted | 0.983 [0.965, 0.996] | 1.013 [1.001, 1.030] | 63.4 [60.5, 66.4] ms | 63.0 ms |

Paired Wilcoxon signed-rank (encrypted − plaintext): $W=182$, $z=-1.04$, **$p=0.299$**,
median paired difference $+0.38$ ms. No detectable difference. The encrypted runs were
verifiably encrypted: 319 `executeMatch` invocations vs 0 in plaintext.

| Operation | Executed by | Median (ms/op) |
|-----------|-------------|----------------|
| `blindNotification` | Publisher | 23.474 |
| `blindSubscription` | Subscriber | 58.967 |
| `executeMatch` | Broker | 0.0127 |

A broker does one `executeMatch` per *subscription group* per publication, so broker-side
cost is bounded by $S \times 0.0127$ ms — at most $50 \times 0.0127 = 0.635$ ms in the
worst case measured. The 30 paired differences have sd 4.37 ms, giving a minimum
detectable effect of 2.23 ms at $\alpha=0.05$, 80% power — so the null result is exactly
what the micro-benchmark predicts, and the endpoint blinding (off the routing path)
dominates the cost by three orders of magnitude.

### Dynamics: Replica-Migration Convergence (20 publishers, 50 subscribers, brake-free)

| Migration class | Events | Converged | Median | p95 |
|-----------------|--------|-----------|--------|-----|
| closer  | 1036 | 998 (96.3%) | 0.42 ms | 1.24 ms |
| farther | 1391 | 0 (0.0%) | — | — |

*Closer* migrations converge in a median of 0.42 ms — five orders of magnitude faster
than a typical 300 s DNS TTL. *Farther* migrations never converge: updates are push-only
and gated on strict improvement, so a subscriber whose replica moved away keeps its stale
minimum until a closer publication happens to arrive or the subscription expires.

---

## Known Limitations (as implemented)

These are reported honestly in the thesis rather than glossed over:

1. **Hash-based grouping exposes the name.** Blinded values are semantically secure and
   cannot serve as a lookup key, so the broker indexes routing state by
   `hashServiceName(plaintext)`. The ingress broker therefore learns the service name,
   and the homomorphic Match becomes a formal re-check rather than the mechanism deciding
   routing. The 64-bit unkeyed digest is enumerable and not protection by itself.
2. **One-directional convergence.** The monotone filter gives sub-millisecond convergence
   when replicas move closer and none when one moves away (above).
3. **Scale ceiling in the harness.** Recall degrades beyond 200 subscribers via kernel UDP
   drops, not routing error (above).
4. **`std::hash` is implementation-defined.** Routing decisions depend on the standard
   library version as well as the seed; a deployment should use a specified hash.
5. **Timing side channel.** Secret-exponent exponentiations use `mpz_powm`, not the
   constant-time variant.
6. **No message authentication.** Any host can forge HELLO / REGION_UPDATE / PUBLISH and
   corrupt MBH aggregation.
7. **Parameter drift.** The hash width $l$ (64 bits) and the stretching factor $r_m$
   (992 bits) are set independently; the original $u-l$ derivation assumed a 32-bit hash
   and was not revisited. The correctness constraint still holds with a $\sim2^{990}$
   margin (Appendix C of the thesis).

---

## Development Roadmap

- [x] Phase 1 — Spatial routing foundation (TLV, closest cache, brake, query-mode)
- [x] Phase 2 — Hierarchical topology (HELLO, MBH, cross-broker forwarding, stats)
- [x] Phase 3 — Cryptographic integration (modified Paillier, HEPS, `executeMatch`)
- [x] Benchmark reproducibility (per-trial seeds, warm-up, env metadata, UDP deltas)
- [x] `bench_dynamics` migration-convergence benchmark
- [x] Cross-broker double-hash encryption fix
- [x] Formal data collection: 30 trials, bootstrap CIs, Wilcoxon, tables + 8 figures
- [x] Thesis / report written and compiled (`DNS++.pdf`)
- [ ] (Future) Algorithm 2 — spatial discovery / FPR aggregation
- [ ] (Future) Cover protocol to recover true name privacy
- [ ] (Future) Soft state / invalidation for bidirectional convergence
- [ ] (Future) Separated HEPS (currently co-located with the root broker)

---

## Engineering Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Transport | UDP (`SOCK_DGRAM`) | Low-latency, connectionless — mirrors real DNS |
| I/O model | `epoll` (level-triggered) | Efficient multiplexing; timeout doubles as TTL timer |
| Protocol format | TLV | Extensible without breaking changes |
| Service name type | `std::string` | Paper supports any URI |
| Distance function | Equirectangular approximation | Single `cos()`; sufficient for ranking |
| Threading | Single-threaded main + async logger | Lock-free routing; logger isolated |
| Crypto library | GMP | Standard for 2048-bit modular arithmetic |
| Crypto integration | `executeMatch()` in broker | Decouples routing from crypto |
| Topology config | `key=value` files | No YAML dependency; trivial to parse |
| Broker spawning | `fork()` in benchmark | Real process isolation (fresh broker per config) |
| RNG discipline | per-trial `seed + trial_index` | Independent trials; no shared-seed repetition |
| Statistics | bootstrap CI + Wilcoxon | Bounded metrics; paired comparison |
| Fail-fast | exit on bad config / missing HE key | No silent plaintext fallback in experiments |

---

## Academic Context

### Research Gap

The DNS++ design [Rio et al.] promises privacy, dynamics, and locality in a single
system — but had only been evaluated in a Java simulation. This work builds it as a real
system and measures which claims survive contact with an implementation.

### What the Measurement Establishes

- ✅ **Proximity routing is correct without throttling** — recall and stretch are exactly
  1.000 at `brake_limit=1000`.
- ✅ **The brake is an overload valve** — 6.9% traffic saved for 77% of correct
  resolutions lost in a three-broker tree; the quadrant cache is the useful filter.
- ✅ **Homomorphic matching is cheap on the critical path** — no detectable latency
  difference ($p=0.299$), broker cost bounded at $S \times 0.0127$ ms.
- ⚠️ **Convergence is directional** — sub-millisecond when closer, absent when farther.
- ⚠️ **Privacy is partial as implemented** — hash-based grouping exposes the name to the
  ingress broker; the homomorphic Match is a re-check, not the routing mechanism.

---

## Contact

**Shangqing Xu** – ethanxx2021@163.com

Project link: [https://github.com/Ethanxx2021/dns_plus_plus](https://github.com/Ethanxx2021/dns_plus_plus)

---

*Last updated: August 2026*
