#!/usr/bin/env python3
"""
aggregate_results.py — 汇总 results/final/ 下的正式实验数据。

产出两份文件(写到 results/final/):
  summary.txt   人读的统计汇总(mean + bootstrap 95% CI,配对检验 p 值等)
  numbers.tex   LaTeX 宏定义,论文直接 \\input

统计方法(对应评分标准 "statistical significance"):
  1. 95% 置信区间用 bootstrap(percentile,10000 次),不是 mean ± std。
  2. 有界指标(recall∈[0,1]、stretch≥1)自然用 bootstrap percentile CI,不会越界。
  3. plaintext vs encrypted 用 Wilcoxon signed-rank 配对检验(按 trial 配对)。
  4. p95/p99 用 trial 级别的分布(先 per-trial 聚合,再对 trial 求分位数),
     不把同一 trial 内强相关的 subscriber 当独立样本 pooled。
  5. 每组都报 n(trial 数)。

用法: venv/bin/python scripts/aggregate_results.py [results_dir]
"""
import csv
import math
import os
import re
import sys

import numpy as np

RNG = np.random.default_rng(20260824)   # bootstrap 固定种子,可复现
N_BOOT = 10000
ALPHA = 0.05


# --------------------------------------------------------------------------
# 基础统计工具
# --------------------------------------------------------------------------
def load_rows(path):
    with open(path, newline='') as f:
        return list(csv.DictReader(f))


def trial_level(rows, metric):
    """把逐 subscriber 行聚合成 trial 级(每个 trial 一个均值)。"""
    by = {}
    for r in rows:
        v = metric(r)
        if v is None:
            continue
        by.setdefault(int(r['trial']), []).append(v)
    return [sum(v) / len(v) for v in by.values() if v]


def mean_ci(values):
    v = np.asarray(values, dtype=float)
    m = float(v.mean())
    boot = RNG.choice(v, size=(N_BOOT, len(v)), replace=True).mean(axis=1)
    lo, hi = float(np.percentile(boot, 100 * ALPHA / 2)), float(np.percentile(boot, 100 * (1 - ALPHA / 2)))
    return m, lo, hi


def pctl(values, p):
    v = np.asarray(values, dtype=float)
    return float(np.percentile(v, p))


def normal_cdf(x):
    return 0.5 * math.erfc(-x / math.sqrt(2.0))


def wilcoxon_signed_rank(x, y):
    """配对 Wilcoxon signed-rank(手写,不依赖 scipy)。返回 (W, p, n, z)。"""
    x = np.asarray(x, dtype=float)
    y = np.asarray(y, dtype=float)
    d = x - y
    d = d[np.isfinite(d) & (d != 0)]
    n = len(d)
    if n == 0:
        return 0.0, 1.0, 0, 0.0
    absd = np.abs(d)
    order = np.argsort(absd)
    ranks = np.empty(n, dtype=float)
    i = 0
    while i < n:
        j = i
        while j < n and absd[order[j]] == absd[order[i]]:
            j += 1
        ranks[order[i:j]] = (i + j + 1) / 2.0   # 平均秩(1-based)
        i = j
    wp = ranks[d > 0].sum()
    wm = ranks[d < 0].sum()
    w = min(wp, wm)
    mu = n * (n + 1) / 4.0
    sigma = math.sqrt(n * (n + 1) * (2 * n + 1) / 24.0)
    z = (w - mu) / sigma if sigma > 0 else 0.0
    p = 2.0 * (1.0 - normal_cdf(abs(z)))
    return w, p, n, z


def f(x, nd=4):
    return f"{x:.{nd}f}"


def ci_str(m, lo, hi, nd=4):
    return f"{f(m, nd)} [{f(lo, nd)}, {f(hi, nd)}]"


# --------------------------------------------------------------------------
# 指标提取器(CSV 列)
# --------------------------------------------------------------------------
def m_recall(r):
    return float(r['recall'])


def m_stretch(r):
    s = float(r['stretch'])
    return s if s > 0 else None          # stretch<0 表示未收到,跳过


def m_latency(r):
    v = float(r['latency_per_pub_ms'])
    return v if v >= 0 else None


# --------------------------------------------------------------------------
# 日志解析
# --------------------------------------------------------------------------
def parse_kv_line(line):
    """把 'a=1 b=2.3 c=-1' 解析成 {a:1.0, b:2.3, c:-1.0}。"""
    out = {}
    for tok in line.split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v
    return out


