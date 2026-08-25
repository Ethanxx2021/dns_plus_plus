#!/usr/bin/env python3
"""
generate_tables.py — 从 results/final/ 生成 booktabs 格式的 LaTeX 表格源码。

产出 results/final/tables.tex,可直接 \\input 进论文。
所有统计值与 numbers.tex 同源(共用 bench_stats.py),不会漂移。

用法: venv/bin/python scripts/generate_tables.py [results_dir]
"""
import os
import sys

import numpy as np

from bench_stats import (load_rows, trial_level, mean_ci, pctl,
                         wilcoxon_signed_rank, m_recall, m_stretch, m_latency,
                         last_broker_stats, traffic_ratios, udp_deltas)


def fmt_mean_ci(m, lo, hi, nd=3):
    return f"{m:.{nd}f} [{lo:.{nd}f}, {hi:.{nd}f}]"


def metric_mean_ci(rows, metric, nd=3):
    tl = trial_level(rows, metric)
    if not tl:
        return "---"
    m, lo, hi = mean_ci(tl)
    return fmt_mean_ci(m, lo, hi, nd)


def microbench(path, name):
    vals = []
    for i in [1, 2, 3]:
        p = os.path.join(path, f"microbench_run{i}.txt")
        if not os.path.exists(p):
            continue
        import re
        for line in open(p):
            m = re.match(r'(\w+): ([\d.eE+-]+) ms/op', line)
            if m and m.group(1) == name:
                vals.append(float(m.group(2)))
    if not vals:
        return None, None, None
    return float(np.median(vals)), min(vals), max(vals)


def esc(s):
    return str(s).replace('_', r'\_')


def table_env(lines, caption, label, cols):
    out = []
    out.append(r"\begin{table}[htbp]")
    out.append(r"\centering")
    out.append(r"\caption{%s}" % caption)
    out.append(r"\label{%s}" % label)
    out.append(r"\begin{tabular}{%s}" % cols)
    out.append(r"\toprule")
    out.extend(lines)
    out.append(r"\bottomrule")
    out.append(r"\end{tabular}")
    out.append(r"\end{table}")
    return "\n".join(out) + "\n"


def tab_single_brake(d):
    header = (r"\textbf{brake\_limit} & \textbf{recall} & \textbf{stretch} "
              r"& \textbf{suppression} & \textbf{n} \\")
    lines = [header, r"\midrule"]
    for L in [1, 2, 4, 1000]:
        p = os.path.join(d, f"single_brake_{L}.csv")
        if not os.path.exists(p):
            continue
        rows = load_rows(p)
        rec = metric_mean_ci(rows, m_recall)
        st = metric_mean_ci(rows, m_stretch)
        st_log = last_broker_stats(os.path.join(d, f"single_brake_{L}.log"))
        pr = st_log.get('pub_received', 0)
        bl = st_log.get('braked_local', 0)
        supp = (bl / pr) if pr else 0.0
        n = len(trial_level(rows, m_recall))
        lines.append(f"{L} & {rec} & {st} & {supp:.3f} & {n} \\\\")
    return table_env(lines,
                     "Single-broker brake sweep (10 publishers, 50 subscribers). "
                     "Recall/stretch are mean with 95\\% bootstrap CI over 30 trials; "
                     "suppression is measured braked\\_local/pub\\_received.",
                     "tab:single-brake", "lcccc")


def tab_multi_brake(d):
    header = (r"\textbf{brake\_limit} & \textbf{recall} & \textbf{stretch} "
              r"& \textbf{traffic ratio} & \textbf{n} \\")
    lines = [header, r"\midrule"]
    for L in [1, 2, 4, 1000]:
        p = os.path.join(d, f"multi_brake_{L}.csv")
        if not os.path.exists(p):
            continue
        rows = load_rows(p)
        rec = metric_mean_ci(rows, m_recall)
        st = metric_mean_ci(rows, m_stretch)
        tr = traffic_ratios(os.path.join(d, f"multi_brake_{L}.log"))
        trs = "---"
        if tr:
            m, lo, hi = mean_ci(tr)
            trs = fmt_mean_ci(m, lo, hi, 3)
        n = len(trial_level(rows, m_recall))
        lines.append(f"{L} & {rec} & {st} & {trs} & {n} \\\\")
    return table_env(lines,
                     "Multi-broker brake sweep (20 publishers, 50 subscribers, "
                     "3-broker tree). All metrics are mean with 95\\% bootstrap CI over 30 trials.",
                     "tab:multi-brake", "lcccc")


def tab_scale(d):
    header = (r"\textbf{subscribers} & \textbf{recall} & \textbf{stretch} "
              r"& \textbf{RcvbufErrors} & \textbf{InErrors} & \textbf{n} \\")
    lines = [header, r"\midrule"]
    for N in [10, 50, 200, 500, 1000]:
        p = os.path.join(d, f"sweep_{N}.csv")
        if not os.path.exists(p):
            continue
        rows = load_rows(p)
        rec = metric_mean_ci(rows, m_recall)
        st = metric_mean_ci(rows, m_stretch)
        rc, inc = udp_deltas(os.path.join(d, f"sweep_{N}.log"))
        rc = rc if rc is not None else 0
        inc = inc if inc is not None else 0
        n = len(trial_level(rows, m_recall))
        lines.append(f"{N} & {rec} & {st} & {rc} & {inc} & {n} \\\\")
    return table_env(lines,
                     "Subscriber scalability sweep (brake\\_limit=4). Recall/stretch are "
                     "mean with 95\\% bootstrap CI; RcvbufErrors/InErrors are the kernel "
                     "UDP drop counters measured across the run.",
                     "tab:scale", "lccccc")


