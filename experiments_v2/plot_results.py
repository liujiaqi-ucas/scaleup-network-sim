#!/usr/bin/env python3
"""
GBN vs SR 对比图 v2 (pacing修复后)
生成:
  1. 综合对比图 (P99 FCT + JCT)，每种流量一张
  2. Speedup ratio 图 (SR相对GBN的加速比)
  3. 文字汇总表
"""
import csv, os, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SR_CSV  = os.path.join(SCRIPT_DIR, "sr", "results.csv")
GBN_CSV = os.path.join(SCRIPT_DIR, "gbn", "results.csv")
OUT_DIR = os.path.join(SCRIPT_DIR, "plots")
os.makedirs(OUT_DIR, exist_ok=True)

ERR_RATES  = [0.00001, 0.00005, 0.0001, 0.0005, 0.001]
MSG_SIZES  = ["1mb", "4mb", "16mb", "64mb"]
SIZE_LABELS = ["1 MB", "4 MB", "16 MB", "64 MB"]
TRAFFICS   = ["allreduce", "alltoall"]
TRAFFIC_LABELS = {"allreduce": "Allreduce", "alltoall": "Alltoall"}

def load_csv(path):
    data = {}
    if not os.path.exists(path):
        print(f"WARNING: {path} not found")
        return data
    with open(path) as f:
        for line in f:
            parts = line.strip().split(',')
            if len(parts) < 9: continue
            try:
                key = (parts[1], parts[2], parts[3])
                data[key] = {
                    'mean': float(parts[6]),
                    'p99':  float(parts[7]),
                    'jct':  float(parts[8]),
                    'flows': int(parts[4]),
                    'errors': int(parts[5]),
                }
            except: pass
    return data

def get_vals(data, traffic, msgsize, metric):
    vals = []
    for er in ERR_RATES:
        key = (traffic, msgsize, str(er))
        vals.append(data.get(key, {}).get(metric, np.nan))
    return vals