def last_broker_stats(log_path):
    """取 .log 里最后一行 broker stats(累计量)。"""
    last = None
    with open(log_path, errors='replace') as f:
        for line in f:
            if 'broker stats' in line and 'cumulative' in line:
                last = line
    if last is None:
        return {}
    return parse_kv_line(last)


def traffic_ratios(log_path):
    """多 broker 每个 trial 的 Traffic Ratio。"""
    out = []
    with open(log_path, errors='replace') as f:
        for line in f:
            if line.startswith('Trial') and 'Traffic Ratio=' in line:
                m = re.search(r'Traffic Ratio=([0-9.]+)', line)
                if m:
                    out.append(float(m.group(1)))
    return out


def udp_deltas(log_path):
    """取 [bench-net] udp_rcvbuf_errors_delta / udp_in_errors_delta。"""
    rc = inc = None
    with open(log_path, errors='replace') as f:
        for line in f:
            m = re.search(r'udp_rcvbuf_errors_delta=(\d+) udp_in_errors_delta=(\d+)', line)
            if m:
                rc, inc = int(m.group(1)), int(m.group(2))
    return rc, inc


# --------------------------------------------------------------------------
# 汇总结果容器
# --------------------------------------------------------------------------
class Summary:
    def __init__(self):
        self.lines = []
        self.tex = []

    def line(self, s=''):
        self.lines.append(s)

    def texcmd(self, name, value):
        self.tex.append(r"\newcommand{\%s}{%s}" % (name, value))


def report_metric(S, label, values, nd=4, pcts=True):
    """对 trial 级 values 报 mean + bootstrap 95% CI(以及可选 p95/p99)。返回 (m,lo,hi)。"""
    if not values:
        S.line(f"{label}: n=0 (no data)")
        return None, None, None
    m, lo, hi = mean_ci(values)
    S.line(f"{label}: n={len(values)} mean={ci_str(m, lo, hi, nd)}")
    if pcts and len(values) >= 2:
        S.line(f"    median={f(np.median(values), nd)} "
               f"p95={f(pctl(values, 95), nd)} p99={f(pctl(values, 99), nd)}")
    return m, lo, hi


# --------------------------------------------------------------------------
# 各实验统计
# --------------------------------------------------------------------------
def single_brake(S, d, tex_prefix):
    S.line("=" * 70)
    S.line("单 broker brake sweep (10 pub / 50 sub)")
    for L in [1, 2, 4, 1000]:
        p = os.path.join(d, f"single_brake_{L}.csv")
        if not os.path.exists(p):
            S.line(f"  brake={L}: MISSING"); continue
        rows = load_rows(p)
        rec = trial_level(rows, m_recall)
        st = trial_level(rows, m_stretch)
        lat = trial_level(rows, m_latency)
        S.line(f"  brake_limit={L}:")
        rm, rlo, rhi = report_metric(S, "    recall", rec)
        sm, slo, shi = report_metric(S, "    stretch", st)
        report_metric(S, "    latency_per_pub(ms)", lat)
        # 统计计数器:suppression
        st_log = last_broker_stats(os.path.join(d, f"single_brake_{L}.log"))
        pr = st_log.get('pub_received', 0)
        bl = st_log.get('braked_local', 0)
        bu = st_log.get('braked_up', 0)
        S.line(f"    STATS: pub_received={pr} braked_local={bl} braked_up={bu} "
               f"suppression={(bl + bu) / pr if pr else 0:.4f}")
        if rm is not None:
            S.texcmd(f"{tex_prefix}{L}Recall", f(rm, 3))
            S.texcmd(f"{tex_prefix}{L}RecallLo", f(rlo, 3))
            S.texcmd(f"{tex_prefix}{L}RecallHi", f(rhi, 3))
        if sm is not None:
            S.texcmd(f"{tex_prefix}{L}Stretch", f(sm, 3))
            S.texcmd(f"{tex_prefix}{L}StretchLo", f(slo, 3))
            S.texcmd(f"{tex_prefix}{L}StretchHi", f(shi, 3))


