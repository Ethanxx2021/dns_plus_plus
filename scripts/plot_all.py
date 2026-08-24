#!/usr/bin/env python3
"""
DNS++ 正式实验图生成器(改造版)。

读取 results/final/ 下的正式数据,生成修正画法的图:
  - 误差棒改用 bootstrap 95% percentile CI(不再用对称 std,避免越界)
  - stretch 增加 CDF
  - 新增 dynamics 收敛时间 CDF(closer vs farther)
  - 图上标注 n(trial 数)

用法: venv/bin/python scripts/plot_all.py [results_dir]
输出: <results_dir>/figures/*.png
"""

import csv
import os
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

RNG = np.random.default_rng(20260824)
N_BOOT = 10000

plt.rcParams.update({
    "font.family": "serif",
    "font.size": 11,
    "axes.labelsize": 12,
    "axes.titlesize": 12,
    "figure.dpi": 200,
    "axes.grid": True,
    "grid.alpha": 0.3,
    "grid.linestyle": "--",
    "axes.spines.top": False,
    "axes.spines.right": False,
})

C = {"green": "#2ca02c", "red": "#d62728", "blue": "#1f77b4",
     "orange": "#ff7f0e", "purple": "#9467bd", "gray": "#7f7f7f"}


# --------------------------------------------------------------------------
def load_rows(path):
    with open(path, newline='') as f:
        return list(csv.DictReader(f))


def trial_level(rows, metric):
    by = {}
    for r in rows:
        v = metric(r)
        if v is None:
            continue
        by.setdefault(int(r['trial']), []).append(v)
    return [sum(v) / len(v) for v in by.values() if v]


def boot_ci(values):
    v = np.asarray(values, dtype=float)
    means = RNG.choice(v, size=(N_BOOT, len(v)), replace=True).mean(axis=1)
    return float(np.percentile(means, 2.5)), float(np.percentile(means, 97.5))


def m_recall(r):
    return float(r['recall'])


def m_stretch(r):
    s = float(r['stretch'])
    return s if s > 0 else None


def m_latency(r):
    v = float(r['latency_per_pub_ms'])
    return v if v >= 0 else None


def traffic_ratios(log_path):
    out = []
    if not os.path.exists(log_path):
        return out
    import re
    for line in open(log_path, errors='replace'):
        if line.startswith('Trial') and 'Traffic Ratio=' in line:
            m = re.search(r'Traffic Ratio=([0-9.]+)', line)
            if m:
                out.append(float(m.group(1)))
    return out


