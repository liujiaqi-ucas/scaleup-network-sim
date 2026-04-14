#!/usr/bin/env python3
"""
NVL72 GBN vs SR 对比图
用法: python3 plot_nvl72_results.py
需要: experiments_nvl72/sr/results.csv 和 experiments_nvl72/gbn/results.csv
"""
import os, sys, csv
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

DIR = "experiments_nvl72"
SR_CSV  = f"{DIR}/sr/results.csv"
GBN_CSV = f"{DIR}/gbn/results.csv"
OUT_DIR = f"{DIR}/plots"
os.makedirs(OUT_DIR, exist_ok=True)

ERR_RATES  = [0.00001, 0.00005, 0.0001, 0.0005, 0.001]
MSG_SIZES  = ["1mb", "4mb", "16mb", "64mb"]
SIZE_LABELS = ["1 MB", "4 MB", "16 MB", "64 MB"]

def load_csv(path):
    data = {}
    if not os.path.exists(path):
        print(f"WARNING: {path} not found"); return data
    for line in open(path):
        parts = line.strip().split(',')
        if len(parts) < 9: continue
        try:
            key = (parts[2], float(parts[3]))  # (msgsize, errrate)
            data[key] = {'mean': float(parts[6]), 'p99': float(parts[7]), 'jct': float(parts[8]),
                         'flows': int(parts[4]), 'errors': int(parts[5])}
        except: pass
    return data

def get_vals(data, msgsize, metric):
    return [data.get((msgsize, er), {}).get(metric, np.nan) for er in ERR_RATES]

sr  = load_csv(SR_CSV)
gbn = load_csv(GBN_CSV)
print(f"SR: {len(sr)} 条, GBN: {len(gbn)} 条")

if not sr or not gbn:
    print("ERROR: 数据不完整"); sys.exit(1)

# =========================================================
# 图1: 综合对比 3行(metric) × 4列(msgsize)
# =========================================================
fig, axes = plt.subplots(3, 4, figsize=(18, 11))
fig.suptitle("NVL72 Alltoall — GBN vs SR  (72-GPU, CBFC)", fontsize=14, fontweight='bold')

metrics = [('p99', 'P99 FCT (μs)'), ('mean', 'Mean FCT (μs)'), ('jct', 'JCT (μs)')]
for row, (m, ylabel) in enumerate(metrics):
    for col, (ms, slabel) in enumerate(zip(MSG_SIZES, SIZE_LABELS)):
        ax = axes[row][col]
        gbn_v = get_vals(gbn, ms, m)
        sr_v  = get_vals(sr,  ms, m)
        ax.plot(ERR_RATES, gbn_v, 's-', color='#2196F3', lw=2, ms=6, label='GBN')
        ax.plot(ERR_RATES, sr_v,  'o-', color='#FF5722', lw=2, ms=6, label='SR')
        allv = [v for v in gbn_v + sr_v if not np.isnan(v) and v > 0]
        if allv: ax.set_ylim(min(allv)*0.90, max(allv)*1.10)
        ax.set_xscale('log'); ax.set_xticks(ERR_RATES)
        if row == 2:
            ax.set_xticklabels([f'{e:.0e}' for e in ERR_RATES], fontsize=7, rotation=30)
            ax.set_xlabel('Link Error Rate')
        else: ax.set_xticklabels([])
        if row == 0: ax.set_title(slabel, fontsize=12, fontweight='bold')
        if col == 0: ax.set_ylabel(ylabel, fontsize=10)
        ax.legend(fontsize=8, loc='upper left'); ax.grid(True, alpha=0.3)
plt.tight_layout()
path = f"{OUT_DIR}/nvl72_compare.png"
plt.savefig(path, dpi=150, bbox_inches='tight'); plt.close()
print(f"  -> {path}")

# =========================================================
# 图2: Speedup ratio 1×3
# =========================================================
fig, axes = plt.subplots(1, 3, figsize=(16, 4.5))
fig.suptitle("NVL72 — SR/GBN Ratio  (< 1 → SR faster)", fontsize=13, fontweight='bold')
colors = ['#1976D2', '#388E3C', '#F57C00', '#D32F2F']
for col, (m, ml) in enumerate(metrics):
    ax = axes[col]
    for i, (ms, sl) in enumerate(zip(MSG_SIZES, SIZE_LABELS)):
        gbn_v = get_vals(gbn, ms, m)
        sr_v  = get_vals(sr,  ms, m)
        ratio = [s/g if g > 0 else np.nan for s, g in zip(sr_v, gbn_v)]
        ax.plot(ERR_RATES, ratio, 'o-', color=colors[i], lw=2, ms=6, label=sl)
    ax.axhline(1.0, color='gray', ls='--', alpha=0.5)
    ax.set_xscale('log'); ax.set_xlabel('Link Error Rate')
    ax.set_xticks(ERR_RATES)
    ax.set_xticklabels([f'{e:.0e}' for e in ERR_RATES], fontsize=7, rotation=30)
    ax.set_title(ml, fontsize=12, fontweight='bold')
    if col == 0: ax.set_ylabel('SR / GBN ratio')
    ax.legend(fontsize=8); ax.grid(True, alpha=0.3); ax.set_ylim(0.80, 1.10)
plt.tight_layout()
path = f"{OUT_DIR}/nvl72_ratio.png"
plt.savefig(path, dpi=150, bbox_inches='tight'); plt.close()
print(f"  -> {path}")

# =========================================================
# 文字汇总
# =========================================================
print(f"\n{'='*85}")
print("NVL72 Alltoall — GBN vs SR 数据汇总")
print(f"{'='*85}")
print(f"{'Size':<5} {'ErrRate':<10} {'GBN mean':>9} {'SR mean':>9} {'Δ%':>7} | {'GBN p99':>9} {'SR p99':>9} {'Δ%':>7} | {'GBN JCT':>9} {'SR JCT':>9} {'Δ%':>7}")
print("-"*100)
for ms in MSG_SIZES:
    for er in ERR_RATES:
        key = (ms, er)
        g, s = gbn.get(key, {}), sr.get(key, {})
        gm, sm = g.get('mean',0), s.get('mean',0)
        gp, sp = g.get('p99',0), s.get('p99',0)
        gj, sj = g.get('jct',0), s.get('jct',0)
        dm = (sm-gm)/gm*100 if gm else 0
        dp = (sp-gp)/gp*100 if gp else 0
        dj = (sj-gj)/gj*100 if gj else 0
        print(f"{ms:<5} {er:<10} {gm:>9.1f} {sm:>9.1f} {dm:>+6.1f}% | {gp:>9.1f} {sp:>9.1f} {dp:>+6.1f}% | {gj:>9.1f} {sj:>9.1f} {dj:>+6.1f}%")
    print()
print(f"{'='*85}")
print("Δ% = (SR-GBN)/GBN×100, 负数=SR更快")