# =========================================================
# 图1/2: 综合对比图 (每种流量一张, 2行×4列)
# 上排: P99 FCT, 下排: Mean FCT
# =========================================================
def plot_combined(traffic, sr, gbn):
    fig, axes = plt.subplots(2, 4, figsize=(18, 8))
    fig.suptitle(f"{TRAFFIC_LABELS[traffic]} — GBN vs SR  (H100 8-GPU, CBFC, pacing fixed)",
                 fontsize=14, fontweight='bold')

    metrics = [('p99', 'P99 FCT (μs)'), ('mean', 'Mean FCT (μs)')]
    for row, (metric, ylabel) in enumerate(metrics):
        for col, (ms, slabel) in enumerate(zip(MSG_SIZES, SIZE_LABELS)):
            ax = axes[row][col]
            gbn_v = get_vals(gbn, traffic, ms, metric)
            sr_v  = get_vals(sr,  traffic, ms, metric)

            ax.plot(ERR_RATES, gbn_v, 's-', color='#2196F3', linewidth=2, markersize=6, label='GBN')
            ax.plot(ERR_RATES, sr_v,  'o-', color='#FF5722', linewidth=2, markersize=6, label='SR')

            ax.set_xscale('log')
            if row == 1:
                ax.set_xlabel('Link Error Rate', fontsize=10)
                ax.set_xticks(ERR_RATES)
                ax.set_xticklabels([f'{e:.0e}' for e in ERR_RATES], fontsize=7, rotation=30)
            else:
                ax.set_xticks(ERR_RATES)
                ax.set_xticklabels([])
                ax.set_title(slabel, fontsize=12, fontweight='bold')
            if col == 0:
                ax.set_ylabel(ylabel, fontsize=10)
            ax.legend(fontsize=8, loc='upper left')
            ax.grid(True, alpha=0.3)
            ax.set_ylim(bottom=0)

    plt.tight_layout()
    path = os.path.join(OUT_DIR, f"compare_{traffic}.png")
    plt.savefig(path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  -> {path}")

# =========================================================
# 图3: Speedup ratio (SR/GBN, <1 means SR faster)
# 一张图, 2行(allreduce/alltoall) × 2列(mean/p99)
# =========================================================
def plot_speedup(sr, gbn):
    fig, axes = plt.subplots(2, 2, figsize=(12, 8))
    fig.suptitle("SR vs GBN Speedup Ratio  (ratio < 1 → SR faster)",
                 fontsize=14, fontweight='bold')

    colors = ['#1976D2', '#388E3C', '#F57C00', '#D32F2F']
    for row, traffic in enumerate(TRAFFICS):
        for col, (metric, mlabel) in enumerate([('mean', 'Mean FCT'), ('p99', 'P99 FCT')]):
            ax = axes[row][col]
            for i, (ms, slabel) in enumerate(zip(MSG_SIZES, SIZE_LABELS)):
                gbn_v = get_vals(gbn, traffic, ms, metric)
                sr_v  = get_vals(sr,  traffic, ms, metric)
                ratio = [s/g if g > 0 else np.nan for s, g in zip(sr_v, gbn_v)]
                ax.plot(ERR_RATES, ratio, 'o-', color=colors[i], linewidth=2,
                        markersize=6, label=slabel)

            ax.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5, linewidth=1)
            ax.set_xscale('log')
            ax.set_xlabel('Link Error Rate', fontsize=10)
            ax.set_xticks(ERR_RATES)
            ax.set_xticklabels([f'{e:.0e}' for e in ERR_RATES], fontsize=7, rotation=30)
            if col == 0:
                ax.set_ylabel(f'SR / GBN ratio ({mlabel})', fontsize=10)
            ax.set_title(f"{TRAFFIC_LABELS[traffic]} — {mlabel}", fontsize=11, fontweight='bold')
            ax.legend(fontsize=8)
            ax.grid(True, alpha=0.3)
            ax.set_ylim(0.85, 1.10)

    plt.tight_layout()
    path = os.path.join(OUT_DIR, "speedup_ratio.png")
    plt.savefig(path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  -> {path}")

# =========================================================
# 文字汇总表
# =========================================================
def print_summary(sr, gbn):
    print("\n" + "="*95)
    print("GBN vs SR 实验数据汇总 (pacing修复后)")
    print("="*95)
    for traffic in TRAFFICS:
        print(f"\n--- {traffic.upper()} ---")
        print(f"{'Size':<6} {'ErrRate':<10} {'GBN mean':>10} {'SR mean':>10} {'Δ%':>7}  |  {'GBN p99':>10} {'SR p99':>10} {'Δ%':>7}")
        print("-"*90)
        for ms in MSG_SIZES:
            for er in ERR_RATES:
                key = (traffic, ms, str(er))
                g = gbn.get(key, {})
                s = sr.get(key, {})
                gm = g.get('mean', 0); sm = s.get('mean', 0)
                gp = g.get('p99', 0);  sp = s.get('p99', 0)
                dm = ((sm - gm) / gm * 100) if gm > 0 else 0
                dp = ((sp - gp) / gp * 100) if gp > 0 else 0
                print(f"{ms:<6} {er:<10} {gm:>10.1f} {sm:>10.1f} {dm:>+6.1f}%  |  {gp:>10.1f} {sp:>10.1f} {dp:>+6.1f}%")
    print("="*95)
    print("Δ% = (SR - GBN) / GBN × 100  → 负数表示 SR 更快\n")

# === Main ===
if __name__ == '__main__':
    print("加载数据...")
    sr_data  = load_csv(SR_CSV)
    gbn_data = load_csv(GBN_CSV)
    print(f"SR:  {len(sr_data)} 条  GBN: {len(gbn_data)} 条")

    if not sr_data and not gbn_data:
        print("ERROR: 无数据"); sys.exit(1)

    print("\n生成图表...")
    for t in TRAFFICS:
        plot_combined(t, sr_data, gbn_data)
    plot_speedup(sr_data, gbn_data)
    print_summary(sr_data, gbn_data)
    print("全部完成!")
