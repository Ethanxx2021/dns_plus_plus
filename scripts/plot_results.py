#!/usr/bin/env python3
"""
DNS++ Phase 1 Benchmark Plotter
Generates recall and stretch charts from bench_broker CSV output.
"""

import csv
import sys
import statistics
import matplotlib
matplotlib.use('Agg')  # non-interactive backend
import matplotlib.pyplot as plt

def parse_csv(filename):
    """Parse bench_broker CSV output.
    Returns list of trials, each trial is a list of subscriber rows."""
    trials = {}
    with open(filename, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            trial = int(row['trial'])
            if trial not in trials:
                trials[trial] = []
            trials[trial].append({
                'recall': int(row['recall']),
                'stretch': float(row['stretch']),
                'num_received': int(row['num_received']),
            })
    return trials

def compute_stats(trials):
    """Compute per-trial recall and stretch, then aggregate."""
    trial_recalls = []
    trial_stretches = []
    
    for trial_num, subs in trials.items():
        recalls = [s['recall'] for s in subs]
        stretches = [s['stretch'] for s in subs if s['stretch'] > 0]
        
        trial_recalls.append(statistics.mean(recalls))
        if stretches:
            trial_stretches.append(statistics.mean(stretches))
    
    return {
        'recall_mean': statistics.mean(trial_recalls),
        'recall_std': statistics.stdev(trial_recalls) if len(trial_recalls) > 1 else 0,
        'stretch_mean': statistics.mean(trial_stretches),
        'stretch_std': statistics.stdev(trial_stretches) if len(trial_stretches) > 1 else 0,
        'n_trials': len(trial_recalls),
    }

def main():
    # brake values and corresponding CSV files
    brake_configs = [
        (1, 'brake_1.csv', 'Brake=1'),
        (2, 'brake_2.csv', 'Brake=2'),
        (4, 'brake_4.csv', 'Brake=4'),
        (1000, 'brake_1000.csv', 'Unbounded'),
    ]
    
    results = []
    for brake_val, csv_file, label in brake_configs:
        try:
            trials = parse_csv(csv_file)
            stats = compute_stats(trials)
            stats['label'] = label
            stats['brake'] = brake_val
            results.append(stats)
            print(f"{label}: recall={stats['recall_mean']:.3f}±{stats['recall_std']:.3f}, "
                  f"stretch={stats['stretch_mean']:.3f}±{stats['stretch_std']:.3f}, "
                  f"trials={stats['n_trials']}")
        except FileNotFoundError:
            print(f"Warning: {csv_file} not found, skipping")
    
    if len(results) < 2:
        print("Error: Need at least 2 configurations to plot")
        sys.exit(1)
    
    # --- Generate plots ---
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
    
    x_labels = [r['label'] for r in results]
    x_pos = range(len(results))
    
    # Plot 1: Recall
    recalls = [r['recall_mean'] for r in results]
    recall_errs = [r['recall_std'] for r in results]
    bars1 = ax1.bar(x_pos, recalls, yerr=recall_errs, capsize=5,
                     color=['#e74c3c', '#e67e22', '#f1c40f', '#2ecc71'],
                     edgecolor='black', linewidth=0.8)
    ax1.set_ylabel('Recall', fontsize=13)
    ax1.set_title('(a) Recall vs Brake Limit', fontsize=13)
    ax1.set_xticks(x_pos)
    ax1.set_xticklabels(x_labels)
    ax1.set_ylim(0, 1.15)
    ax1.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5, label='Perfect (1.0)')
    ax1.legend(fontsize=10)
    ax1.grid(axis='y', alpha=0.3)
    
    # Add value labels on bars
    for bar, val in zip(bars1, recalls):
        ax1.text(bar.get_x() + bar.get_width()/2., bar.get_height() + 0.02,
                 f'{val:.2f}', ha='center', va='bottom', fontsize=11, fontweight='bold')
    
    # Plot 2: Stretch
    stretches = [r['stretch_mean'] for r in results]
    stretch_errs = [r['stretch_std'] for r in results]
    bars2 = ax2.bar(x_pos, stretches, yerr=stretch_errs, capsize=5,
                     color=['#e74c3c', '#e67e22', '#f1c40f', '#2ecc71'],
                     edgecolor='black', linewidth=0.8)
    ax2.set_ylabel('Average Stretch', fontsize=13)
    ax2.set_title('(b) Stretch vs Brake Limit', fontsize=13)
    ax2.set_xticks(x_pos)
    ax2.set_xticklabels(x_labels)
    ax2.set_ylim(0.9, max(s + 0.2 for s in stretches))
    ax2.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5, label='Optimal (1.0)')
    ax2.legend(fontsize=10)
    ax2.grid(axis='y', alpha=0.3)
    
    # Add value labels on bars
    for bar, val in zip(bars2, stretches):
        ax2.text(bar.get_x() + bar.get_width()/2., bar.get_height() + 0.01,
                 f'{val:.2f}', ha='center', va='bottom', fontsize=11, fontweight='bold')
    
    plt.tight_layout()
    output_file = 'phase1_brake_sweep.png'
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"\nChart saved to: {output_file}")
    
    # --- Also print LaTeX table ---
    print("\n--- LaTeX Table ---")
    print(r"\begin{table}[h]")
    print(r"\centering")
    print(r"\begin{tabular}{lccc}")
    print(r"\toprule")
    print(r"Brake Limit & Recall (mean $\pm$ std) & Stretch (mean $\pm$ std) & Trials \\")
    print(r"\midrule")
    for r in results:
        print(f"{r['label']} & {r['recall_mean']:.3f} $\\pm$ {r['recall_std']:.3f} & "
              f"{r['stretch_mean']:.3f} $\\pm$ {r['stretch_std']:.3f} & {r['n_trials']} \\\\")
    print(r"\bottomrule")
    print(r"\end{tabular}")
    print(r"\caption{Effect of propagation brake on recall and stretch (10 publishers, 50 subscribers, 5 trials per configuration)}")
    print(r"\label{tab:brake-sweep}")
    print(r"\end{table}")

if __name__ == '__main__':
    main()