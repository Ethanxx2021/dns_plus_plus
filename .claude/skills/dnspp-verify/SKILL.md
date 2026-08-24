---
name: dnspp-verify
description: DNS++ 项目的构建、测试与不变量检查流程。任何时候修改了 src/、benchmarks/、tests/、config/ 下的文件,或者被要求排查 broker 行为、brake 语义、同态加密匹配、地理计算、benchmark 复现性问题时,都必须使用本 skill。它规定了改动后必须执行的验证步骤、必须保持的系统不变量,以及 PR 描述的固定格式。即使任务看起来只是小改动,也要用它来做收尾验证。
---

# DNS++ 构建与验证

本 skill 描述改动 DNS++ 代码库后的标准验证流程,以及一组必须保持的系统不变量。

## 何时用

- 修改了 `src/`、`benchmarks/`、`tests/`、`config/` 中的任何文件之后
- 被要求排查 broker 的路由行为、brake 是否生效、加密匹配是否真的在跑
- 准备写 PR 描述之前

## 验证流程

按顺序执行,任何一步失败就停下来修,不要继续往下走。

### 1. 环境

```bash
which cmake g++ || sudo apt-get update && sudo apt-get install -y build-essential cmake libgmp-dev
ls /usr/include/gmpxx.h || sudo apt-get install -y libgmp-dev
```

### 2. 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j 2>&1 | tail -30
```

必须零 error。新增的 warning 要么修掉,要么在 PR 描述里说明为什么保留。

### 3. 单元测试

```bash
for t in test_tlv test_geo test_paillier test_heps; do
  echo "=== $t ==="; ./build/$t || echo "FAILED: $t";
done

# test_brake 会 fork 一个真实 dns_broker 子进程（验证 brake_scope 的本地/上行限流），
# 所以单列，并显式传入 broker 二进制路径：
./build/test_brake ./build/dns_broker || echo "FAILED: test_brake"
```

`test_paillier` 和 `test_heps` 会做 2048 位密钥生成,单次可能要几十秒,属正常。

### 4. 集成冒烟测试

```bash
bash scripts/run_integration_test.sh
```

这个脚本验证 Algorithm 1 的核心行为:伦敦和柏林两个订阅者,巴黎和华沙两个发布者;
巴黎(第一条 publication)两人都应收到,华沙只有柏林应该收到。

### 5. 内存与线程检查(改动过 `broker.cpp` 时必做)

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON \
  && cmake --build build-asan -j \
  && ./build-asan/test_tlv && ./build-asan/test_geo
```

### 6. 冒烟运行 benchmark(只看能否跑通,不看数字)

```bash
timeout 120 ./build/bench_broker 127.0.0.1 8080 5 10 4 1 42 0 > /tmp/smoke.csv 2>/tmp/smoke.err
head -3 /tmp/smoke.csv; cat /tmp/smoke.err
```

确认:进程正常退出、CSV 有表头和数据行、stderr 里没有异常。
**不要把这里的数字当作实验结果记录到任何文件里。**

## 系统不变量

改动之后必须仍然成立。如果某个改动会打破其中一条,那是设计决策,必须在 PR 描述里
单独说明,不能默默打破。

**I1 — Stretch 下界。** `stretch ≥ 1.0` 恒成立。若 benchmark 输出小于 1 的 stretch,
说明 ground truth 的最近副本计算有 bug(通常是距离函数不对称,或者跨反子午线)。

**I2 — 距离函数对称。** `geoDistance(A,B) == geoDistance(B,A)`,且经度差必须按
`fmod(dlon + 540, 360) - 180` 归一化,否则跨 ±180° 的点对会算出绕地球一圈的距离。

**I3 — 单调收敛。** 对固定的订阅者,`cached_closest_dist` 只能单调下降。
任何让它变大的代码路径都是 bug。

**I4 — 密文不能当索引键。** 盲化是语义安全的(同一明文两次盲化产生不同密文)。
因此 `bval_n` / `bval_m1` **不能**用作 `unordered_map` 的 key:那会导致每条消息落进
一个全新的桶,使缓存、去重、限流全部失效,并造成无界内存增长。
路由表必须用与密文分离的稳定标识来索引。

**I5 — 加密模式必须可观测。** 任何"加密"实验都必须能从 broker 的输出中证明它确实在
执行同态匹配(例如匹配调用次数计数器)。不允许出现"加密模式和明文模式的输出无法区分"
的情况。

**I6 — 不静默降级。** 密钥缺失、配置非法、前置条件不满足时一律 fail-fast,
不要退回到明文模式或默认值后继续运行。

**I7 — 随机性可复现。** benchmark 中所有随机放置必须由命令行种子控制,
且种子要出现在输出 CSV 的每一行。

**I8 — 论文一致性。** `docs/` 与 `README.md` 中对某个机制的描述,必须与 `src/` 中该机制的
实际行为一致。发现不一致时,以代码为准修正文档,并在 PR 描述里指出。

## 排查技巧

**想知道 brake 到底有没有生效**:brake 的调用点目前只在向上传播分支内,
被 `has_parent_` 保护。单 broker 配置下 `has_parent_ == false`,所以 brake 可能完全不执行。
排查方法是在 `brakeAllows()` 入口加一条日志,跑单 broker benchmark,看日志里有没有输出。

**想知道加密匹配有没有生效**:在 `executeMatch()` 入口加计数器,并在 broker 启动日志里
打印 `he_enabled_` 的值。如果加密实验跑完 `executeMatch` 调用次数为 0,说明 broker 走了
明文分支。

**想知道 UDP 丢包**:`ss -uanm` 或 `netstat -su | grep -i "receive errors"`,
在 benchmark 前后各取一次,差值就是丢包数。大量订阅者并发时内核接收缓冲区溢出是常见原因。

**想验证 Paillier 的 Match 数学**:`Match` 的结果应当是 `r_m·(x−v) + r (mod n)`。
`x == v` 时结果小于 `r_m`;`x > v` 时结果在 `[r_m, n/2)`;`x < v` 时结果在 `(n/2, n)`。
可以临时打印 `diff` 的位长来确认落在哪个区间。

## PR 描述格式

每个 PR 必须包含以下小节:

```markdown
## 改了什么
(一两句话)

## 为什么
(这解决的是哪个具体问题;如果是行为变更,说明改前改后的行为差异)

## 怎么验证的
(粘贴 build + 单元测试 + 集成测试的关键输出)

## 需要重跑的实验
(列出受影响的实验、对应论文章节、重跑命令。若无则写"无")

## 顺带发现
(本次没有修但发现的问题。若无则写"无")
```
