# AGENTS.md — DNS++ 项目上下文与验证流程

> 这个文件是给任何在这个仓库里工作的编码 agent 看的(不限于特定厂商的工具)。
> 放在仓库根目录。开始任何改动之前先完整读一遍这个文件。

---

## 第一部分:项目是什么

这是 UCL MSc Internet Engineering 的毕业设计(ELEC0054),**不是普通的软件项目**。
仓库产出会被写进一篇学位论文并接受学术评审。这决定了两条最高优先级原则:

1. **代码必须与论文中的描述一致。** 任何改变系统行为的修改,都必须同步更新
   `docs/` 下的对应说明,并在提交信息里明确写出「这会导致论文哪一节/哪张表的
   数据作废,需要重跑」。
2. **可复现性高于一切。** 所有随机性必须可通过种子控制;所有实验参数必须
   出现在输出的 CSV 里;任何"魔数"必须有注释说明其数学来源。

### 项目是什么

DNS++ 是对互联网名字解析的重新设计,用一个**分层 publish/subscribe overlay**
替代传统 DNS,同时解决三个问题:隐私(同态加密匹配,broker 学不到查询的名字)、
动态性(推送式更新)、位置感知(把订阅者导向最近的服务副本)。

原始设计(Rio et al., SIGCOMM'26)只有 Java 仿真。本仓库是**第一个真实系统实现**
(C++17),研究贡献在于「在真实硬件上测量该设计的可行性与加密层的开销」。

### 三个角色

- **Publisher**:服务副本,发 `PUBLISH` 宣告自己的名字 + 经纬度
- **Subscriber**:客户端,发 `SUBSCRIBE` 请求某个名字,期望收到**最近的**副本
- **Broker**:overlay 节点,组织成树,负责匹配与路由

### 核心算法

- **Algorithm 1 (Proximity Routing)**:每个订阅者维护 `cached_closest_dist`,
  broker 只在新 publication 比该订阅者已知的最近副本更近时才投递。
- **Propagation brake**:按象限的滑动窗口限流器,限制 publication 的传播,
  用 recall 换 traffic。现在受 `brake_scope`(upward/local/both)控制,
  默认 both,已修复过"单 broker 下 brake 完全不生效"的历史 bug。
- **Quadrant cache**:父 broker 对每个子 broker 的每个象限维护最近距离,
  过滤向下传播。
- **MBH (Minimum Bounding Hyperrectangle)**:每个 broker 的空间覆盖范围,
  自底向上聚合。
- **Modified Paillier**:`Match(bval_n, bval_m)` 让 broker 在不解密的前提下
  判断 publication 的名字是否等于 subscription 的名字。

### 一个必须知道的当前设计状态(重要,不要重新"发现"或悄悄改动)

加密模式下,broker 路由表的 key 是 `hashServiceName(明文服务名)`,**不是密文**。
这是一次有意的、已记录的权衡(见 commit `f3cfeaa` 的 message):它让同一服务名的
订阅者能正确合并成一个组(否则语义安全的密文每次都不同,分组会失效),代价是
broker 直接看到明文名字,同态匹配退化为形式复核而非真正决定路由的机制。
**这个隐私含义已经在论文里作为 limitation 讨论,不要在不了解全局的情况下
"修复"它、也不要恢复成密文当 key 的旧写法。**

跨 broker 的加密路由目前还有一个已知缺陷:子 broker 转发时 SERVICE_NAME 字段
已经是 `hashServiceName(name)`,父 broker 收到后又哈希了一次,导致
`child_active_`(存的是单次哈希)与向下传播查表用的 `pub_key`(双重哈希)
对不上。这是接下来任务要修的,见下面的任务队列。

---

## 第二部分:代码结构

```
src/
  broker/broker.{h,cpp}    # 核心:epoll 事件循环 + 所有消息处理器
  main.cpp                 # 配置解析(parseConfig,fail-fast)与启动
  protocol/                # TLV 编解码
  common/geo.h 或 utils/geo.h  # Region / geoDistance / quadrant(确认实际路径)
  crypto/Paillier.{h,cpp}  # 修改版 Paillier,2048 位稳定,RNG 已提升为成员
  crypto/Heps.{h,cpp}      # 密钥服务封装,loadState 返回 bool、fail-fast
benchmarks/
  bench_broker.cpp         # 单 broker 实验
  bench_multi_broker.cpp   # 多 broker 实验(fork 三个 broker)
tests/                     # test_tlv / test_geo / test_paillier / test_heps /
                            # test_brake / test_crypto_microbench
scripts/                   # 集成测试(run_integration_test.sh)与绘图
configs/                   # root.conf / leaf1.conf / leaf2.conf / sweep.conf /
                            # single.conf,均已支持 brake_scope、require_he 字段
docs/
  audit_2026-08.md          # 完整代码审计报告,改动前必读
  learning_log.md           # 开发日志
```

---

## 第三部分:构建与测试(每次改动后必须跑)

```bash
# 依赖(通常已装好,首次或换环境时确认)
sudo apt-get update && sudo apt-get install -y build-essential cmake libgmp-dev

# Release 构建
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 单元测试(全部必须绿,顺序不重要)
./build/test_tlv
./build/test_geo
./build/test_paillier
./build/test_heps
./build/test_brake ./build/dns_broker

# 集成测试(Algorithm 1 端到端回归)
bash scripts/run_integration_test.sh

# ASan/UBSan 构建(改动过 broker.cpp / Paillier.cpp / TlvMessage.cpp 之后必做)
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build-asan -j$(nproc)
./build-asan/test_tlv
./build-asan/test_geo
```

任何一步失败就停下来修,不要跳过继续往后走。

---

## 第四部分:系统不变量(不能被打破,除非任务明确要求并在提交信息里说明)

**I1 — Stretch 下界。** `stretch ≥ 1.0` 恒成立。

**I2 — 距离函数对称。** `geoDistance(A,B) == geoDistance(B,A)`。

**I3 — 单调收敛。** 对固定订阅者,`cached_closest_dist` 只能单调下降。

**I4 — 密文不能当索引键(对新代码而言)。** 语义安全的密文不能直接用作
`unordered_map` 的 key(会导致每条消息落进新桶)。当前用
`hashServiceName(明文)` 规避了这个问题,新代码延续这个模式,不要引入新的
"密文当 key"路径。

**I5 — 加密模式必须可观测。** 任何"加密"实验必须能从 broker 的
STATS_DATA_EXT 输出中证明它确实执行了同态匹配(`match_calls > 0`)。

**I6 — 不静默降级。** 密钥缺失、配置非法、前置条件不满足时,受
`require_he` 等显式开关控制;缺省行为必须在日志里有清晰可见的警告,
不能是一行容易被淹没的 `cerr` 或者完全无声。

**I7 — 随机性可复现。** benchmark 中所有随机放置由命令行种子控制,
且种子出现在输出 CSV 的每一行;每个 trial 用不同的派生种子
(`seed + trial_index`),不是所有 trial 共用一个种子。

**I8 — 论文一致性。** `docs/` 与 `README.md` 中对某个机制的描述,
必须与 `src/` 中该机制的实际行为一致。发现不一致时,以代码为准修正文档,
并在提交信息里指出。

**I9 — 每次实验起全新 broker 进程。** 复用同一个 broker 跑多组参数会污染
`sub_groups` 之类的累计计数器(已被 PR 实测确认)。benchmark 脚本必须体现
这一点。

---

## 第五部分:硬性约束

### 不要跑 benchmark 生成论文数据

只允许用来验证"能跑通、不崩溃、输出格式正确"。**绝不能把跑出来的数字写进
`results/` 或任何文档。** 所有进论文的正式数据由作者本人在代码全部冻结之后
统一重跑一次。

### 不要提交实验产物

不要修改或新增 `results/**` 下的 CSV 和图片,除非任务明确要求。

### 每个任务只做一件事

不要"顺手"重构。看到别的问题,写进提交信息或改动说明里的「顺带发现」部分,
不要动手改。改动面越小,作者本地验证越快、风险越低。

### 密码学代码的额外要求

- 任何参数位宽必须有注释,说明它来自哪条数学约束
- `L(x) = (x-1)/n` 只在 `x ≡ 1 (mod n)` 时有意义,加断言
- 涉及私钥的模幂优先用 `mpz_powm_sec`(当前代码库尚未做,已知的顺带发现项)
- 不要把 `gmp_randclass` 放在函数内部反复构造(已修复为类成员,不要退回旧写法)

### 代码风格

- C++17,现有代码混用中英文注释,保持现状即可
- 现有命名:成员变量带尾下划线(`config_`、`my_region_`),部分老成员没有,
  不要统一改
- 不引入新的第三方依赖(GMP 之外)

---

## 第六部分:每次改动收尾格式

改完之后,在终端里用下面这个结构总结给我(不用开 PR,这是本地流程):

```
## 改了什么
(一两句话)

## 为什么
(解决的是哪个具体问题;如果是行为变更,说明改前改后的行为差异)

## 怎么验证的
(粘贴 build + 单元测试 + 集成测试的关键输出)

## 需要重跑的实验
(列出受影响的实验、对应论文章节、重跑命令。若无则写"无")

## 顺带发现
(本次没有修但发现的问题。若无则写"无")
```

然后停下来等待确认,不要自己 `git commit`——由我看过 diff 之后决定是否提交。