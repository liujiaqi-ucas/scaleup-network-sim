#!/usr/bin/env python3
"""
NVL72 alltoall 16mb: SR vs GBN 对比图
三个指标 (Mean FCT / P99 FCT / JCT) vs BER
"""
import csv, os, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SR_CSV  = os.path.join(SCRIPT_DIR, "sr",  "results.csv")
GBN_CSV = os.path.join(SCRIPT_DIR, "gbn", "results.csv")
OUT_DIR = os.path.join(SCRIPT_DIR, "plots")
os.makedirs(OUT_DIR, exist_ok=True)

# 10 个 BER 值（从小到大）
BER_VALS = [
    1e-15, 1e-14, 1e-13, 1e-12, 1e-11,
    1e-10, 1e-9,  1e-8,  1e-7,  1e-6,
]

def load_csv(path):
    data = {}
    if not os.path.exists(path):
        print(f"WARNING: {path} not found")
        return data
    with open(path) as f:
        for line in f:
            parts = line.strip().split(',')
            if len(parts) < 9:
                continue
            try:
                er = float(parts[3])
                data[er] = {
                    'mean':   float(parts[6]),
                    'p99':    float(parts[7]),
                    'jct':    float(parts[8]),
                    'flows':  int(parts[4]),
                    'errors': int(parts[5]),
                }
            except Exception:
                pass
    return data

def get_vals(data, metric):
    return [data.get(er, {}).get(metric, np.nan) for er in BER_VALS]

def plot_comparison(sr, gbn):
    metrics = [
        ('mean', 'Mean FCT (μs)'),
        ('p99',  'P99 FCT (μs)'),
        ('jct',  'JCT (μs)'),
    ]

    fig, axes = plt.subplots(1, 3, figsize=(16, 5))
    fig.suptitle("NVL72 Alltoall 16MB — SR vs GBN  (BER 错误率)",
                 fontsize=14, fontweight='bold')

    for ax, (metric, ylabel) in zip(axes, metrics):
        sr_v  = get_vals(sr,  metric)
        gbn_v = get_vals(gbn, metric)

        ax.plot(BER_VALS, sr_v,  'o-', color='#FF5722', lw=2, ms=7, label='SR (CBFC)')
        ax.plot(BER_VALS, gbn_v, 's-', color='#2196F3', lw=2, ms=7, label='GBN (CBFC)')

        # 紧凑 Y 范围
        all_v = [v for v in sr_v + gbn_v if not np.isnan(v) and v > 0]
        if all_v:
            ax.set_ylim(min(all_v) * 0.90, max(all_v) * 1.10)

        ax.set_xscale('log')
        ax.set_xlabel('Bit Error Rate (BER)', fontsize=11)
        ax.set_ylabel(ylabel, fontsize=11)
        ax.set_title(ylabel.split(' ')[0] + ' ' + ylabel.split(' ')[1], fontsize=12, fontweight='bold')
        ax.set_xticks(BER_VALS)
        ax.set_xticklabels([f'{v:.0e}' for v in BER_VALS], fontsize=7, rotation=40, ha='right')
        ax.legend(fontsize=9)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    path = os.path.join(OUT_DIR, "nvl72_alltoall_16mb_compare.png")
    plt.savefig(path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  -> {path}")

def plot_ratio(sr, gbn):
    """SR/GBN 加速比图"""
    fig, axes = plt.subplots(1, 3, figsize=(16, 5))
    fig.suptitle("SR / GBN Ratio — NVL72 Alltoall 16MB  (< 1 表示 SR 更快)",
                 fontsize=13, fontweight='bold')

    metrics = [('mean','Mean FCT'), ('p99','P99 FCT'), ('jct','JCT')]
    for ax, (metric, title) in zip(axes, metrics):
        sr_v  = get_vals(sr,  metric)
        gbn_v = get_vals(gbn, metric)
        ratio = [s/g if g > 0 and not np.isnan(s) and not np.isnan(g)
                 else np.nan for s, g in zip(sr_v, gbn_v)]

        ax.plot(BER_VALS, ratio, 'D-', color='#388E3C', lw=2, ms=7)
        ax.axhline(y=1.0, color='gray', ls='--', lw=1.2, alpha=0.7)
        ax.set_xscale('log')
        ax.set_xlabel('Bit Error Rate (BER)', fontsize=11)
        ax.set_ylabel('SR / GBN ratio', fontsize=11)
        ax.set_title(title, fontsize=12, fontweight='bold')
        ax.set_xticks(BER_VALS)
        ax.set_xticklabels([f'{v:.0e}' for v in BER_VALS], fontsize=7, rotation=40, ha='right')
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    path = os.path.join(OUT_DIR, "nvl72_alltoall_16mb_ratio.png")
    plt.savefig(path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  -> {path}")

def print_summary(sr, gbn):
    print("\n" + "="*85)
    print("NVL72 Alltoall 16MB: SR vs GBN 汇总")
    print("="*85)
    print(f"{'BER':<12} {'SR mean':>10} {'GBN mean':>10} {'Δ%':>7}  "
          f"{'SR p99':>10} {'GBN p99':>10} {'Δ%':>7}  "
          f"{'SR jct':>10} {'GBN jct':>10} {'Δ%':>7}")
    print("-"*85)
    for er in BER_VALS:
        s = sr.get(er,  {}); g = gbn.get(er, {})
        sm, gm = s.get('mean',0), g.get('mean',0)
        sp, gp = s.get('p99',0),  g.get('p99',0)
        sj, gj = s.get('jct',0),  g.get('jct',0)
        dm = (sm-gm)/gm*100 if gm > 0 else 0
        dp = (sp-gp)/gp*100 if gp > 0 else 0
        dj = (sj-gj)/gj*100 if gj > 0 else 0
        print(f"{er:<12.2e} {sm:>10.1f} {gm:>10.1f} {dm:>+6.1f}%  "
              f"{sp:>10.1f} {gp:>10.1f} {dp:>+6.1f}%  "
              f"{sj:>10.1f} {gj:>10.1f} {dj:>+6.1f}%")
    print("="*85)
    print("Δ% = (SR - GBN) / GBN × 100  → 负数表示 SR 更快\n")

if __name__ == '__main__':
    print("加载数据...")
    sr  = load_csv(SR_CSV)
    gbn = load_csv(GBN_CSV)
    print(f"SR: {len(sr)} 条  GBN: {len(gbn)} 条")

    if not sr and not gbn:
        print("ERROR: 无数据"); sys.exit(1)

    print("\n生成图表...")
    plot_comparison(sr, gbn)
    plot_ratio(sr, gbn)
    print_summary(sr, gbn)
    print("完成!")