def multi_brake(S, d, tex_prefix):
    S.line("=" * 70)
    S.line("多 broker brake sweep (20 pub / 50 sub / 3 broker)")
    for L in [1, 2, 4, 1000]:
        p = os.path.join(d, f"multi_brake_{L}.csv")
        if not os.path.exists(p):
            S.line(f"  brake={L}: MISSING"); continue
        rows = load_rows(p)
        rec = trial_level(rows, m_recall)
        st = trial_level(rows, m_stretch)
        tr = traffic_ratios(os.path.join(d, f"multi_brake_{L}.log"))
        S.line(f"  brake_limit={L}:")
        rm, rlo, rhi = report_metric(S, "    recall", rec)
        sm, slo, shi = report_metric(S, "    stretch", st)
        tmr, tlo, thi = report_metric(S, "    traffic_ratio", tr)
        st_log = last_broker_stats(os.path.join(d, f"multi_brake_{L}.log"))
        pr = st_log.get('pub_received', 0)
        bl = st_log.get('braked_local', 0)
        bu = st_log.get('braked_up', 0)
        S.line(f"    STATS: pub_received={pr} braked_local={bl} braked_up={bu} "
               f"suppression={(bl + bu) / pr if pr else 0:.4f}")
        if rm is not None:
            S.texcmd(f"{tex_prefix}{L}Recall", f(rm, 3))
            S.texcmd(f"{tex_prefix}{L}RecallLo", f(rlo, 3))
            S.texcmd(f"{tex_prefix}{L}RecallHi", f(rhi, 3))
        if tmr is not None:
            S.texcmd(f"{tex_prefix}{L}Traffic", f(tmr, 3))
            S.texcmd(f"{tex_prefix}{L}TrafficLo", f(tlo, 3))
            S.texcmd(f"{tex_prefix}{L}TrafficHi", f(thi, 3))


def scale_sweep(S, d, tex_prefix):
    S.line("=" * 70)
    S.line("订阅者规模扫描 (brake_limit=4)")
    for N in [10, 50, 200, 500, 1000]:
        p = os.path.join(d, f"sweep_{N}.csv")
        if not os.path.exists(p):
            S.line(f"  subs={N}: MISSING"); continue
        rows = load_rows(p)
        rec = trial_level(rows, m_recall)
        st = trial_level(rows, m_stretch)
        rc, inc = udp_deltas(os.path.join(d, f"sweep_{N}.log"))
        S.line(f"  subs={N}:")
        rm, rlo, rhi = report_metric(S, "    recall", rec)
        report_metric(S, "    stretch", st)
        S.line(f"    UDP: rcvbuf_errors_delta={rc} in_errors_delta={inc}")
        if rm is not None:
            S.texcmd(f"{tex_prefix}{N}Recall", f(rm, 3))
            S.texcmd(f"{tex_prefix}{N}RecallLo", f(rlo, 3))
            S.texcmd(f"{tex_prefix}{N}RecallHi", f(rhi, 3))


def plain_encrypted(S, d):
    S.line("=" * 70)
    S.line("明文 vs 加密 (10 pub / 50 sub / brake=4)")
    pp, ep = os.path.join(d, "plain.csv"), os.path.join(d, "encrypted.csv")
    if not (os.path.exists(pp) and os.path.exists(ep)):
        S.line("  MISSING plain/encrypted"); return
    pr = load_rows(pp)
    er = load_rows(ep)

    S.line("  明文:")
    prec = trial_level(pr, m_recall)
    pst = trial_level(pr, m_stretch)
    plat = trial_level(pr, m_latency)
    report_metric(S, "    recall", prec)
    report_metric(S, "    stretch", pst)
    report_metric(S, "    latency_per_pub(ms)", plat)

    S.line("  加密:")
    erec = trial_level(er, m_recall)
    est = trial_level(er, m_stretch)
    elat = trial_level(er, m_latency)
    report_metric(S, "    recall", erec)
    report_metric(S, "    stretch", est)
    report_metric(S, "    latency_per_pub(ms)", elat)

    # 配对检验(按 trial 配对)
    S.line("  配对 Wilcoxon signed-rank(加密 - 明文 latency_per_pub):")
    if len(plat) == len(elat) and len(plat) > 0:
        w, p, n, z = wilcoxon_signed_rank(elat, plat)
        med_diff = float(np.median(np.asarray(elat) - np.asarray(plat)))
        S.line(f"    n_pairs={n} W={w:.1f} z={z:.3f} p={p:.4g} "
               f"median_diff(enc-plain)={med_diff:.3f} ms")
        S.texcmd("wilcoxonLatencyP", f"{p:.4g}")
        S.texcmd("latencyMedianDiff", f(med_diff, 2))
        S.texcmd("latencyPlain", f(np.median(plat), 2))
        S.texcmd("latencyEncrypted", f(np.median(elat), 2))
        # 加密观测性
        st = last_broker_stats(os.path.join(d, "encrypted.log"))
        S.line(f"    STATS(encrypted): match_calls={st.get('match_calls', 0)} "
               f"he_mode={st.get('he_mode', 0)} sub_groups={st.get('sub_groups', 0)}")
        stp = last_broker_stats(os.path.join(d, "plain.log"))
        S.line(f"    STATS(plain):     match_calls={stp.get('match_calls', 0)} "
               f"he_mode={stp.get('he_mode', 0)} sub_groups={stp.get('sub_groups', 0)}")


