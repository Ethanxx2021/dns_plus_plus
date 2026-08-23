#!/usr/bin/env python3
"""
DNS++ Scalability Sweep Plotter
Generates recall and stretch charts vs subscriber count.
"""

import csv
import sys
import statistics
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

def parse_log(filename):
    """Parse bench_broker stderr log for per-trial summary."""
    trials = []
    with open(filename, 'r') as f:
        for line in f:
            if line.startswith('Trial'):
                parts = line.strip().split()
                recall_str = next((p for p in parts if p.startswith('recall=')), None)
                stretch_str = next((p for p in parts if p.startswith('avg_stretch=')), None)
                
                if recall_str and stretch_str:
                    recall = float(recall_str.split('=')[1])
                    stretch = float(stretch_str.split('=')[1])
                    trials.append({
                        'recall': recall,
                        'stretch': stretch,
                    })
    return trials

def compute_stats(trials):
    return {
        'recall_mean': statistics.mean([t['recall'] for t in trials]),
        'recall_std': statistics.stdev([t['recall'] for t in trials]) if len(trials) > 1 else 0,
        'stretch_mean': statistics.mean([t['stretch'] for t in trials]),
        'stretch_std': statistics.stdev([t['stretch'] for t in trials]) if len(trials) > 1 else 0,
        'n_trials': len(trials),
    }

def main():
    configs = [
        (10, 'sweep_10.log', '10'),
        (50, 'sweep_50.log', '50'),
        (200, 'sweep_200.log', '200'),
        (500, 'sweep_500.log', '500'),
        (1000, 'sweep_1000.log', '1000'),
    ]
    
    results = []
    for subs, log_file, label in configs:
        try:
            trials = parse_log(log_file)
            stats = compute_stats(trials)
            stats['label'] = label
            stats['subs'] = subs
            results.append(stats)
            print(f"{label} subs: recall={stats['recall_mean']:.3f}±{stats['recall_std']:.3f}, "
                  f"stretch={stats['stretch_mean']:.3f}±{stats['stretch_std']:.3f}, "
                  f"trials={stats['n_trials']}")
        except FileNotFoundError:
            print(f"Warning: {log_file} not found, skipping")
    
    if len(results) < 2:
        print("Error: Need at least 2 configurations")
        sys.exit(1)
    
    # --- Generate 2-panel figure ---
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
    
    x_labels = [r['label'] for r in results]
    x_pos = np.arange(len(results))
    colors = ['#2ecc71'] * len(results)
    
    # Plot 1: Recall
    recalls = [r['recall_mean'] for r in results]
    recall_errs = [r['recall_std'] for r in results]
    bars1 = ax1.bar(x_pos, recalls, yerr=recall_errs, capsize=5,
                     color=colors, edgecolor='black', linewidth=0.8)
    ax1.set_ylabel('Recall', fontsize=13)
    ax1.set_title('(a) Recall vs Subscriber Count', fontsize=13)
    ax1.set_xticks(x_pos)
    ax1.set_xticklabels(x_labels)
    ax1.set_ylim(0.9, 1.05)
    ax1.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5)
    ax1.grid(axis='y', alpha=0.3)
    for bar, val in zip(bars1, recalls):
        ax1.text(bar.get_x() + bar.get_width()/2., bar.get_height() + 0.01,
                 f'{val:.3f}', ha='center', va='bottom', fontsize=11, fontweight='bold')
    
    # Plot 2: Stretch
    stretches = [r['stretch_mean'] for r in results]
    stretch_errs = [r['stretch_std'] for r in results]
    bars2 = ax2.bar(x_pos, stretches, yerr=stretch_errs, capsize=5,
                     color=colors, edgecolor='black', linewidth=0.8)
    ax2.set_ylabel('Average Stretch', fontsize=13)
    ax2.set_title('(b) Stretch vs Subscriber Count', fontsize=13)
    ax2.set_xticks(x_pos)
    ax2.set_xticklabels(x_labels)
    ax2.set_ylim(0.98, 1.02)
    ax2.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5)
    ax2.grid(axis='y', alpha=0.3)
    for bar, val in zip(bars2, stretches):
        ax2.text(bar.get_x() + bar.get_width()/2., bar.get_height() + 0.001,
                 f'{val:.3f}', ha='center', va='bottom', fontsize=11, fontweight='bold')
    
    plt.tight_layout()
    output_file = 'phase2_scalability.png'
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"\nChart saved to: {output_file}")

if __name__ == '__main__':
    main()