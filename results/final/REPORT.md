# DNS++ 正式实验数据采集报告

- commit: `7cb5f3ccdac35fe546cfa898f294f9dbac2bf12b` (`feat(bench): add dynamics benchmark...`)
- 环境: AMD Ryzen 9 8945HS, **2 核**, Linux 7.0.0-30-generic, GMP 6.3.0, Release 构建
- 采集时间: 2026-08-24
- `git diff src/` 为空(全程未修改 `src/`)

---

## 关键数字(mean + bootstrap 95% CI,n=30 trials)

### 单 broker brake sweep(10 pub / 50 sub)

| brake | recall | stretch | suppression(实测 braked_local/pub_received) |
|---|---|---|---|
| 1 | 0.452 [0.404, 0.499] | 2.172 [1.970, 2.384] | 0.624 |
| 2 | 0.749 [0.699, 0.797] | 1.373 [1.262, 1.496] | 0.321 |
| 4 | 0.983 [0.965, 0.996] | 1.013 [1.001, 1.030] | 0.033 |
| 1000 | 1.000 [1.000, 1.000] | 1.000 [1.000, 1.000] | 0.000 |

### 多 broker brake sweep(20 pub / 50 sub / 3 broker)

| brake | recall | traffic ratio |
|---|---|---|
| 1 | 0.227 [0.201, 0.253] | 1.087 [1.082, 1.092] |
| 2 | 0.458 [0.424, 0.493] | 1.109 [1.104, 1.115] |
| 4 | 0.779 [0.753, 0.805] | 1.142 [1.136, 1.148] |
| 1000 | 0.981 [0.967, 0.992] | 1.168 [1.162, 1.174] |

### 订阅者规模扫描(brake=4)

| subs | recall | UDP RcvbufErrors 差值(实测) |
|---|---|---|
| 10 | 0.983 [0.953, 1.000] | 0 |
| 50 | 0.983 [0.965, 0.996] | 0 |
| 200 | 0.982 [0.966, 0.996] | 0 |
| 500 | 0.879 [0.812, 0.938] | **1818** |
| 1000 | 0.833 [0.746, 0.905] | **4963** |

### 明文 vs 加密(brake=4,10 pub / 50 sub)

| 指标 | 明文 | 加密 |
|---|---|---|
| recall | 0.983 [0.966, 0.996] | 0.983 [0.966, 0.996] |
| stretch | 1.013 [1.001, 1.030] | 1.013 [1.001, 1.030] |
| latency_per_pub(ms) | 63.58 [60.33, 67.13] | 63.40 [60.60, 66.30] |

- **Wilcoxon 配对检验(加密−明文 latency_per_pub):p=0.299,median 差值 = +0.38 ms(不显著)**
- 可观测性:加密 `match_calls=319, he_mode=1`;明文 `match_calls=0`(证明加密确实执行了同态匹配)

### dynamics: 副本迁移收敛(20 pub / 50 sub / 20 migrations / 30 trials,brake-free)

- **closer**: 收敛率 **0.963**(998/1036),收敛时间 trial 级 mean 0.56 ms [0.44, 0.69],median 0.42 ms
- **farther**: 收敛率 **0.000**(0/1391),系统性不收敛(设计使然)
- `braked=0`(测量未被 brake 污染)
- 加密 dynamics:closer 0.973 / farther 0.000(与明文一致)

### 密码学 microbenchmark(3 次取中位数)

- blindNotification: **23.5 ms/op**
- blindSubscription: **59.0 ms/op**
- executeMatch: **0.013 ms/op**
- keyGen: **20/20 次全部精确 2048 位**(修复验证点)

---

## 与论文现有数字的对比(哪些结论需要改写)

| 论文旧陈述 | 实测 | 结论 |
|---|---|---|
| 单 broker brake sweep 旧表(已撤回) | recall 0.452→1.0,brake 现在真正作用于本地投递 | 需用上表替换 |
| 多 broker recall ~0.94–0.96(所有 brake) | brake=1 时 recall **0.227**,brake=1000 时 0.981 | brake 语义修复后 recall 大幅下降,旧表作废 |
| §6.1 "60% suppression"(编的) | 实测 brake=1 单 broker suppression=**0.624** | 用实测替换(巧合地接近 60%,但现在有依据) |
| Phase 3 "加密 3.3× latency 开销(109→358ms)" | 明文 63.6ms vs 加密 63.4ms,**p=0.30,无显著差异** | **旧"3.3× 开销"不复现**。加密开销在客户端盲化(23–59ms,发布前一次性),broker 侧 executeMatch 仅 0.013ms,不在投递关键路径上 |
| §5.5 500 订阅者 recall 下降归因于"缓冲区溢出"(假设) | 实测 UDP RcvbufErrors:500 订阅者 **1818**,1000 订阅者 **4963** | 归因从"假设"升级为"实测确认" |
| dynamics 无数据 | closer 亚毫秒收敛(median 0.42ms),farther 不收敛 | 补上 dynamics 证据 |

---

## 异常与警告

1. **latency 绝对值受测量方法学污染(仅影响绝对值,不影响相对比较)**:
   `bench_broker` 先发完所有 10 条 publication(每条之间 `sleep 10ms`)再统一 poll。
   在 2 核机器上 `sleep 10ms` 会大幅 oversleep(实测单条可达 ~1s),导致 `latency_ms`
   和 `latency_per_pub_ms` 的"收到时刻"实际上是"poll 循环读到 socket 的时刻",
   包含了发布循环的时长。因此 **brake=1/2/4 的 latency 绝对值(数百~上千 ms)不可信**。
   **但明文 vs 加密是配对比较(同 seed 同结构),该污染对称抵消,差异结论(p=0.30,~0.4ms)仍成立**。
   论文如需报告绝对 latency,建议改用"逐条发→逐条收"的测量方式(超出本次范围)。

2. **farther 类系统性不收敛(rate=0)是设计使然,不是 bug**:迁移只重发被移动的副本,
   新最优副本既不重发、又比订阅者 `cached_closest_dist` 更远,push-only 的 Algorithm 1
   不会推它。论文 dynamics 声明应精确化为"副本**靠近**订阅者时亚毫秒收敛;副本**远离/消失**
   时当前设计不收敛,需后续更近 publication 或 TTL 兜底"。

3. **多 broker suppression 比值可能 >1**(brake=1 时 1.067):同一条 publication 可在多个
   broker 上分别被 braked,`(braked_local+braked_up)/pub_received` 对多 broker 不是干净的
   suppression 度量;多 broker 用 traffic ratio 更有意义。

4. 无进程崩溃、无 recall 恒 0、无 `braked>0` 污染(dynamics 确认 `braked=0`)。

---

## 复现

```bash
cd ~/projects/dns_plus_plus
git checkout 7cb5f3c
rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
# 全部测试
./build/test_tlv && ./build/test_geo && ./build/test_paillier && ./build/test_heps \
  && ./build/test_brake ./build/dns_broker \
  && ./build/test_encrypted_cross_broker ./build/dns_broker \
  && bash scripts/run_integration_test.sh
# 正式采集(单 broker / 多 broker / 规模 / 明文加密)
bash scripts/run_full_eval.sh --trials=30 --out=results/final
# dynamics(brake-free)
# (见任务 Step 3 命令)
# 统计 + 图
venv/bin/python scripts/aggregate_results.py results/final
venv/bin/python scripts/plot_all.py results/final
```

产物:`results/final/{summary.txt,numbers.tex,figures/*.png}` + 15 个 CSV + 2 个 dynamics CSV。
