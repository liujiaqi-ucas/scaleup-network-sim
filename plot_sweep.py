#!/usr/bin/env python3
"""
全局固定 α(0.5) vs 动态 α 对比
横轴: 消息大小 (1MB/4MB/16MB)
纵轴: JCT (μs)
分面: 错误率 (0.00001 / 0.0001 / 0.001)
两子图: alltoall / allreduce
"""
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import os, itertools

BASE = "/tmp/sweep_exp"
RATES  = ["0.00001", "0.0001", "0.001"]
SIZES  = ["1mb", "4mb", "16mb"]
SIZE_X = ["1MB", "4MB", "16MB"]

def load(path):
    rows = []
    if not os.path.exists(path): return rows
    with open(path) as f:
        for line in f:
            p = line.strip().split()
            if len(p)>=7:
                try: rows.append((int(p[5]), int(p[6])))
                except: pass
    return rows

def jct(rows):
    if not rows: return None
    return (max(r[0]+r[1] for r in rows) - min(r[0] for r in rows)) / 1000.0

def mean_fct(rows):
    if not rows: return None
    return np.mean([r[1] for r in rows]) / 1000.0

# ── 收集数据 ──
data = {}   # data[(flow_type, rate, size)] = {'global': jct, 'dynamic': jct}
for flow_type in ["alltoall", "allreduce"]:
    for rate in RATES:
        for size in SIZES:
            fname = f"flow_{flow_type}_8gpu_{size}"
            gp = f"{BASE}/global/{rate}_{fname}.txt"
            dp = f"{BASE}/dynamic/{rate}_{fname}.txt"
            key = (flow_type, rate, size)
            data[key] = {
                'global_jct':   jct(load(gp)),
                'dynamic_jct':  jct(load(dp)),
                'global_mean':  mean_fct(load(gp)),
                'dynamic_mean': mean_fct(load(dp)),
            }

# ── 打印数据表 ──
print("="*80)
print(f"{'流量类型':<10} {'错误率':<10} {'消息':<6} "
      f"{'固定α JCT':>12} {'动态α JCT':>12} {'JCT改善%':>10} "
      f"{'固定α 均值':>12} {'动态α 均值':>12}")
print("="*80)
for ft in ["alltoall","allreduce"]:
    for rate in RATES:
        for size in SIZES:
            d = data[(ft,rate,size)]
            gj, dj = d['global_jct'], d['dynamic_jct']
            gm, dm = d['global_mean'], d['dynamic_mean']
            imp = (gj-dj)/gj*100 if gj and gj>0 else 0
            print(f"{ft:<10} {rate:<10} {size:<6} "
                  f"{gj:>12.1f} {dj:>12.1f} {imp:>9.1f}% "
                  f"{gm:>12.1f} {dm:>12.1f}")
    print("-"*80)

# ── 绘图 ──
fig, axes = plt.subplots(2, 3, figsize=(15, 9), sharey='row')
fig.suptitle("Global Fixed α (0.5) vs Dynamic α\nH100 8-GPU | Metric: JCT",
             fontsize=13, fontweight='bold', y=1.01)

rate_colors  = {"0.00001":"#43A047", "0.0001":"#FB8C00", "0.001":"#E53935"}
rate_labels  = {"0.00001":"err=10⁻⁵", "0.0001":"err=10⁻⁴", "0.001":"err=10⁻³"}
flow_titles  = {"alltoall":"Alltoall", "allreduce":"Allreduce"}

for row, flow_type in enumerate(["alltoall","allreduce"]):
    for col, rate in enumerate(RATES):
        ax = axes[row][col]
        x  = np.arange(len(SIZES))
        bw = 0.35

        g_jcts = [data[(flow_type,rate,s)]['global_jct'] or 0 for s in SIZES]
        d_jcts = [data[(flow_type,rate,s)]['dynamic_jct'] or 0 for s in SIZES]

        b1 = ax.bar(x - bw/2, g_jcts, bw, label='Global α=0.5',
                    color='#EF5350', alpha=0.85, zorder=3)
        b2 = ax.bar(x + bw/2, d_jcts, bw, label='Dynamic α',
                    color='#1565C0', alpha=0.9, zorder=3, edgecolor='#0D47A1', lw=1)

        # 改善百分比标注
        for xi, (gv, dv) in enumerate(zip(g_jcts, d_jcts)):
            if gv > 0 and dv > 0:
                imp = (gv - dv) / gv * 100
                sign = '+' if imp > 0 else ''
                color = 'green' if imp > 0 else 'red'
                ax.text(xi, max(gv, dv) * 1.04,
                        f'{sign}{imp:.1f}%', ha='center', va='bottom',
                        fontsize=8, color=color, fontweight='bold')

        # 柱顶数值
        for bars in [b1, b2]:
            for bar in bars:
                h = bar.get_height()
                ax.text(bar.get_x()+bar.get_width()/2, h*0.97,
                        f'{h:.0f}', ha='center', va='top', fontsize=7.5, color='white', fontweight='bold')

        ax.set_xticks(x)
        ax.set_xticklabels(SIZE_X, fontsize=10)
        ax.set_title(f"{flow_titles[flow_type]}  |  {rate_labels[rate]}",
                     fontsize=10, fontweight='bold')
        ax.grid(axis='y', alpha=0.3, zorder=0)
        ax.set_ylim(bottom=0)
        if col == 0:
            ax.set_ylabel('JCT (μs)', fontsize=10)
        if row == 0 and col == 2:
            ax.legend(fontsize=9, loc='upper left')

plt.tight_layout()
out = "/tmp/sweep_comparison.png"
plt.savefig(out, dpi=150, bbox_inches='tight')
print(f"\n图表已保存: {out}")

# ── 额外：JCT 改善率热力图 ──
fig2, axes2 = plt.subplots(1, 2, figsize=(12, 4))
fig2.suptitle("JCT Improvement: Dynamic α vs Global α (0.5)\n(正值 = Dynamic α 更快)",
              fontsize=12, fontweight='bold')

for ax2, flow_type in zip(axes2, ["alltoall","allreduce"]):
    matrix = np.zeros((len(RATES), len(SIZES)))
    for ri, rate in enumerate(RATES):
        for si, size in enumerate(SIZES):
            d = data[(flow_type, rate, size)]
            gj, dj = d['global_jct'], d['dynamic_jct']
            if gj and gj > 0:
                matrix[ri, si] = (gj - dj) / gj * 100

    im = ax2.imshow(matrix, cmap='RdYlGn', aspect='auto',
                    vmin=-5, vmax=20)
    ax2.set_xticks(range(len(SIZES)));  ax2.set_xticklabels(SIZE_X, fontsize=11)
    ax2.set_yticks(range(len(RATES)));  ax2.set_yticklabels([rate_labels[r] for r in RATES], fontsize=10)
    ax2.set_title(flow_titles[flow_type], fontsize=12, fontweight='bold')
    plt.colorbar(im, ax=ax2, label='JCT improvement (%)')
    for ri in range(len(RATES)):
        for si in range(len(SIZES)):
            ax2.text(si, ri, f"{matrix[ri,si]:.1f}%",
                     ha='center', va='center', fontsize=11, fontweight='bold',
                     color='black')

plt.tight_layout()
out2 = "/tmp/sweep_heatmap.png"
plt.savefig(out2, dpi=150, bbox_inches='tight')
print(f"热力图已保存: {out2}")
