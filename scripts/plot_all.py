#!/usr/bin/env python3
"""
DNS++ Complete Figure Generator
Generates all Phase 1-3 figures to top-journal standards.
"""

import csv
import sys
import os
import statistics
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# ============================================================
# Global Style Settings (SIGCOMM / TMLR Standard)
# ============================================================
plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "DejaVu Serif", "Liberation Serif"],
    "font.size": 11,
    "axes.labelsize": 12,
    "axes.titlesize": 12,
    "xtick.labelsize": 10,
    "ytick.labelsize": 10,
    "legend.fontsize": 10,
    "figure.dpi": 300,
    "axes.grid": True,
    "grid.alpha": 0.3,
    "grid.linestyle": "--",
    "axes.spines.top": False,
    "axes.spines.right": False,
})

# Color palette (colorblind-friendly)
COLORS = {
    "green": "#2ca02c",
    "red": "#d62728",
    "blue": "#1f77b4",
    "orange": "#ff7f0e",
    "purple": "#9467bd",
    "gray": "#7f7f7f",
}

# ============================================================
# Helper: Parse bench_broker CSV
# ============================================================
def parse_csv(filename):
    trials = {}
    with open(filename, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            t = int(row['trial'])
            if t not in trials:
                trials[t] = []
            trials[t].append(row)
    return trials

def compute_stats(trials):
    recalls = []
    stretches = []
    latencies = []
    for t, subs in trials.items():
        recalls.append(statistics.mean(int(s['recall']) for s in subs))
        st = [float(s['stretch']) for s in subs if float(s['stretch']) > 0]
        if st:
            stretches.append(statistics.mean(st))
        # Phase 1 CSVs don't have latency_ms column
        if 'latency_ms' in subs[0]:
            lat = [float(s['latency_ms']) for s in subs if float(s['latency_ms']) > 0]
            if lat:
                latencies.append(statistics.mean(lat))
    return {
        'recall_mean': statistics.mean(recalls) if recalls else 0,
        'recall_std': statistics.stdev(recalls) if len(recalls) > 1 else 0,
        'stretch_mean': statistics.mean(stretches) if stretches else 0,
        'stretch_std': statistics.stdev(stretches) if len(stretches) > 1 else 0,
        'latency_mean': statistics.mean(latencies) if latencies else 0,
        'latency_std': statistics.stdev(latencies) if len(latencies) > 1 else 0,
    }
# ============================================================
# Helper: Parse bench_multi_broker log
# ============================================================
def parse_multi_log(filename):
    trials = []
    with open(filename, 'r') as f:
        for line in f:
            if line.startswith('Trial'):
                parts = line.strip().split()
                recall_str = next((p for p in parts if p.startswith('recall=')), None)
                stretch_str = next((p for p in parts if p.startswith('avg_stretch=')), None)
                tr_str = next((p for p in parts if p.startswith('Ratio=')), None)
                if recall_str and stretch_str and tr_str:
                    trials.append({
                        'recall': float(recall_str.split('=')[1]),
                        'stretch': float(stretch_str.split('=')[1]),
                        'traffic_ratio': float(tr_str.split('=')[1]),
                    })
    return trials

def compute_multi_stats(trials):
    if not trials:
        return None
    return {
        'recall_mean': statistics.mean(t['recall'] for t in trials),
        'recall_std': statistics.stdev(t['recall'] for t in trials) if len(trials) > 1 else 0,
        'stretch_mean': statistics.mean(t['stretch'] for t in trials),
        'stretch_std': statistics.stdev(t['stretch'] for t in trials) if len(trials) > 1 else 0,
        'tr_mean': statistics.mean(t['traffic_ratio'] for t in trials),
        'tr_std': statistics.stdev(t['traffic_ratio'] for t in trials) if len(trials) > 1 else 0,
    }

# ============================================================
# Helper: Parse scalability sweep log
# ============================================================
def parse_sweep_log(filename):
    trials = []
    with open(filename, 'r') as f:
        for line in f:
            if line.startswith('Trial'):
                parts = line.strip().split()
                recall_str = next((p for p in parts if p.startswith('recall=')), None)
                stretch_str = next((p for p in parts if p.startswith('avg_stretch=')), None)
                if recall_str and stretch_str:
                    trials.append({
                        'recall': float(recall_str.split('=')[1]),
                        'stretch': float(stretch_str.split('=')[1]),
                    })
    return trials

# ============================================================
# Figure 1: Phase 1 Brake Sweep (Line Chart)
# ============================================================
def plot_phase1_brake_sweep():
    configs = [
        (1, 'brake_1.csv', '1'),
        (2, 'brake_2.csv', '2'),
        (4, 'brake_4.csv', '4'),
        (1000, 'brake_1000.csv', '∞'),
    ]
    
    results = []
    for b, f, label in configs:
        if os.path.exists(f):
            stats = compute_stats(parse_csv(f))
            stats['label'] = label
            results.append(stats)
    
    if len(results) < 2:
        print("Skipping Phase 1: not enough CSV files")
        return
    
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 3.5))
    
    x = range(len(results))
    x_labels = [r['label'] for r in results]
    
    # (a) Recall
    recalls = [r['recall_mean'] for r in results]
    recall_errs = [r['recall_std'] for r in results]
    ax1.errorbar(x, recalls, yerr=recall_errs, marker='o', color=COLORS['green'],
                 linewidth=1.5, markersize=7, capsize=4, capthick=1.5)
    ax1.set_ylabel('Recall')
    ax1.set_title('(a) Recall vs Brake Limit')
    ax1.set_xticks(x)
    ax1.set_xticklabels(x_labels)
    ax1.set_ylim(0, 1.15)
    ax1.axhline(y=1.0, color=COLORS['gray'], linestyle=':', alpha=0.5)
    
    # (b) Stretch
    stretches = [r['stretch_mean'] for r in results]
    stretch_errs = [r['stretch_std'] for r in results]
    ax2.errorbar(x, stretches, yerr=stretch_errs, marker='s', color=COLORS['red'],
                 linewidth=1.5, markersize=7, capsize=4, capthick=1.5)
    ax2.set_ylabel('Average Stretch')
    ax2.set_title('(b) Stretch vs Brake Limit')
    ax2.set_xticks(x)
    ax2.set_xticklabels(x_labels)
    ax2.set_ylim(0.8, max(s + 0.3 for s in stretches))
    ax2.axhline(y=1.0, color=COLORS['gray'], linestyle=':', alpha=0.5)
    
    plt.tight_layout()
    plt.savefig('phase1_brake_sweep.png', bbox_inches='tight')
    plt.close()
    print("Generated: phase1_brake_sweep.png")