def dynamics(S, d):
    S.line("=" * 70)
    S.line("dynamics: 副本迁移收敛 (20 pub / 50 sub / 20 migrations)")
    p = os.path.join(d, "dynamics_plain.csv")
    if not os.path.exists(p):
        S.line("  MISSING dynamics_plain.csv"); return
    rows = load_rows(p)
    closer = [r for r in rows if r['migration_class'] == 'closer']
    farther = [r for r in rows if r['migration_class'] == 'farther']

    S.line(f"  closer:  records={len(closer)}")
    if closer:
        conv = sum(1 for r in closer if r['converged'] == '1')
        rate = conv / len(closer)
        # trial 级收敛时间
        by = {}
        for r in closer:
            if r['converged'] == '1':
                by.setdefault(int(r['trial']), []).append(float(r['convergence_ms']))
        tlv = [sum(v) / len(v) for v in by.values() if v]
        S.line(f"    convergence rate={rate:.4f} ({conv}/{len(closer)})")
        report_metric(S, "    convergence_ms (trial-level mean)", tlv)
        S.texcmd("dynCloserRate", f(rate, 3))
        if tlv:
            S.texcmd("dynCloserMedianMs", f(np.median(tlv), 2))

    S.line(f"  farther: records={len(farther)}")
    if farther:
        conv = sum(1 for r in farther if r['converged'] == '1')
        S.line(f"    convergence rate={conv / len(farther):.4f} ({conv}/{len(farther)})")
        S.texcmd("dynFartherRate", f(conv / len(farther), 3))
    rc, inc = udp_deltas(os.path.join(d, "dynamics_plain.log"))
    S.line(f"  UDP: rcvbuf_errors_delta={rc} in_errors_delta={inc}")
    b = last_broker_stats(os.path.join(d, "dynamics_plain.log"))
    if b:
        S.line(f"  STATS: braked={b.get('braked', 0)} (must be 0 for valid dynamics data)")


def crypto(S, d):
    S.line("=" * 70)
    S.line("密码学 microbenchmark (3 runs, 取中位数)")
    vals = {}
    for i in [1, 2, 3]:
        p = os.path.join(d, f"microbench_run{i}.txt")
        if not os.path.exists(p):
            continue
        for line in open(p):
            m = re.match(r'(\w+): ([\d.eE+-]+) ms/op', line)
            if m:
                vals.setdefault(m.group(1), []).append(float(m.group(2)))
    for k in ['blindNotification', 'blindSubscription', 'executeMatch']:
        if k in vals:
            med = np.median(vals[k])
            S.line(f"  {k}: {med:.4f} ms/op (runs: {[f'{v:.3f}' for v in vals[k]]})")
            S.texcmd("crypto" + k + "Ms", f(med, 3))
    # keygen n_bits
    p = os.path.join(d, "keygen_nbits.txt")
    if os.path.exists(p):
        txt = open(p).read()
        m = re.search(r'summary: (\d+)/(\d+) at exactly 2048 bits', txt)
        if m:
            S.line(f"  keygen n_bits: {m.group(1)}/{m.group(2)} at exactly 2048 bits")
            S.texcmd("keygen2048Count", m.group(1))


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else 'results/final'
    S = Summary()
    S.line(f"DNS++ 正式实验结果汇总 (results dir: {d})")
    S.line(f"bootstrap: {N_BOOT} resamples, 95% percentile CI; RNG seed fixed")
    S.line("")

    single_brake(S, d, 'singleBrake')
    multi_brake(S, d, 'multiBrake')
    scale_sweep(S, d, 'sweep')
    plain_encrypted(S, d)
    dynamics(S, d)
    crypto(S, d)

    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'summary.txt'), 'w') as f:
        f.write('\n'.join(S.lines) + '\n')
    with open(os.path.join(d, 'numbers.tex'), 'w') as f:
        f.write('% Auto-generated by scripts/aggregate_results.py — do not hand-edit\n')
        f.write('\n'.join(S.tex) + '\n')
    print(f"wrote {d}/summary.txt and {d}/numbers.tex")
    print(f"{len(S.lines)} summary lines, {len(S.tex)} tex macros")


if __name__ == '__main__':
    main()
