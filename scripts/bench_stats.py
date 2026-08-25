#!/usr/bin/env python3
"""
bench_stats.py — 共享统计工具。

被 aggregate_results.py 与 generate_tables.py 共同引用,保证两者算出的
均值/置信区间/检验值完全一致(同一数据源、同一 bootstrap)。

bootstrap 用「由输入值决定的稳定种子」逐调用播种,结果与调用顺序无关,
所以两个脚本重算也不会漂移。
"""
import csv
import hashlib
import math
import re

import numpy as np

N_BOOT = 10000
ALPHA = 0.05


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


def _stable_seed(values):
    v = np.asarray(values, dtype=float)
    return int.from_bytes(hashlib.md5(v.tobytes()).digest()[:8], 'little') % (2 ** 32)


def mean_ci(values):
    v = np.asarray(values, dtype=float)
    m = float(v.mean())
    rng = np.random.default_rng(_stable_seed(v))
    boot = rng.choice(v, size=(N_BOOT, len(v)), replace=True).mean(axis=1)
    lo = float(np.percentile(boot, 100 * ALPHA / 2))
    hi = float(np.percentile(boot, 100 * (1 - ALPHA / 2)))
    return m, lo, hi


def pctl(values, p):
    return float(np.percentile(np.asarray(values, dtype=float), p))


def normal_cdf(x):
    return 0.5 * math.erfc(-x / math.sqrt(2.0))


def wilcoxon_signed_rank(x, y):
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
        ranks[order[i:j]] = (i + j + 1) / 2.0
        i = j
    wp = ranks[d > 0].sum()
    wm = ranks[d < 0].sum()
    w = min(wp, wm)
    mu = n * (n + 1) / 4.0
    sigma = math.sqrt(n * (n + 1) * (2 * n + 1) / 24.0)
    z = (w - mu) / sigma if sigma > 0 else 0.0
    p = 2.0 * (1.0 - normal_cdf(abs(z)))
    return w, p, n, z


# ---- 指标提取器(CSV 列) ----
def m_recall(r):
    return float(r['recall'])


def m_stretch(r):
    s = float(r['stretch'])
    return s if s > 0 else None


def m_latency(r):
    v = float(r['latency_per_pub_ms'])
    return v if v >= 0 else None


# ---- 日志解析 ----
def parse_kv_line(line):
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
    last = None
    with open(log_path, errors='replace') as f:
        for line in f:
            if 'broker stats' in line and 'cumulative' in line:
                last = line
    return parse_kv_line(last) if last else {}


def traffic_ratios(log_path):
    out = []
    with open(log_path, errors='replace') as f:
        for line in f:
            if line.startswith('Trial') and 'Traffic Ratio=' in line:
                m = re.search(r'Traffic Ratio=([0-9.]+)', line)
                if m:
                    out.append(float(m.group(1)))
    return out


def udp_deltas(log_path):
    rc = inc = None
    with open(log_path, errors='replace') as f:
        for line in f:
            m = re.search(r'udp_rcvbuf_errors_delta=(\d+) udp_in_errors_delta=(\d+)', line)
            if m:
                rc, inc = int(m.group(1)), int(m.group(2))
    return rc, inc