# --------------------------------------------------------------------------
def fig_single_brake(d, outdir):
    configs = [(1, '1'), (2, '2'), (4, '4'), (1000, '∞')]
    rec, stre = [], []
    labels = []
    for L, lab in configs:
        p = os.path.join(d, f'single_brake_{L}.csv')
        if not os.path.exists(p):
            continue
        rows = load_rows(p)
        r = trial_level(rows, m_recall)
        s = trial_level(rows, m_stretch)
        if r:
            rec.append(r); stre.append(s); labels.append(lab)
    if not rec:
        return
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.5))
    x = np.arange(len(rec))
    for ax, data, name, ylab in [
        (axes[0], rec, 'Recall', 'Recall'),
        (axes[1], stre, 'Stretch', 'Stretch'),
    ]:
        means = [np.mean(v) for v in data]
        los = []; his = []
        for v in data:
            lo, hi = boot_ci(v); los.append(lo); his.append(hi)
        yerr = [[m - lo for m, lo in zip(means, los)], [hi - m for m, hi in zip(means, his)]]
        ax.errorbar(x, means, yerr=yerr, marker='o', color=C['green'] if name == 'Recall' else C['red'],
                    linewidth=1.5, markersize=6, capsize=4, capthick=1.2)
        ax.set_xticks(x); ax.set_xticklabels(labels)
        ax.set_ylabel(ylab); ax.set_title(f'({ "a" if name=="Recall" else "b" }) {name} vs brake limit')
        ax.annotate(f'n={len(data[0])} trials', xy=(0.02, 0.02), xycoords='axes fraction',
                    fontsize=9, color=C['gray'])
        if name == 'Recall':
            ax.set_ylim(0, 1.05); ax.axhline(1.0, color=C['gray'], ls=':', alpha=0.5)
        else:
            ax.axhline(1.0, color=C['gray'], ls=':', alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig_single_brake.png'), bbox_inches='tight')
    plt.close(fig)
    print('fig_single_brake.png')


def fig_multi_brake(d, outdir):
    configs = [(1, '1'), (2, '2'), (4, '4'), (1000, '∞')]
    rec, stre, tr = [], [], []
    labels = []
    for L, lab in configs:
        p = os.path.join(d, f'multi_brake_{L}.csv')
        if not os.path.exists(p):
            continue
        rows = load_rows(p)
        r = trial_level(rows, m_recall)
        s = trial_level(rows, m_stretch)
        t = traffic_ratios(os.path.join(d, f'multi_brake_{L}.log'))
        if r and t:
            rec.append(r); stre.append(s); tr.append(t); labels.append(lab)
    if not rec:
        return
    fig, axes = plt.subplots(1, 3, figsize=(13, 3.5))
    x = np.arange(len(rec))
    for ax, data, ylab, color, title in [
        (axes[0], rec, 'Recall', C['green'], '(a) Recall'),
        (axes[1], stre, 'Stretch', C['red'], '(b) Stretch'),
        (axes[2], tr, 'Traffic ratio', C['blue'], '(c) Traffic ratio'),
    ]:
        means = [np.mean(v) for v in data]
        los = []; his = []
        for v in data:
            lo, hi = boot_ci(v); los.append(lo); his.append(hi)
        yerr = [[m - lo for m, lo in zip(means, los)], [hi - m for m, hi in zip(means, his)]]
        ax.errorbar(x, means, yerr=yerr, marker='o', color=color, linewidth=1.5,
                    markersize=6, capsize=4, capthick=1.2)
        ax.set_xticks(x); ax.set_xticklabels(labels)
        ax.set_ylabel(ylab); ax.set_title(f'{title} vs brake limit')
        ax.annotate(f'n={len(data[0])} trials', xy=(0.02, 0.02), xycoords='axes fraction',
                    fontsize=9, color=C['gray'])
        ax.axhline(1.0, color=C['gray'], ls=':', alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig_multi_brake.png'), bbox_inches='tight')
    plt.close(fig)
    print('fig_multi_brake.png')


def fig_scale(d, outdir):
    configs = [(10, '10'), (50, '50'), (200, '200'), (500, '500'), (1000, '1000')]
    rec, stre = [], []
    labels = []
    for N, lab in configs:
        p = os.path.join(d, f'sweep_{N}.csv')
        if not os.path.exists(p):
            continue
        rows = load_rows(p)
        r = trial_level(rows, m_recall)
        s = trial_level(rows, m_stretch)
        if r:
            rec.append(r); stre.append(s); labels.append(lab)
    if not rec:
        return
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.5))
    x = np.arange(len(rec))
    for ax, data, name, ylab in [
        (axes[0], rec, 'Recall', 'Recall'),
        (axes[1], stre, 'Stretch', 'Stretch'),
    ]:
        means = [np.mean(v) for v in data]
        los = []; his = []
        for v in data:
            lo, hi = boot_ci(v); los.append(lo); his.append(hi)
        yerr = [[m - lo for m, lo in zip(means, los)], [hi - m for m, hi in zip(means, his)]]
        ax.errorbar(x, means, yerr=yerr, marker='o', color=C['green'] if name == 'Recall' else C['red'],
                    linewidth=1.5, markersize=6, capsize=4, capthick=1.2)
        ax.set_xticks(x); ax.set_xticklabels(labels)
        ax.set_ylabel(ylab); ax.set_title(f'({ "a" if name=="Recall" else "b" }) {name} vs subscribers')
        ax.annotate(f'n={len(data[0])} trials', xy=(0.02, 0.02), xycoords='axes fraction',
                    fontsize=9, color=C['gray'])
        if name == 'Recall':
            ax.set_ylim(0, 1.05)
        ax.axhline(1.0, color=C['gray'], ls=':', alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig_scale.png'), bbox_inches='tight')
    plt.close(fig)
    print('fig_scale.png')


def fig_stretch_cdf(d, outdir):
    configs = [(1, '1'), (2, '2'), (4, '4'), (1000, '∞')]
    fig, ax = plt.subplots(figsize=(5, 3.5))
    colors = [C['red'], C['orange'], C['green'], C['blue']]
    for (L, lab), color in zip(configs, colors):
        p = os.path.join(d, f'single_brake_{L}.csv')
        if not os.path.exists(p):
            continue
        rows = load_rows(p)
        s = trial_level(rows, m_stretch)   # trial 级 stretch 均值
        if not s:
            continue
        s = np.sort(s)
        y = np.arange(1, len(s) + 1) / len(s)
        ax.step(np.concatenate([[1.0], s]), np.concatenate([[0.0], y]),
                where='post', color=color, label=f'brake={lab}')
    ax.set_xlabel('Stretch'); ax.set_ylabel('CDF')
    ax.set_title('Stretch CDF (single broker, trial-level)')
    ax.axvline(1.0, color=C['gray'], ls=':', alpha=0.5)
    ax.legend(fontsize=9)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig_stretch_cdf.png'), bbox_inches='tight')
    plt.close(fig)
    print('fig_stretch_cdf.png')


def fig_crypto(d, outdir):
    pp, ep = os.path.join(d, 'plain.csv'), os.path.join(d, 'encrypted.csv')
    if not (os.path.exists(pp) and os.path.exists(ep)):
        return
    pr = load_rows(pp); er = load_rows(ep)
    plat = trial_level(pr, m_latency)
    elat = trial_level(er, m_latency)
    if not plat or not elat:
        return
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.5))
    # bar with bootstrap CI
    ax = axes[0]
    names = ['Plaintext', 'Encrypted']
    means = [np.mean(plat), np.mean(elat)]
    los = []; his = []
    for v in (plat, elat):
        lo, hi = boot_ci(v); los.append(lo); his.append(hi)
    yerr = [[m - lo for m, lo in zip(means, los)], [hi - m for m, hi in zip(means, his)]]
    ax.bar(names, means, yerr=yerr, capsize=5, color=[C['blue'], C['orange']],
           edgecolor='black', width=0.5, error_kw={'elinewidth': 1.2})
    ax.set_ylabel('latency_per_pub (ms)')
    ax.set_title('(a) Delivery latency (95% bootstrap CI)')
    ax.annotate(f'n={len(plat)} trials', xy=(0.02, 0.02), xycoords='axes fraction',
                fontsize=9, color=C['gray'])
    # CDF
    ax = axes[1]
    for data, lab, color in [(plat, 'Plaintext', C['blue']), (elat, 'Encrypted', C['orange'])]:
        s = np.sort(data)
        y = np.arange(1, len(s) + 1) / len(s)
        ax.step(np.concatenate([[s[0]], s]), np.concatenate([[0.0], y]),
                where='post', color=color, label=lab)
    ax.set_xlabel('latency_per_pub (ms)'); ax.set_ylabel('CDF')
    ax.set_title('(b) Latency CDF (trial-level)')
    ax.legend(fontsize=9)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig_crypto.png'), bbox_inches='tight')
    plt.close(fig)
    print('fig_crypto.png')


