#!/usr/bin/env python3
"""PFC vs CBFC 对比图 — 6 消息大小 × 5 错误率 × 2 流量类型"""
import csv, os, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CBFC_CSV = os.path.join(SCRIPT_DIR, "cbfc", "results.csv")
PFC_CSV  = os.path.join(SCRIPT_DIR, "pfc", "results.csv")
OUT_DIR  = os.path.join(SCRIPT_DIR, "plots")
os.makedirs(OUT_DIR, exist_ok=True)

ERR_RATES   = [0.00001, 0.00005, 0.0001, 0.0005, 0.001]
MSG_SIZES   = ["1mb", "4mb", "16mb", "64mb", "128mb", "256mb"]
SIZE_LABELS = ["1 MB", "4 MB", "16 MB", "64 MB", "128 MB", "256 MB"]
TRAFFICS    = ["allreduce", "alltoall"]
TRAFFIC_LABELS = {"allreduce": "Allreduce", "alltoall": "Alltoall"}

def load_csv(path):
    data = {}
    if not os.path.exists(path):
        print(f"WARNING: {path} not found"); return data
    with open(path) as f:
        for line in f:
            parts = line.strip().split(',')
            if len(parts) < 9: continue
            try:
                key = (parts[1], parts[2], float(parts[3]))
                data[key] = {'mean': float(parts[6]), 'p99': float(parts[7]),
                             'jct': float(parts[8]), 'flows': int(parts[4]),
                             'errors': int(parts[5])}
            except: pass
    return data

def get_vals(data, traffic, msgsize, metric):
    return [data.get((traffic, msgsize, er), {}).get(metric, np.nan) for er in ERR_RATES]

# ─── 图1/2: 综合对比 (每种流量一张, 3行×6列) ───
def plot_combined(traffic, cbfc, pfc):
    ncols = len(MSG_SIZES)
    fig, axes = plt.subplots(3, ncols, figsize=(ncols * 4, 11))
    fig.suptitle(f"{TRAFFIC_LABELS[traffic]} — CBFC vs PFC  (H100 8-GPU, egress-based PFC)",
                 fontsize=14, fontweight='bold')
    metrics = [('p99', 'P99 FCT (μs)'), ('mean', 'Mean FCT (μs)'), ('jct', 'JCT (μs)')]
    for row, (metric, ylabel) in enumerate(metrics):
        for col, (ms, slabel) in enumerate(zip(MSG_SIZES, SIZE_LABELS)):
            ax = axes[row][col]
            cbfc_v = get_vals(cbfc, traffic, ms, metric)
            pfc_v  = get_vals(pfc,  traffic, ms, metric)
            ax.plot(ERR_RATES, cbfc_v, 's-', color='#2196F3', lw=2, ms=6, label='CBFC')
            ax.plot(ERR_RATES, pfc_v,  'o-', color='#FF5722', lw=2, ms=6, label='PFC')
            all_v = [v for v in cbfc_v + pfc_v if not np.isnan(v) and v > 0]
            if all_v:
                ax.set_ylim(min(all_v)*0.85, max(all_v)*1.15)
            ax.set_xscale('log')
            if row == 2:
                ax.set_xlabel('Link Error Rate', fontsize=10)
                ax.set_xticks(ERR_RATES)
                ax.set_xticklabels([f'{e:.0e}' for e in ERR_RATES], fontsize=7, rotation=30)
            else:
                ax.set_xticks(ERR_RATES); ax.set_xticklabels([])
            if row == 0: ax.set_title(slabel, fontsize=12, fontweight='bold')
            if col == 0: ax.set_ylabel(ylabel, fontsize=10)
            ax.legend(fontsize=8, loc='upper left')
            ax.grid(True, alpha=0.3)
    plt.tight_layout()
    path = os.path.join(OUT_DIR, f"pfc_vs_cbfc_{traffic}.png")
    plt.savefig(path, dpi=150, bbox_inches='tight'); plt.close()
    print(f"  -> {path}")