# ============================================================
# Figure 2: Phase 2 Multi-Broker (3-panel)
# ============================================================
def plot_phase2_multi_broker():
    configs = [
        (1, 'multi_brake1.log', '1'),
        (2, 'multi_brake2.log', '2'),
        (4, 'multi_brake4.log', '4'),
        (1000, 'multi_brake_inf.log', '∞'),
    ]
    
    results = []
    for b, f, label in configs:
        if os.path.exists(f):
            stats = compute_multi_stats(parse_multi_log(f))
            if stats:
                stats['label'] = label
                results.append(stats)
    
    if len(results) < 2:
        print("Skipping Phase 2 multi-broker: not enough log files")
        return
    
    fig, (ax1, ax2, ax3) = plt.subplots(1, 3, figsize=(13, 3.5))
    
    x = range(len(results))
    x_labels = [r['label'] for r in results]
    
    # (a) Recall
    recalls = [r['recall_mean'] for r in results]
    recall_errs = [r['recall_std'] for r in results]
    ax1.errorbar(x, recalls, yerr=recall_errs, marker='o', color=COLORS['green'],
                 linewidth=1.5, markersize=7, capsize=4, capthick=1.5)
    ax1.set_ylabel('Recall')
    ax1.set_title('(a) Recall vs Brake Limit')
    ax1.set_xticks(x)
    ax1.set_xticklabels(x_labels)
    ax1.set_ylim(0.85, 1.05)
    
    # (b) Stretch
    stretches = [r['stretch_mean'] for r in results]
    stretch_errs = [r['stretch_std'] for r in results]
    ax2.errorbar(x, stretches, yerr=stretch_errs, marker='s', color=COLORS['red'],
                 linewidth=1.5, markersize=7, capsize=4, capthick=1.5)
    ax2.set_ylabel('Average Stretch')
    ax2.set_title('(b) Stretch vs Brake Limit')
    ax2.set_xticks(x)
    ax2.set_xticklabels(x_labels)
    ax2.set_ylim(1.0, max(s + 0.02 for s in stretches))
    
    # (c) Traffic Ratio
    trs = [r['tr_mean'] for r in results]
    tr_errs = [r['tr_std'] for r in results]
    ax3.errorbar(x, trs, yerr=tr_errs, marker='D', color=COLORS['blue'],
                 linewidth=1.5, markersize=7, capsize=4, capthick=1.5)
    ax3.set_ylabel('Traffic Ratio')
    ax3.set_title('(c) Traffic Ratio vs Brake Limit')
    ax3.set_xticks(x)
    ax3.set_xticklabels(x_labels)
    ax3.set_ylim(1.0, max(t + 0.05 for t in trs))
    ax3.axhline(y=1.0, color=COLORS['gray'], linestyle=':', alpha=0.5)
    
    plt.tight_layout()
    plt.savefig('phase2_multi_broker.png', bbox_inches='tight')
    plt.close()
    print("Generated: phase2_multi_broker.png")

