# CLAUDE.md — DNS++ 项目上下文

> 放在仓库根目录。Claude Code 每次任务都会自动读取本文件。

## 项目性质

这是 UCL MSc Internet Engineering 的毕业设计(ELEC0054),**不是普通的软件项目**。
仓库产出会被写进一篇学位论文并接受学术评审。这决定了两条最高优先级原则:

1. **代码必须与论文中的描述一致。** 任何改变系统行为的修改,都必须同步更新 `docs/` 下的
   对应说明,并在 PR 描述里明确写出「这会导致论文哪一节/哪张表的数据作废,需要重跑」。
2. **可复现性高于一切。** 所有随机性必须可通过种子控制;所有实验参数必须出现在输出的
   CSV 里;任何"魔数"必须有注释说明其数学来源。

## 项目是什么

DNS++ 是对互联网名字解析的重新设计,用一个**分层 publish/subscribe overlay** 替代传统 DNS,
同时解决三个问题:隐私(同态加密匹配,broker 学不到查询的名字)、动态性(推送式更新)、
位置感知(把订阅者导向最近的服务副本)。

原始设计(Rio et al., SIGCOMM'26)只有 Java 仿真。本仓库是**第一个真实系统实现**(C++17),
研究贡献在于「在真实硬件上测量该设计的可行性与加密层的开销」。

### 三个角色

- **Publisher**:服务副本,发 `PUBLISH` 宣告自己的名字 + 经纬度
- **Subscriber**:客户端,发 `SUBSCRIBE` 请求某个名字,期望收到**最近的**副本
- **Broker**:overlay 节点,组织成树,负责匹配与路由

### 核心算法

- **Algorithm 1 (Proximity Routing)**:每个订阅者维护 `cached_closest_dist`,
  broker 只在新 publication 比该订阅者已知的最近副本更近时才投递。
- **Propagation brake**:按象限的滑动窗口限流器,限制 publication 的传播,
  用 recall 换 traffic。
- **Quadrant cache**:父 broker 对每个子 broker 的每个象限维护最近距离,过滤向下传播。
- **MBH (Minimum Bounding Hyperrectangle)**:每个 broker 的空间覆盖范围,自底向上聚合。
- **Modified Paillier**:`Match(bval_n, bval_m)` 让 broker 在不解密的前提下判断
  publication 的名字是否等于 subscription 的名字。

## 代码结构

```
src/
  broker/broker.{h,cpp}    # 核心:epoll 事件循环 + 所有消息处理器
  broker/main.cpp          # 配置解析与启动
  protocol/                # TLV 编解码
  common/geo.h             # Region / geoDistance / quadrant
  crypto/Paillier.{h,cpp}  # 修改版 Paillier
  crypto/Heps.{h,cpp}      # 密钥服务封装
benchmarks/
  bench_broker.cpp         # 单 broker 实验
  bench_multi_broker.cpp   # 多 broker 实验(fork 三个 broker)
tests/                     # test_tlv / test_geo / test_paillier / test_heps / test_brake
scripts/                   # 集成测试与绘图
config/                    # root.conf / leaf1.conf / leaf2.conf
results/                   # 实验数据 CSV 与图表
docs/learning_log.md       # 开发日志
```

## 构建与测试

```bash
# 依赖(容器里可能需要先装)
sudo apt-get update && sudo apt-get install -y build-essential cmake libgmp-dev python3-pip

# 构建
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 单元测试(每一次改动后都必须全绿)
./build/test_tlv && ./build/test_geo && ./build/test_paillier && ./build/test_heps \
  && ./build/test_brake ./build/dns_broker   # test_brake 会 fork 一个真实 broker，需先构建 dns_broker

# 单 broker 集成测试
bash scripts/run_integration_test.sh

# 带 sanitizer 的构建(改动 broker.cpp 后建议跑一次)
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build-asan -j && ./build-asan/test_tlv
```

## 硬性约束(违反即视为任务失败)

### 不要跑 benchmark 生成论文数据

沙箱环境的 CPU 调度、UDP 缓冲区行为、进程隔离都与作者本地机器不同。
**benchmark 只允许用来验证"能跑通、不崩溃、输出格式正确",绝不能把跑出来的数字
写进 `results/` 或任何文档。**所有进论文的数据由作者在本地重跑。

如果一个改动会让已有数据失效,在 PR 描述里写一节 `## 需要重跑的实验`,列出:
受影响的实验、对应论文章节、重跑命令。

### 不要提交实验产物

不要修改或新增 `results/**` 下的 CSV 和图片,除非任务明确要求。

### 不要"顺手"重构

每个 PR 只解决 prompt 里指定的那一个问题。看到别的问题,写进 PR 描述的
`## 顺带发现` 一节,不要动手改。改动面越小,作者本地验证越快。

### 不要静默降级

这个代码库有一个已知的坏模式:出错时打印一条 warning 然后退回到某种"默认行为"
(例如密钥读取失败就退回明文模式)。**这在学术实验里是灾难性的**——它会让
一次失败的加密实验看起来像一次成功的实验。

新写的代码遇到配置错误或前置条件不满足时,一律 fail-fast:打印明确错误并
`exit(EXIT_FAILURE)`,不要继续以降级模式运行。

### 密码学代码的额外要求

- 任何参数位宽(如 `r_m` 的 992 位)必须有注释,说明它来自哪条数学约束
- `L(x) = (x-1)/n` 只在 `x ≡ 1 (mod n)` 时有意义,否则整数除法会静默给出错误结果 → 加断言
- 涉及私钥的模幂优先用 `mpz_powm_sec`
- 不要把 `gmp_randclass` 放在函数内部构造(每次调用重新播种,既慢又是随机性隐患)

## 代码风格

- C++17,现有代码混用中英文注释,保持现状即可,新注释用中文或英文都行
- 现有命名:成员变量带尾下划线(`config_`、`my_region_`),部分老成员没有,不要统一改
- 不引入新的第三方依赖(GMP 之外)。配置解析故意用手写 `key=value` 而非 YAML

## 术语速查

| 术语 | 含义 |
|---|---|
| MBH | broker 的空间覆盖矩形 |
| Brake | 按象限限制 publication 传播的滑动窗口 |
| Quadrant cache | 父 broker 对子 broker 每象限的最近距离缓存,过滤向下传播 |
| IT[] / OT[] | 输入/输出订阅转发表 |
| HEPS | 同态加密参数服务,生成并分发密钥(应当独立于 broker) |
| `bval_n` | publication 侧的盲化值 |
| `bval_m1` / `bval_m2` | subscription 侧的两个盲化值(分别对应 `-v` 和 `-(v+1)`) |
| Recall | 收到了真正最近副本的订阅者比例 |
| Stretch | 实际收到副本的距离 / 最优距离,下界为 1 |
| Traffic ratio | (上行 + 下行 + 本地投递) / 成功本地投递数 |
