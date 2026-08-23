#!/usr/bin/env python3
"""
DNS++ Phase 3 Crypto Benchmark Plotter
Generates plaintext vs encrypted comparison chart.
"""

import csv
import sys
import statistics
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

def parse_csv(filename):
    """Parse bench_broker CSV output."""
    latencies = []
    recalls = []
    stretches = []
    
    with open(filename, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            lat = float(row['latency_ms'])
            if lat >= 0:
                latencies.append(lat)
            recalls.append(int(row['recall']))
            s = float(row['stretch'])
            if s > 0:
                stretches.append(s)
    
    return {
        'latencies': latencies,
        'recalls': recalls,
        'stretches': stretches,
    }

def main():
    configs = [
        ('plain.csv', 'Plaintext', '#2ecc71'),
        ('encrypted.csv', 'Encrypted (Paillier)', '#e74c3c'),
    ]
    
    results = []
    for csv_file, label, color in configs:
        try:
            data = parse_csv(csv_file)
            data['label'] = label
            data['color'] = color
            results.append(data)
            
            lat = data['latencies']
            print(f"{label}:")
            print(f"  Latency: mean={statistics.mean(lat):.2f}ms, "
                  f"median={statistics.median(lat):.2f}ms, "
                  f"p95={sorted(lat)[int(len(lat)*0.95)]:.2f}ms, "
                  f"std={statistics.stdev(lat):.2f}ms")
            print(f"  Recall: {statistics.mean(data['recalls']):.3f}")
            print(f"  Stretch: {statistics.mean(data['stretches']):.3f}")
            print(f"  N={len(lat)}")
        except FileNotFoundError:
            print(f"Warning: {csv_file} not found, skipping")
    
    if len(results) < 2:
        print("Error: Need both plain and encrypted results")
        sys.exit(1)
    
    # --- Generate 2-panel figure ---
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
    
    # Plot 1: Latency Comparison (Bar chart with mean + std)
    labels = [r['label'] for r in results]
    x_pos = np.arange(len(results))
    colors = [r['color'] for r in results]
    
    means = [statistics.mean(r['latencies']) for r in results]
    stds = [statistics.stdev(r['latencies']) for r in results]
    
    bars1 = ax1.bar(x_pos, means, yerr=stds, capsize=8,
                     color=colors, edgecolor='black', linewidth=0.8, width=0.5)
    ax1.set_ylabel('Latency (ms)', fontsize=13)
    ax1.set_title('(a) Mean Latency Comparison', fontsize=13)
    ax1.set_xticks(x_pos)
    ax1.set_xticklabels(labels, fontsize=11)
    ax1.set_ylim(0, max(m + s + 50 for m, s in zip(means, stds)))
    ax1.grid(axis='y', alpha=0.3)
    for bar, val in zip(bars1, means):
        ax1.text(bar.get_x() + bar.get_width()/2., bar.get_height() + 10,
                 f'{val:.1f}ms', ha='center', va='bottom', fontsize=12, fontweight='bold')
    
    # Plot 2: Latency Distribution (Histogram overlay)
    for r in results:
        ax2.hist(r['latencies'], bins=30, alpha=0.6, 
                 color=r['color'], label=r['label'], edgecolor='black', linewidth=0.5)
    
    ax2.set_xlabel('Latency (ms)', fontsize=13)
    ax2.set_ylabel('Frequency', fontsize=13)
    ax2.set_title('(b) Latency Distribution', fontsize=13)
    ax2.legend(fontsize=11)
    ax2.grid(axis='y', alpha=0.3)
    
    plt.tight_layout()
    output_file = 'phase3_crypto_comparison.png'
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"\nChart saved to: {output_file}")
    
    # --- LaTeX Table ---
    print("\n--- LaTeX Table ---")
    print(r"\begin{table}[htbp]")
    print(r"\centering")
    print(r"\begin{tabular}{lcccc}")
    print(r"\toprule")
    print(r"Mode & Recall & Stretch & Latency (mean $\pm$ std) & p95 Latency \\")
    print(r"\midrule")
    for r in results:
        lat = r['latencies']
        mean_lat = statistics.mean(lat)
        std_lat = statistics.stdev(lat)
        p95_lat = sorted(lat)[int(len(lat)*0.95)]
        recall = statistics.mean(r['recalls'])
        stretch = statistics.mean(r['stretches'])
        print(f"{r['label']} & {recall:.3f} & {stretch:.3f} & "
              f"{mean_lat:.2f} $\\pm$ {std_lat:.2f} ms & {p95_lat:.2f} ms \\\\")
    print(r"\bottomrule")
    print(r"\end{tabular}")
    print(r"\caption{Plaintext vs Encrypted matching performance (10 publishers, 50 subscribers, brake=4, 5 trials, seed=42). Latency measured from first PUBLISH sent to subscriber receipt.}")
    print(r"\label{tab:crypto-comparison}")
    print(r"\end{table}")

if __name__ == '__main__':
    main()