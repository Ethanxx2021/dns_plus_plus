#!/usr/bin/env python3
"""
aggregate_results.py — 汇总 results/final/ 下的正式实验数据。

统计工具在 bench_stats.py(共享),本脚本只做各实验的汇总编排。
产出 results/final/{summary.txt, numbers.tex}。

用法: venv/bin/python scripts/aggregate_results.py [results_dir]
"""
import os
import sys

import numpy as np

from bench_stats import (load_rows, trial_level, mean_ci, pctl,
                         wilcoxon_signed_rank, m_recall, m_stretch, m_latency,
                         last_broker_stats, traffic_ratios, udp_deltas)


def f(x, nd=4):
    return f"{x:.{nd}f}"


def ci_str(m, lo, hi, nd=4):
    return f"{f(m, nd)} [{f(lo, nd)}, {f(hi, nd)}]"


class Summary:
    def __init__(self):
        self.lines = []
        self.tex = []

    def line(self, s=''):
        self.lines.append(s)

    def texcmd(self, name, value):
        self.tex.append(r"\newcommand{\%s}{%s}" % (name, value))


def report_metric(S, label, values, nd=4, pcts=True):
    if not values:
        S.line(f"{label}: n=0 (no data)")
        return None, None, None
    m, lo, hi = mean_ci(values)
    S.line(f"{label}: n={len(values)} mean={ci_str(m, lo, hi, nd)}")
    if pcts and len(values) >= 2:
        S.line(f"    median={f(np.median(values), nd)} "
               f"p95={f(pctl(values, 95), nd)} p99={f(pctl(values, 99), nd)}")
    return m, lo, hi


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
            m = __import__('re').match(r'(\w+): ([\d.eE+-]+) ms/op', line)
            if m:
                vals.setdefault(m.group(1), []).append(float(m.group(2)))
    for k in ['blindNotification', 'blindSubscription', 'executeMatch']:
        if k in vals:
            med = np.median(vals[k])
            S.line(f"  {k}: {med:.4f} ms/op (runs: {[f'{v:.3f}' for v in vals[k]]})")
            S.texcmd("crypto" + k + "Ms", f(med, 3))
    p = os.path.join(d, "keygen_nbits.txt")
    if os.path.exists(p):
        txt = open(p).read()
        m = __import__('re').search(r'summary: (\d+)/(\d+) at exactly 2048 bits', txt)
        if m:
            S.line(f"  keygen n_bits: {m.group(1)}/{m.group(2)} at exactly 2048 bits")
            S.texcmd("keygen2048Count", m.group(1))


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else 'results/final'
    S = Summary()
    S.line(f"DNS++ 正式实验结果汇总 (results dir: {d})")
    S.line("bootstrap: 10000 resamples, 95% percentile CI; stable per-value seed")
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