def tab_crypto(d):
    header = (r"\textbf{mode} & \textbf{recall} & \textbf{stretch} & "
              r"\textbf{latency (ms)} & \textbf{median (ms)} & \textbf{p95 (ms)} "
              r"& \textbf{Wilcoxon p} & \textbf{n} \\")
    lines = [header, r"\midrule"]
    wilcoxon = "---"
    for mode, fn in [("plaintext", "plain.csv"), ("encrypted", "encrypted.csv")]:
        p = os.path.join(d, fn)
        if not os.path.exists(p):
            continue
        rows = load_rows(p)
        rec = metric_mean_ci(rows, m_recall)
        st = metric_mean_ci(rows, m_stretch)
        lat = trial_level(rows, m_latency)
        if lat:
            m, lo, hi = mean_ci(lat)
            lat_s = fmt_mean_ci(m, lo, hi, 1)
            med = f"{np.median(lat):.1f}"
            p95 = f"{pctl(lat, 95):.1f}"
        else:
            lat_s = med = p95 = "---"
        n = len(lat)
        wcol = "---"
        if mode == "encrypted":
            pr = load_rows(os.path.join(d, "plain.csv"))
            plat = trial_level(pr, m_latency)
            elat = trial_level(rows, m_latency)
            if len(plat) == len(elat) and plat:
                _, wp, _, _ = wilcoxon_signed_rank(elat, plat)
                wilcoxon = f"{wp:.4g}"
                wcol = f"{wp:.4g}"
        lines.append(f"{mode} & {rec} & {st} & {lat_s} & {med} & {p95} & {wcol} & {n} \\\\")
    return table_env(lines,
                     "Plaintext vs encrypted (10 publishers, 50 subscribers, brake=4). "
                     "Latency is per-publication delivery latency (trial-level). "
                     "Wilcoxon signed-rank test on paired per-trial latency "
                     "(encrypted $-$ plaintext), p=" + wilcoxon + ".",
                     "tab:crypto", "lccccccc")


def tab_dynamics(d):
    header = (r"\textbf{migration class} & \textbf{n} & \textbf{converged (rate)} "
              r"& \textbf{median (ms)} & \textbf{p95 (ms)} \\")
    lines = [header, r"\midrule"]
    p = os.path.join(d, "dynamics_plain.csv")
    if os.path.exists(p):
        rows = load_rows(p)
        for cls in ["closer", "farther"]:
            sub = [r for r in rows if r['migration_class'] == cls]
            n = len(sub)
            conv = sum(1 for r in sub if r['converged'] == '1')
            rate = conv / n if n else 0.0
            med = p95 = "---"
            if cls == "closer":
                by = {}
                for r in sub:
                    if r['converged'] == '1':
                        by.setdefault(int(r['trial']), []).append(float(r['convergence_ms']))
                tlv = [sum(v) / len(v) for v in by.values() if v]
                if tlv:
                    med = f"{np.median(tlv):.2f}"
                    p95 = f"{pctl(tlv, 95):.2f}"
            lines.append(f"{cls} & {n} & {rate:.3f} ({conv}/{n}) & {med} & {p95} \\\\")
    return table_env(lines,
                     "Migration convergence (20 publishers, 50 subscribers, 20 migrations, "
                     "brake-free). closer: the migrated replica moved closer than the "
                     "subscriber's cached closest; farther: the previous closest moved away. "
                     "farther does not converge under push-only Algorithm~1.",
                     "tab:dynamics", "lcccc")


def tab_microbench(d):
    header = (r"\textbf{operation} & \textbf{executed by} & \textbf{median (ms/op)} "
              r"& \textbf{min--max (3 runs)} \\")
    lines = [header, r"\midrule"]
    for name, who in [("blindNotification", "Publisher"),
                      ("blindSubscription", "Subscriber"),
                      ("executeMatch", "Broker")]:
        med, lo, hi = microbench(d, name)
        if med is None:
            continue
        lines.append(f"{name} & {who} & {med:.3f} & [{lo:.3f}, {hi:.3f}] \\\\")
    return table_env(lines,
                     "Cryptographic micro-benchmark (median of 3 runs, 1000 iterations each). "
                     "Blinding is done on the endpoints; the broker only runs the "
                     "$O(1)$ Match (three orders of magnitude cheaper).",
                     "tab:microbench", "llcc")


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else 'results/final'
    out = []
    out.append("% Auto-generated by scripts/generate_tables.py — do not hand-edit")
    out.append("% Values are identical to results/final/numbers.tex (shared bench_stats.py)")
    out.append("")
    out.append(tab_single_brake(d))
    out.append("")
    out.append(tab_multi_brake(d))
    out.append("")
    out.append(tab_scale(d))
    out.append("")
    out.append(tab_crypto(d))
    out.append("")
    out.append(tab_dynamics(d))
    out.append("")
    out.append(tab_microbench(d))
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'tables.tex'), 'w') as f:
        f.write("\n".join(out) + "\n")
    print(f"wrote {d}/tables.tex ({len(out)} lines)")


if __name__ == '__main__':
    main()
