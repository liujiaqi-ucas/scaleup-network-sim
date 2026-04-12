#!/usr/bin/env python3
"""
分离架构 vs 统一架构(dynamic-alpha) 对比图
16MB, H100 8-GPU, 5个错误率, allreduce + alltoall
3个指标: Mean FCT / P99 FCT / JCT
"""
import os, csv
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "plots")
os.makedirs(OUT_DIR, exist_ok=True)

ERR_RATES = [0.00001, 0.00005, 0.0001, 0.0005, 0.001]
TRAFFICS = ['allreduce', 'alltoall']
TRAFFIC_LABELS = {'allreduce': 'Allreduce 16MB', 'alltoall': 'Alltoall 16MB'}

def load_csv(path):
    data = {}
    if not os.path.exists(path):
        print(f"WARNING: {path} not found"); return data
    for line in open(path):
        parts = line.strip().split(',')
        if len(parts) < 9: continue
        try:
            key = (parts[1], float(parts[3]))
            data[key] = {'mean': float(parts[6]), 'p99': float(parts[7]), 'jct': float(parts[8]),
                         'errors': int(parts[5])}
        except: pass
    return data

def get_vals(data, traffic, metric):
    return [data.get((traffic, er), {}).get(metric, np.nan) for er in ERR_RATES]

sep_data = load_csv(os.path.join(SCRIPT_DIR, "separate_results.csv"))
uni_data = load_csv(os.path.join(SCRIPT_DIR, "unified_results.csv"))
print(f"分离架构: {len(sep_data)} 条, 统一架构: {len(uni_data)} 条")

# =========================================================
# 图1: 综合对比 (2行traffic × 3列metric)
# =========================================================
fig, axes = plt.subplots(2, 3, figsize=(16, 8))
fig.suptitle("Separate Replay vs Unified SR  (H100 8-GPU, 16MB, CBFC)",
             fontsize=14, fontweight='bold')

metrics = [('mean', 'Mean FCT (μs)'), ('p99', 'P99 FCT (μs)'), ('jct', 'JCT (μs)')]

for row, traffic in enumerate(TRAFFICS):
    for col, (metric, ylabel) in enumerate(metrics):
        ax = axes[row][col]
        sep_v = get_vals(sep_data, traffic, metric)
        uni_v = get_vals(uni_data, traffic, metric)

        ax.plot(ERR_RATES, sep_v, 's-', color='#9C27B0', linewidth=2, markersize=7, label='Separate Replay')
        ax.plot(ERR_RATES, uni_v, 'o-', color='#FF5722', linewidth=2, markersize=7, label='Unified SR')

        all_v = [v for v in sep_v + uni_v if not np.isnan(v) and v > 0]
        if all_v:
            ax.set_ylim(min(all_v)*0.90, max(all_v)*1.10)

        ax.set_xscale('log')
        ax.set_xticks(ERR_RATES)
        if row == 1:
            ax.set_xticklabels([f'{e:.0e}' for e in ERR_RATES], fontsize=8, rotation=30)
            ax.set_xlabel('Link Error Rate', fontsize=10)
        else:
            ax.set_xticklabels([])
        if row == 0:
            ax.set_title(ylabel, fontsize=12, fontweight='bold')
        if col == 0:
            ax.set_ylabel(TRAFFIC_LABELS[traffic], fontsize=11, fontweight='bold')
        ax.legend(fontsize=8, loc='upper left')
        ax.grid(True, alpha=0.3)

plt.tight_layout()
path = os.path.join(OUT_DIR, "separate_vs_unified.png")
plt.savefig(path, dpi=150, bbox_inches='tight')
plt.close()
print(f"  -> {path}")

# =========================================================
# 图2: Speedup ratio (Separate/Unified, >1 means Separate slower)
# =========================================================
fig, axes = plt.subplots(1, 3, figsize=(16, 4.5))
fig.suptitle("Separate / Unified Ratio  (>1 → Separate slower, <1 → Separate faster)",
             fontsize=13, fontweight='bold')

colors = {'allreduce': '#2196F3', 'alltoall': '#FF9800'}
for col, (metric, mlabel) in enumerate(metrics):
    ax = axes[col]
    for traffic in TRAFFICS:
        sep_v = get_vals(sep_data, traffic, metric)
        uni_v = get_vals(uni_data, traffic, metric)
        ratio = [s/u if u > 0 else np.nan for s, u in zip(sep_v, uni_v)]
        ax.plot(ERR_RATES, ratio, 'o-', color=colors[traffic], linewidth=2, markersize=7,
                label=TRAFFIC_LABELS[traffic])

    ax.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5)
    ax.set_xscale('log')
    ax.set_xlabel('Link Error Rate', fontsize=10)
    ax.set_xticks(ERR_RATES)
    ax.set_xticklabels([f'{e:.0e}' for e in ERR_RATES], fontsize=8, rotation=30)
    ax.set_title(mlabel, fontsize=12, fontweight='bold')
    if col == 0:
        ax.set_ylabel('Separate / Unified ratio', fontsize=10)
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3)
    # 自适应Y轴
    ax.set_ylim(0.90, 1.15)

plt.tight_layout()
path = os.path.join(OUT_DIR, "separate_unified_ratio.png")
plt.savefig(path, dpi=150, bbox_inches='tight')
plt.close()
print(f"  -> {path}")

# =========================================================
# 文字汇总
# =========================================================
print("\n" + "="*80)
print("分离架构 vs 统一架构 对比 (16MB)")
print("="*80)
for traffic in TRAFFICS:
    print(f"\n--- {traffic.upper()} 16MB ---")
    print(f"{'ErrRate':<10} {'Sep mean':>9} {'Uni mean':>9} {'Δ%':>7} | {'Sep p99':>9} {'Uni p99':>9} {'Δ%':>7} | {'Sep JCT':>9} {'Uni JCT':>9} {'Δ%':>7}")
    print("-"*95)
    for er in ERR_RATES:
        key = (traffic, er)
        s = sep_data.get(key, {})
        u = uni_data.get(key, {})
        for metric_set in [('mean',), ('p99',), ('jct',)]:
            pass
        sm, um = s.get('mean',0), u.get('mean',0)
        sp, up = s.get('p99',0), u.get('p99',0)
        sj, uj = s.get('jct',0), u.get('jct',0)
        dm = (sm-um)/um*100 if um>0 else 0
        dp = (sp-up)/up*100 if up>0 else 0
        dj = (sj-uj)/uj*100 if uj>0 else 0
        print(f"{er:<10} {sm:>9.1f} {um:>9.1f} {dm:>+6.1f}% | {sp:>9.1f} {up:>9.1f} {dp:>+6.1f}% | {sj:>9.1f} {uj:>9.1f} {dj:>+6.1f}%")
print("="*80)
print("Δ% = (Separate - Unified) / Unified × 100  → 正数表示分离架构更慢")
