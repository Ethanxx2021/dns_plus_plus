#!/bin/bash
# ============================================================
# DNS++ full evaluation driver
#
# 依次执行四组实验,并把 CSV 输出到指定目录:
#   1. 单 broker brake sweep   (single_brake_<L>.csv)
#   2. 多 broker brake sweep   (multi_brake_<L>.csv)
#   3. 订阅者规模扫描          (sweep_<S>.csv)
#   4. plaintext vs encrypted  (plain.csv / encrypted.csv)
#
# 用法:
#   scripts/run_full_eval.sh --trials=<n> --out=<dir>
#
# 复现性 / 可恢复性要求:
#   * 每个实验、每组参数都起全新的 broker 进程(CLAUDE.md 不变量 I9)。
#   * 已存在的输出文件会跳过、绝不覆盖(便于中断后重跑)。
#   * 每次实验独立做 warm-up(见 --warmup 默认值),避免 plaintext/encrypted
#     先后顺序带来的冷启动偏向。
#   * Ctrl+C / SIGTERM 通过 trap 清理前台 benchmark 与残留 broker,不留僵尸。
# ============================================================

set -euo pipefail

TRIALS=30
WARMUP=3
SEED=42
OUT=""

# 实验规模(与 README 既有实验保持一致)
PUBS=10
SUBS=50
PUBS_MULTI=20
BRAKE_LIMITS="1 2 4 1000"
SCALE_SUBS="10 50 200 500 1000"
PORT=8080

BROKER_BIN="./build/dns_broker"
BENCH_BROKER="./build/bench_broker"
BENCH_MULTI="./build/bench_multi_broker"

TMPDIR="/tmp/dnspp_eval_$$"

usage() {
    cat <<EOF
Usage: $0 --out=<dir> [--trials=<n>]

  --trials=<n>   number of measured trials per configuration (default $TRIALS)
  --out=<dir>    directory to write CSVs and env.txt (required)

The script re-runs safely: existing output files are skipped, never overwritten.
EOF
}

# ---- 命令行解析 ----
while [ $# -gt 0 ]; do
    case "$1" in
        --trials=*) TRIALS="${1#--trials=}" ;;
        --out=*)    OUT="${1#--out=}" ;;
        --help|-h)  usage; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; usage; exit 1 ;;
    esac
    shift
done

if [ -z "$OUT" ]; then
    echo "error: --out=<dir> is required" >&2
    usage
    exit 1
fi
if [ "$TRIALS" -lt 1 ] 2>/dev/null; then
    echo "error: --trials must be a positive integer" >&2
    exit 1
fi

mkdir -p "$OUT"
mkdir -p "$TMPDIR"

# ---- 清理:保证 Ctrl+C / SIGTERM / 出错时都不留残留 broker ----
BROKER_PID=""
BENCH_PID=""

cleanup() {
    [ -n "$BENCH_PID" ]  && kill "$BENCH_PID"  2>/dev/null || true
    [ -n "$BROKER_PID" ] && kill "$BROKER_PID" 2>/dev/null || true
    # bench_multi_broker 内部 fork 的 broker(argv[0] 形如 ./dns_broker 或
    # ./build/dns_broker)在中断时可能成为孤儿;这里按命令行清掉所有残留 broker。
    pkill -f '/dns_broker' 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$TMPDIR"
}
# INT/TERM 必须显式 exit(经 EXIT trap 走 cleanup),否则 trap 返回后脚本会继续
# 跑下一组参数,又拉起新的 broker。130=128+SIGINT,143=128+SIGTERM。
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ---- env.txt:记录环境元数据 ----
write_env() {
    local git_commit git_dirty cpu_model gmp_ver
    git_commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
    git_dirty="$(git status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
    cpu_model="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //; s/^[[:space:]]*//' || true)"
    gmp_ver="$(dpkg-query -W -f='${Version}' libgmp10 2>/dev/null || echo unknown)"

    {
        echo "timestamp=$(date -Is)"
        echo "git_commit=$git_commit"
        echo "git_dirty_files=$git_dirty"
        echo "cpu_model=$cpu_model"
        echo "nproc=$(nproc 2>/dev/null || echo unknown)"
        echo "kernel=$(uname -r)"
        echo "gmp_libgmp10=$gmp_ver"
        echo "build_type=see each .log ([bench-env] build=...)"
        echo "trials=$TRIALS"
        echo "warmup=$WARMUP"
        echo "seed=$SEED"
        echo "single_broker_pubs=$PUBS subs=$SUBS"
        echo "multi_broker_pubs=$PUBS_MULTI subs=$SUBS"
        echo "scale_subs=$SCALE_SUBS"
        echo "brake_limits=$BRAKE_LIMITS"
    } > "$OUT/env.txt"
    echo "wrote: $OUT/env.txt"
}