# ============================================================
# Figure 3: Phase 2 Scalability (Line Chart)
# ============================================================
def plot_phase2_scalability():
    configs = [
        (10, 'sweep_10.log', '10'),
        (50, 'sweep_50.log', '50'),
        (200, 'sweep_200.log', '200'),
        (500, 'sweep_500.log', '500'),
        (1000, 'sweep_1000.log', '1000'),
    ]
    
    results = []
    for subs, f, label in configs:
        if os.path.exists(f):
            trials = parse_sweep_log(f)
            if trials:
                stats = {
                    'recall_mean': statistics.mean(t['recall'] for t in trials),
                    'recall_std': statistics.stdev(t['recall'] for t in trials) if len(trials) > 1 else 0,
                    'stretch_mean': statistics.mean(t['stretch'] for t in trials),
                    'stretch_std': statistics.stdev(t['stretch'] for t in trials) if len(trials) > 1 else 0,
                    'label': label,
                    'subs': subs,
                }
                results.append(stats)
    
    if len(results) < 2:
        print("Skipping Phase 2 scalability: not enough log files")
        return
    
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 3.5))
    
    x = range(len(results))
    x_labels = [r['label'] for r in results]
    
    # (a) Recall
    recalls = [r['recall_mean'] for r in results]
    recall_errs = [r['recall_std'] for r in results]
    ax1.errorbar(x, recalls, yerr=recall_errs, marker='o', color=COLORS['green'],
                 linewidth=1.5, markersize=7, capsize=4, capthick=1.5)
    ax1.set_ylabel('Recall')
    ax1.set_title('(a) Recall vs Subscriber Count')
    ax1.set_xticks(x)
    ax1.set_xticklabels(x_labels)
    ax1.set_ylim(0.9, 1.05)
    ax1.axhline(y=1.0, color=COLORS['gray'], linestyle=':', alpha=0.5)
    
    # (b) Stretch
    stretches = [r['stretch_mean'] for r in results]
    stretch_errs = [r['stretch_std'] for r in results]
    ax2.errorbar(x, stretches, yerr=stretch_errs, marker='s', color=COLORS['red'],
                 linewidth=1.5, markersize=7, capsize=4, capthick=1.5)
    ax2.set_ylabel('Average Stretch')
    ax2.set_title('(b) Stretch vs Subscriber Count')
    ax2.set_xticks(x)
    ax2.set_xticklabels(x_labels)
    ax2.set_ylim(0.98, 1.02)
    ax2.axhline(y=1.0, color=COLORS['gray'], linestyle=':', alpha=0.5)
    
    plt.tight_layout()
    plt.savefig('phase2_scalability.png', bbox_inches='tight')
    plt.close()
    print("Generated: phase2_scalability.png")

# ============================================================
# Figure 4: Phase 3 Crypto Comparison (2-panel: Bar + Histogram)
# ============================================================
def plot_phase3_crypto():
    results = []
    for file, label, color in [('plain.csv', 'Plaintext', COLORS['blue']), 
                               ('encrypted.csv', 'Encrypted\n(Paillier)', COLORS['orange'])]:
        if os.path.exists(file):
            trials = parse_csv(file)
            lats = []
            recalls = []
            stretches = []
            for t, subs in trials.items():
                for s in subs:
                    if 'latency_ms' in s:
                        lat = float(s['latency_ms'])
                        if lat > 0: lats.append(lat)
                    recalls.append(int(s['recall']))
                    st = float(s['stretch'])
                    if st > 0: stretches.append(st)
            results.append({
                'label': label, 'color': color, 'lats': lats, 
                'recalls': recalls, 'stretches': stretches
            })
    
    if len(results) < 2:
        print("Skipping Phase 3: CSV files not found")
        return

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 3.5))
    
    # Panel 1: Bar chart with mean + std
    x_pos = np.arange(len(results))
    means = [statistics.mean(r['lats']) for r in results]
    stds = [statistics.stdev(r['lats']) for r in results]
    
    bars = ax1.bar(x_pos, means, yerr=stds, capsize=5,
                   color=[r['color'] for r in results], edgecolor='black', linewidth=0.8, width=0.5,
                   error_kw={'elinewidth': 1.5, 'capthick': 1.5})
    ax1.set_ylabel('Latency (ms)')
    ax1.set_title('(a) Mean Latency Comparison')
    ax1.set_xticks(x_pos)
    ax1.set_xticklabels([r['label'] for r in results])
    ax1.set_ylim(0, max(m + s + 20 for m, s in zip(means, stds)))
    
    for bar, val in zip(bars, means):
        ax1.text(bar.get_x() + bar.get_width() / 2., bar.get_height() + 5,
                 f'{val:.1f} ms', ha='center', va='bottom', fontweight='bold', fontsize=11)
    
    # Panel 2: Histogram overlay
    for r in results:
        ax2.hist(r['lats'], bins=30, alpha=0.6, 
                 color=r['color'], label=r['label'].replace('\n', ' '), edgecolor='black', linewidth=0.5)
    
    ax2.set_xlabel('Latency (ms)')
    ax2.set_ylabel('Frequency')
    ax2.set_title('(b) Latency Distribution')
    ax2.legend()
    
    plt.tight_layout()
    plt.savefig('phase3_crypto_comparison.png', bbox_inches='tight')
    plt.close()
    print("Generated: phase3_crypto_comparison.png")

# ============================================================
# Main
# ============================================================
if __name__ == '__main__':
    print("=== DNS++ Figure Generator ===\n")
    plot_phase1_brake_sweep()
    plot_phase2_multi_broker()
    plot_phase2_scalability()
    plot_phase3_crypto()
    print("\nDone. Upload the generated PNG files to Overleaf.")