def fig_dynamics_cdf(d, outdir):
    p = os.path.join(d, 'dynamics_plain.csv')
    if not os.path.exists(p):
        print('fig_dynamics_cdf.png: SKIP (no dynamics_plain.csv)')
        return
    rows = load_rows(p)
    closer_ms = [float(r['convergence_ms']) for r in rows
                 if r['migration_class'] == 'closer' and r['converged'] == '1']
    n_farther = sum(1 for r in rows if r['migration_class'] == 'farther')
    n_farther_conv = sum(1 for r in rows if r['migration_class'] == 'farther' and r['converged'] == '1')

    fig, ax = plt.subplots(figsize=(5.5, 3.5))
    if closer_ms:
        s = np.sort(closer_ms)
        y = np.arange(1, len(s) + 1) / len(s)
        ax.step(np.concatenate([[0.0], s]), np.concatenate([[0.0], y]),
                where='post', color=C['green'], label=f'closer (n={len(closer_ms)} converged)')
        ax.axvline(np.median(s), color=C['green'], ls=':', alpha=0.5)
        ax.text(np.median(s), 0.5, f"median={np.median(s):.2f} ms",
                rotation=90, va='center', ha='right', fontsize=8, color=C['green'])
    # farther 不收敛:画一条在超时处的竖线标注,而不是空白
    ax.axvline(0, color=C['red'], ls='-', alpha=0.8)
    ax.annotate(f'farther: {n_farther_conv}/{n_farther} converged (design: not pushed)',
                xy=(0.98, 0.05), xycoords='axes fraction', ha='right',
                fontsize=9, color=C['red'])
    ax.set_xlabel('convergence time (ms)')
    ax.set_ylabel('CDF')
    ax.set_title('Migration convergence CDF (closer vs farther)')
    ax.set_xlim(left=0)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig_dynamics_cdf.png'), bbox_inches='tight')
    plt.close(fig)
    print('fig_dynamics_cdf.png')


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else 'results/final'
    outdir = os.path.join(d, 'figures')
    os.makedirs(outdir, exist_ok=True)
    fig_single_brake(d, outdir)
    fig_multi_brake(d, outdir)
    fig_scale(d, outdir)
    fig_stretch_cdf(d, outdir)
    fig_crypto(d, outdir)
    fig_dynamics_cdf(d, outdir)
    print(f'figures written to {outdir}/')


if __name__ == '__main__':
    main()