# ---- 启动一个全新的单 broker(brake_limit 由参数指定),并等待就绪 ----
start_single_broker() {
    local limit="$1"
    local conf="$TMPDIR/broker_${limit}.conf"
    local log="$TMPDIR/broker_${limit}.log"
    sed "s/^brake_limit=.*/brake_limit=${limit}/" configs/single.conf > "$conf"

    "$BROKER_BIN" "$conf" > "$log" 2>&1 &
    BROKER_PID=$!

    # 等 broker 真正监听。root broker 会先做 2048-bit keyGen(秒级)再进事件循环,
    # 所以必须以日志里的 "Listening on port" 为准,而不是固定 sleep。
    local ready=false
    for _ in $(seq 1 120); do
        if ! kill -0 "$BROKER_PID" 2>/dev/null; then
            echo "error: broker died during startup (see $log)" >&2
            cat "$log" >&2 || true
            exit 1
        fi
        if grep -q "Listening on port" "$log" 2>/dev/null; then
            ready=true
            break
        fi
        sleep 0.25
    done
    if [ "$ready" != true ]; then
        echo "error: broker did not become ready within 30s (see $log)" >&2
        exit 1
    fi
}

stop_single_broker() {
    if [ -n "$BROKER_PID" ]; then
        kill "$BROKER_PID" 2>/dev/null || true
        wait "$BROKER_PID" 2>/dev/null || true
    fi
    BROKER_PID=""
}

# ---- 跑单 broker benchmark(每组参数起全新 broker)----
run_bench_broker() {
    local out_csv="$1" limit="$2" subs="$3" enc="$4"
    local out_log="${out_csv%.csv}.log"

    if [ -s "$out_csv" ]; then
        echo "skip: $(basename "$out_csv") already exists"
        return 0
    fi

    start_single_broker "$limit"

    "$BENCH_BROKER" 127.0.0.1 "$PORT" "$PUBS" "$subs" "$limit" "$TRIALS" "$SEED" "$enc" \
        --warmup="$WARMUP" > "$out_csv" 2> "$out_log" &
    BENCH_PID=$!

    local rc=0
    set +e
    wait "$BENCH_PID"
    rc=$?
    set -e
    BENCH_PID=""

    stop_single_broker

    if [ "$rc" -ne 0 ]; then
        echo "error: bench_broker failed (rc=$rc) -> $(basename "$out_csv"); see $(basename "$out_log")" >&2
        exit 1
    fi
    echo "done: $(basename "$out_csv")"
}

# ---- 跑多 broker benchmark(它自己按 trial fork 全新 broker)----
run_bench_multi() {
    local out_csv="$1" limit="$2"
    local out_log="${out_csv%.csv}.log"

    if [ -s "$out_csv" ]; then
        echo "skip: $(basename "$out_csv") already exists"
        return 0
    fi

    "$BENCH_MULTI" "$PUBS_MULTI" "$SUBS" "$limit" "$TRIALS" "$SEED" \
        --warmup="$WARMUP" > "$out_csv" 2> "$out_log" &
    BENCH_PID=$!

    local rc=0
    set +e
    wait "$BENCH_PID"
    rc=$?
    set -e
    BENCH_PID=""

    if [ "$rc" -ne 0 ]; then
        echo "error: bench_multi_broker failed (rc=$rc) -> $(basename "$out_csv"); see $(basename "$out_log")" >&2
        exit 1
    fi
    echo "done: $(basename "$out_csv")"
}

# ---- 检查二进制是否已构建 ----
for b in "$BROKER_BIN" "$BENCH_BROKER" "$BENCH_MULTI"; do
    if [ ! -x "$b" ]; then
        echo "error: missing executable $b -- run 'cmake --build build -j' first" >&2
        exit 1
    fi
done

write_env

echo ""
echo "=== [1/4] single-broker brake sweep ==="
for L in $BRAKE_LIMITS; do
    run_bench_broker "$OUT/single_brake_${L}.csv" "$L" "$SUBS" 0
done

echo ""
echo "=== [2/4] multi-broker brake sweep ==="
for L in $BRAKE_LIMITS; do
    run_bench_multi "$OUT/multi_brake_${L}.csv" "$L"
done

echo ""
echo "=== [3/4] subscriber scale sweep ==="
for S in $SCALE_SUBS; do
    run_bench_broker "$OUT/sweep_${S}.csv" 4 "$S" 0
done

echo ""
echo "=== [4/4] plaintext vs encrypted ==="
# 两种模式各用独立的 broker + 独立 warm-up,避免"先跑哪个"偏向另一侧。
run_bench_broker "$OUT/plain.csv" 4 "$SUBS" 0
run_bench_broker "$OUT/encrypted.csv" 4 "$SUBS" 1

echo ""
echo "All requested outputs present under $OUT/"