# ─── 图3: PFC/CBFC 比值 ───
def plot_ratio(cbfc, pfc):
    colors = ['#1976D2', '#388E3C', '#F57C00', '#D32F2F', '#7B1FA2', '#00838F']
    fig, axes = plt.subplots(2, 3, figsize=(18, 8))
    fig.suptitle("PFC / CBFC Ratio  (ratio > 1 → PFC slower)", fontsize=14, fontweight='bold')
    for row, traffic in enumerate(TRAFFICS):
        for col, (metric, mlabel) in enumerate([('mean','Mean FCT'), ('p99','P99 FCT'), ('jct','JCT')]):
            ax = axes[row][col]
            for i, (ms, slabel) in enumerate(zip(MSG_SIZES, SIZE_LABELS)):
                cbfc_v = get_vals(cbfc, traffic, ms, metric)
                pfc_v  = get_vals(pfc,  traffic, ms, metric)
                ratio = [p/c if c > 0 else np.nan for p, c in zip(pfc_v, cbfc_v)]
                ax.plot(ERR_RATES, ratio, 'o-', color=colors[i], lw=2, ms=6, label=slabel)
            ax.axhline(y=1.0, color='gray', ls='--', alpha=0.5, lw=1)
            ax.set_xscale('log')
            ax.set_xlabel('Link Error Rate', fontsize=10)
            ax.set_xticks(ERR_RATES)
            ax.set_xticklabels([f'{e:.0e}' for e in ERR_RATES], fontsize=7, rotation=30)
            if col == 0: ax.set_ylabel('PFC / CBFC ratio', fontsize=10)
            ax.set_title(f"{TRAFFIC_LABELS[traffic]} — {mlabel}", fontsize=11, fontweight='bold')
            ax.legend(fontsize=8); ax.grid(True, alpha=0.3)
    plt.tight_layout()
    path = os.path.join(OUT_DIR, "pfc_cbfc_ratio.png")
    plt.savefig(path, dpi=150, bbox_inches='tight'); plt.close()
    print(f"  -> {path}")

# ─── 汇总表 ───
def print_summary(cbfc, pfc):
    print("\n" + "="*100)
    print("PFC vs CBFC 实验数据汇总 (egress-based PFC)")
    print("="*100)
    for traffic in TRAFFICS:
        print(f"\n--- {traffic.upper()} ---")
        print(f"{'Size':<6} {'ErrRate':<10} {'CBFC mean':>10} {'PFC mean':>10} {'Δ%':>7}  |  "
              f"{'CBFC p99':>10} {'PFC p99':>10} {'Δ%':>7}  |  "
              f"{'CBFC jct':>10} {'PFC jct':>10} {'Δ%':>7}")
        print("-"*100)
        for ms in MSG_SIZES:
            for er in ERR_RATES:
                key = (traffic, ms, er)
                c = cbfc.get(key, {}); p = pfc.get(key, {})
                cm, pm = c.get('mean',0), p.get('mean',0)
                cp, pp = c.get('p99',0),  p.get('p99',0)
                cj, pj = c.get('jct',0),  p.get('jct',0)
                dm = ((pm-cm)/cm*100) if cm>0 else 0
                dp = ((pp-cp)/cp*100) if cp>0 else 0
                dj = ((pj-cj)/cj*100) if cj>0 else 0
                print(f"{ms:<6} {er:<10} {cm:>10.1f} {pm:>10.1f} {dm:>+6.1f}%  |  "
                      f"{cp:>10.1f} {pp:>10.1f} {dp:>+6.1f}%  |  "
                      f"{cj:>10.1f} {pj:>10.1f} {dj:>+6.1f}%")
    print("="*100)
    print("Δ% = (PFC - CBFC) / CBFC × 100  → 正数表示 PFC 更慢\n")

if __name__ == '__main__':
    print("加载数据...")
    cbfc = load_csv(CBFC_CSV)
    pfc  = load_csv(PFC_CSV)
    print(f"CBFC: {len(cbfc)} 条  PFC: {len(pfc)} 条")
    if not cbfc and not pfc: print("ERROR: 无数据"); sys.exit(1)
    print("\n生成图表...")
    for t in TRAFFICS: plot_combined(t, cbfc, pfc)
    plot_ratio(cbfc, pfc)
    print_summary(cbfc, pfc)
    print("全部完成!")
