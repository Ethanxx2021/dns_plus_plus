#!/usr/bin/env python3
"""
DNS++ Phase 2 Multi-Broker Benchmark Plotter
"""

import csv
import sys
import statistics
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

def parse_log(filename):
    """Parse bench_multi_broker stderr log for per-trial summary."""
    trials = []
    with open(filename, 'r') as f:
        for line in f:
            if line.startswith('Trial'):
                parts = line.strip().split()
                
                # Robustly find the metrics by prefix
                recall_str = next((p for p in parts if p.startswith('recall=')), None)
                stretch_str = next((p for p in parts if p.startswith('avg_stretch=')), None)
                tr_str = next((p for p in parts if p.startswith('Ratio=')), None)
                
                if recall_str and stretch_str and tr_str:
                    recall = float(recall_str.split('=')[1])
                    stretch = float(stretch_str.split('=')[1])
                    traffic_ratio = float(tr_str.split('=')[1])
                    trials.append({
                        'recall': recall,
                        'stretch': stretch,
                        'traffic_ratio': traffic_ratio,
                    })
    return trials

def compute_stats(trials):
    return {
        'recall_mean': statistics.mean([t['recall'] for t in trials]),
        'recall_std': statistics.stdev([t['recall'] for t in trials]) if len(trials) > 1 else 0,
        'stretch_mean': statistics.mean([t['stretch'] for t in trials]),
        'stretch_std': statistics.stdev([t['stretch'] for t in trials]) if len(trials) > 1 else 0,
        'tr_mean': statistics.mean([t['traffic_ratio'] for t in trials]),
        'tr_std': statistics.stdev([t['traffic_ratio'] for t in trials]) if len(trials) > 1 else 0,
        'n_trials': len(trials),
    }

def main():
    configs = [
        (1, 'multi_brake1.log', 'Brake=1'),
        (2, 'multi_brake2.log', 'Brake=2'),
        (4, 'multi_brake4.log', 'Brake=4'),
        (1000, 'multi_brake_inf.log', 'Unbounded'),
    ]
    
    results = []
    for brake_val, log_file, label in configs:
        try:
            trials = parse_log(log_file)
            stats = compute_stats(trials)
            stats['label'] = label
            stats['brake'] = brake_val
            results.append(stats)
            print(f"{label}: recall={stats['recall_mean']:.3f}±{stats['recall_std']:.3f}, "
                  f"stretch={stats['stretch_mean']:.3f}±{stats['stretch_std']:.3f}, "
                  f"traffic_ratio={stats['tr_mean']:.3f}±{stats['tr_std']:.3f}, "
                  f"trials={stats['n_trials']}")
        except FileNotFoundError:
            print(f"Warning: {log_file} not found, skipping")
    
    if len(results) < 2:
        print("Error: Need at least 2 configurations")
        sys.exit(1)
    
    # --- Generate 3-panel figure ---
    fig, axes = plt.subplots(1, 3, figsize=(15, 5))
    
    x_labels = [r['label'] for r in results]
    x_pos = np.arange(len(results))
    colors = ['#e74c3c', '#e67e22', '#f1c40f', '#2ecc71']
    
    # Plot 1: Recall
    ax1 = axes[0]
    recalls = [r['recall_mean'] for r in results]
    recall_errs = [r['recall_std'] for r in results]
    bars1 = ax1.bar(x_pos, recalls, yerr=recall_errs, capsize=5,
                     color=colors, edgecolor='black', linewidth=0.8)
    ax1.set_ylabel('Recall', fontsize=13)
    ax1.set_title('(a) Recall vs Brake Limit', fontsize=13)
    ax1.set_xticks(x_pos)
    ax1.set_xticklabels(x_labels, fontsize=11)
    ax1.set_ylim(0.8, 1.05)
    ax1.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5)
    ax1.grid(axis='y', alpha=0.3)
    for bar, val in zip(bars1, recalls):
        ax1.text(bar.get_x() + bar.get_width()/2., bar.get_height() + 0.01,
                 f'{val:.2f}', ha='center', va='bottom', fontsize=11, fontweight='bold')
    
    # Plot 2: Stretch
    ax2 = axes[1]
    stretches = [r['stretch_mean'] for r in results]
    stretch_errs = [r['stretch_std'] for r in results]
    bars2 = ax2.bar(x_pos, stretches, yerr=stretch_errs, capsize=5,
                     color=colors, edgecolor='black', linewidth=0.8)
    ax2.set_ylabel('Average Stretch', fontsize=13)
    ax2.set_title('(b) Stretch vs Brake Limit', fontsize=13)
    ax2.set_xticks(x_pos)
    ax2.set_xticklabels(x_labels, fontsize=11)
    ax2.set_ylim(0.98, max(s + 0.05 for s in stretches))
    ax2.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5)
    ax2.grid(axis='y', alpha=0.3)
    for bar, val in zip(bars2, stretches):
        ax2.text(bar.get_x() + bar.get_width()/2., bar.get_height() + 0.005,
                 f'{val:.3f}', ha='center', va='bottom', fontsize=11, fontweight='bold')
    
    # Plot 3: Traffic Ratio
    ax3 = axes[2]
    trs = [r['tr_mean'] for r in results]
    tr_errs = [r['tr_std'] for r in results]
    bars3 = ax3.bar(x_pos, trs, yerr=tr_errs, capsize=5,
                     color=colors, edgecolor='black', linewidth=0.8)
    ax3.set_ylabel('Traffic Ratio', fontsize=13)
    ax3.set_title('(c) Traffic Ratio vs Brake Limit', fontsize=13)
    ax3.set_xticks(x_pos)
    ax3.set_xticklabels(x_labels, fontsize=11)
    ax3.set_ylim(1.0, max(t + 0.05 for t in trs))
    ax3.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5, label='Optimal (1.0)')
    ax3.grid(axis='y', alpha=0.3)
    for bar, val in zip(bars3, trs):
        ax3.text(bar.get_x() + bar.get_width()/2., bar.get_height() + 0.005,
                 f'{val:.3f}', ha='center', va='bottom', fontsize=11, fontweight='bold')
    
    plt.tight_layout()
    output_file = 'phase2_multi_broker.png'
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"\nChart saved to: {output_file}")
    
    # --- LaTeX Table ---
    print("\n--- LaTeX Table ---")
    print(r"\begin{table}[htbp]")
    print(r"\centering")
    print(r"\begin{tabular}{lcccc}")
    print(r"\toprule")
    print(r"Brake Limit & Recall & Stretch & Traffic Ratio & Trials \\")
    print(r"\midrule")
    for r in results:
        print(f"{r['label']} & {r['recall_mean']:.3f} $\\pm$ {r['recall_std']:.3f} & "
              f"{r['stretch_mean']:.3f} $\\pm$ {r['stretch_std']:.3f} & "
              f"{r['tr_mean']:.3f} $\\pm$ {r['tr_std']:.3f} & {r['n_trials']} \\\\")
    print(r"\bottomrule")
    print(r"\end{tabular}")
    print(r"\caption{Multi-broker evaluation: effect of brake limit on recall, stretch, and traffic ratio (3-broker tree, 20 publishers, 50 subscribers, 5 trials per configuration, globally random placement)}")
    print(r"\label{tab:multi-broker}")
    print(r"\end{table}")

if __name__ == '__main__':
    main()