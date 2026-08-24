#!/usr/bin/env python3
"""
Generate Appendix tables for the DNS++ paper.
Outputs LaTeX code for:
- Per-trial summary statistics
- Traffic breakdown (from multi-broker logs)
"""

import csv
import os
import statistics
import re

def generate_per_trial_table():
    """Generate per-trial latency table for Phase 3."""
    print("% --- Appendix: Per-Trial Statistics ---")
    print(r"\begin{table}[H]")
    print(r"\centering")
    print(r"\caption{Per-trial broker-side routing latency (N=50 subscribers per trial). Endpoint blinding amortized.}")
    print(r"\label{tab:per-trial}")
    print(r"\begin{tabular}{llccc}")
    print(r"\toprule")
    print(r"Mode & Trial & Latency Mean (ms) & Stretch Mean & Recall \\")
    print(r"\midrule")

    for mode, filename in [("Plaintext", "plain.csv"), ("Encrypted", "encrypted.csv")]:
        if not os.path.exists(filename):
            continue
        trials = {}
        with open(filename, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                t = int(row['trial'])
                if t not in trials:
                    trials[t] = []
                trials[t].append(row)

        for t in sorted(trials.keys()):
            subs = trials[t]
            lats = [float(s['latency_ms']) for s in subs if float(s['latency_ms']) > 0]
            stretches = [float(s['stretch']) for s in subs if float(s['stretch']) > 0]
            recalls = [int(s['recall']) for s in subs]

            mean_lat = statistics.mean(lats) if lats else 0
            mean_str = statistics.mean(stretches) if stretches else 0
            mean_rec = statistics.mean(recalls) if recalls else 0

            print(f"{mode} & {t} & {mean_lat:.2f} & {mean_str:.3f} & {mean_rec:.3f} \\\\")

    print(r"\bottomrule")
    print(r"\end{tabular}")
    print(r"\end{table}")
    print()


def generate_traffic_breakdown():
    """Generate traffic breakdown table from multi-broker logs."""
    print("% --- Appendix: Traffic Breakdown ---")
    print(r"\begin{table}[H]")
    print(r"\centering")
    print(r"\caption{Traffic breakdown for multi-broker evaluation (3-broker tree, 20 publishers, 50 subscribers, averaged over 5 trials).}")
    print(r"\label{tab:traffic-breakdown}")
    print(r"\begin{tabular}{lcccc}")
    print(r"\toprule")
    print(r"Brake Limit & Upward & Downward & Local Delivery & Braked \\")
    print(r"\midrule")

    # Parse multi_broker logs for stats
    configs = [
        ("1", "multi_brake1.log"),
        ("2", "multi_brake2.log"),
        ("4", "multi_brake4.log"),
        ("$\\infty$", "multi_brake_inf.log"),
    ]

    for brake_label, logfile in configs:
        if not os.path.exists(logfile):
            continue

        # Try to parse stats from log
        # The bench_multi_broker outputs STATS lines or we parse stderr
        up_vals = []
        down_vals = []
        local_vals = []
        braked_vals = []

        with open(logfile, 'r') as f:
            for line in f:
                # Look for stats patterns in the log
                # Format varies, try common patterns
                m = re.search(r'up=(\d+)', line)
                if m: up_vals.append(int(m.group(1)))
                m = re.search(r'down=(\d+)', line)
                if m: down_vals.append(int(m.group(1)))
                m = re.search(r'local=(\d+)', line)
                if m: local_vals.append(int(m.group(1)))
                m = re.search(r'braked=(\d+)', line)
                if m: braked_vals.append(int(m.group(1)))

        if up_vals:
            up = statistics.mean(up_vals)
            down = statistics.mean(down_vals) if down_vals else 0
            local = statistics.mean(local_vals) if local_vals else 0
            braked = statistics.mean(braked_vals) if braked_vals else 0
            print(f"{brake_label} & {up:.1f} & {down:.1f} & {local:.1f} & {braked:.1f} \\\\")
        else:
            # If no stats in log, output placeholder
            print(f"{brake_label} & --- & --- & --- & --- \\\\")

    print(r"\bottomrule")
    print(r"\end{tabular}")
    print(r"\end{table}")


def generate_crypto_microbench_table(blind_notif_ms, blind_sub_ms, match_ms):
    """Generate crypto micro-benchmark table."""
    print()
    print("% --- Appendix: Crypto Micro-Benchmark ---")
    print(r"\begin{table}[H]")
    print(r"\centering")
    print(r"\caption{Micro-benchmark of cryptographic operations (2048-bit Paillier, 1000 iterations, commodity CPU).}")
    print(r"\label{tab:microbench}")
    print(r"\begin{tabular}{lcc}")
    print(r"\toprule")
    print(r"Operation & Executed By & Mean Time (ms/op) \\")
    print(r"\midrule")
    print(f"blindNotification & Publisher & {blind_notif_ms:.2f} \\\\")
    print(f"blindSubscription & Subscriber & {blind_sub_ms:.2f} \\\\")
    print(f"executeMatch & Broker & {match_ms:.4f} \\\\")
    print(r"\bottomrule")
    print(r"\end{tabular}")
    print(r"\end{table}")


if __name__ == '__main__':
    generate_per_trial_table()
    generate_traffic_breakdown()

    # Crypto micro-benchmark values - replace with actual output from test_crypto_microbench
    # Run: ./test_crypto_microbench
    # Then paste the values here
    print()
    print("% Run ./test_crypto_microbench and paste the output values below:")
    print("% Example output: blindNotification: 25.12 ms/op")
    print("% Then uncomment the line below with actual values:")
    # generate_crypto_microbench_table(25.12, 25.34, 0.08)