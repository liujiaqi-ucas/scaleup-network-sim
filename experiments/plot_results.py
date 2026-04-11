#!/usr/bin/env python3
"""GBN vs SR 对比图：p99 FCT 和 JCT"""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import glob, os
import pandas as pd

EXP_DIR = os.path.dirname(os.path.abspath(__file__))

ERROR_RATES = [0.00001, 0.00005, 0.0001, 0.0005, 0.001]
MSG_SIZES   = ['1mb', '4mb', '16mb', '64mb']
SIZE_LABELS = ['1MB', '4MB', '16MB', '64MB']
TRAFFICS    = ['alltoall', 'allreduce']
METRICS     = {'p99_fct_us': 'P99 FCT (μs)', 'jct_us': 'JCT (μs)'}

COLS = ['protocol','traffic','msgsize','errrate','flows','error_pkts',
        'avg_fct_us','p99_fct_us','jct_us']

def load_all():
    rows = []
    for proto in ['sr','gbn']:
        for f in glob.glob(f"{EXP_DIR}/{proto}/*.csv"):
            for line in open(f):
                line = line.strip()
                if not line: continue
                parts = line.split(',')
                if len(parts) == 9:
                    try:
                        rows.append({
                            'protocol': parts[0], 'traffic': parts[1],
                            'msgsize':  parts[2], 'errrate': float(parts[3]),
                            'flows':    int(parts[4]), 'error_pkts': int(parts[5]),
                            'avg_fct_us': float(parts[6]),
                            'p99_fct_us': float(parts[7]),
                            'jct_us':     float(parts[8]),
                        })
                    except: pass
    return pd.DataFrame(rows)

def plot_metric(df, traffic, metric, ax_title_suffix=''):
    fig, axes = plt.subplots(1, 4, figsize=(18, 4), sharey=False)
    fig.suptitle(f'{traffic.upper()} — {METRICS[metric]}{ax_title_suffix}',
                 fontsize=13, fontweight='bold')
    colors = {'sr': '#FF5722', 'gbn': '#2196F3'}
    labels = {'sr': 'SR (Selective Repeat)', 'gbn': 'GBN (Go-Back-N)'}
    markers = {'sr': 'o', 'gbn': 's'}

    sub = df[df['traffic'] == traffic]
    for i, (size, slabel) in enumerate(zip(MSG_SIZES, SIZE_LABELS)):
        ax = axes[i]
        for proto in ['sr', 'gbn']:
            d = sub[(sub['protocol']==proto) & (sub['msgsize']==size)]
            d = d.sort_values('errrate')
            if d.empty: continue
            ax.plot(d['errrate'], d[metric],
                    color=colors[proto], marker=markers[proto],
                    linewidth=2, markersize=7, label=labels[proto])
        ax.set_title(slabel, fontsize=11)
        ax.set_xscale('log')
        ax.set_xlabel('Error Rate', fontsize=10)
        ax.set_ylabel(METRICS[metric] if i==0 else '', fontsize=10)
        ax.grid(True, alpha=0.3)
        ax.tick_params(axis='x', labelsize=8)
        if i==0: ax.legend(fontsize=8)

    plt.tight_layout()
    fname = f"{EXP_DIR}/plots/{traffic}_{metric}.png"
    plt.savefig(fname, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {fname}")

def print_summary(df):
    print("\n===== Data Summary =====")
    print(f"Total rows: {len(df)}")
    for proto in ['sr','gbn']:
        for traffic in TRAFFICS:
            d = df[(df['protocol']==proto)&(df['traffic']==traffic)]
            n = len(d)
            print(f"  {proto}/{traffic}: {n}/20 experiments done")
    print()

if __name__ == '__main__':
    df = load_all()
    if df.empty:
        print("No data yet."); exit(0)
    print_summary(df)
    os.makedirs(f"{EXP_DIR}/plots", exist_ok=True)
    for traffic in TRAFFICS:
        for metric in METRICS:
            plot_metric(df, traffic, metric)
    print("All plots generated.")
