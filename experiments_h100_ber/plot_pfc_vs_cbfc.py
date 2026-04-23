#!/usr/bin/env python3
"""
专为 PFC vs CBFC 小节生成的图
图1: 2×3 矩阵 —— allreduce(左) vs alltoall(右)，3行=mean/p99/jct，x=BER，6条线=消息大小
图2: 收敛分析图 —— alltoall ratio (PFC/SR) vs BER，直观展示差距随BER变化
"""
import os, numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "plots", "sr_vs_pfc")
os.makedirs(OUT_DIR, exist_ok=True)

def load(path):
    data = {}
    with open(path) as f:
        for line in f:
            p = line.strip().split(',')
            if len(p) < 9: continue
            try:
                off = 1 if len(p) >= 10 else 0
                data[(p[1],p[2],float(p[3]))] = {
                    'mean': float(p[6+off]), 'p99': float(p[7+off]), 'jct': float(p[8+off])
                }
            except: pass
    return data

sr  = load(os.path.join(SCRIPT_DIR, "sr",  "results.csv"))
pfc = load(os.path.join(SCRIPT_DIR, "PFC", "results.csv"))

BERS      = [1e-15,1e-14,1e-13,1e-12,1e-11,1e-10,1e-9,1e-8,1e-7,1e-6]
BER_TICKS = [f"{b:.0e}" for b in BERS]
SIZES     = ["1mb","4mb","16mb","64mb","128mb","256mb"]
SZ_LABELS = ["1MB","4MB","16MB","64MB","128MB","256MB"]
COLORS    = ["#1976D2","#388E3C","#F57C00","#D32F2F","#7B1FA2","#00838F"]
METRICS   = [("mean","Mean FCT (μs)"),("p99","P99 FCT (μs)"),("jct","JCT (ms)")]

def get(d, traffic, sz, metric):
    return [d.get((traffic,sz,b),{}).get(metric, np.nan) for b in BERS]

# ═══════════════════════════════════════════════════════
# 图1: 2×3 主对比图
# ═══════════════════════════════════════════════════════
fig, axes = plt.subplots(3, 2, figsize=(13, 11))
fig.suptitle("CBFC vs PFC: FCT Comparison (SR retransmission, H100 8-GPU)",
             fontsize=13, fontweight='bold', y=1.01)

x = np.arange(len(BERS))

for row, (metric, ylabel) in enumerate(METRICS):
    for col, traffic in enumerate(["allreduce","alltoall"]):
        ax = axes[row][col]
        for sz, slbl, clr in zip(SIZES, SZ_LABELS, COLORS):
            y_sr  = get(sr,  traffic, sz, metric)
            y_pfc = get(pfc, traffic, sz, metric)
            ax.plot(x, y_sr,  color=clr, ls='-',  lw=2, marker='o', ms=4)
            ax.plot(x, y_pfc, color=clr, ls='--', lw=2, marker='s', ms=4)

        if row == 0:
            ax.set_title(f"{'Allreduce' if col==0 else 'Alltoall'}",
                         fontsize=12, fontweight='bold')
        ax.set_xticks(x)
        ax.set_xticklabels(BER_TICKS, rotation=40, ha='right', fontsize=7.5)
        ax.set_xlabel("BER", fontsize=9)
        ax.set_ylabel(ylabel, fontsize=9)
        ax.grid(True, alpha=0.25)

        # allreduce: 用线性轴更能体现"几乎没差"
        if col == 1:
            ax.set_yscale('log')

# 统一图例：颜色=消息大小，线型=协议
legend_size = [Line2D([0],[0], color=c, lw=2, label=l)
               for c,l in zip(COLORS, SZ_LABELS)]
legend_proto = [
    Line2D([0],[0], color='gray', ls='-',  lw=2, marker='o', ms=5, label='CBFC'),
    Line2D([0],[0], color='gray', ls='--', lw=2, marker='s', ms=5, label='PFC'),
]
fig.legend(handles=legend_size + legend_proto,
           loc='lower center', ncol=8, fontsize=9,
           frameon=True, bbox_to_anchor=(0.5, -0.04))

plt.tight_layout()
out1 = os.path.join(OUT_DIR, "cbfc_vs_pfc_main.png")
fig.savefig(out1, dpi=150, bbox_inches='tight')
plt.close(fig)
print(f"saved: {out1}")

# ═══════════════════════════════════════════════════════
# 图2: ratio 图 —— allreduce + alltoall 并排
#   核心故事：allreduce ≈ 1.0 全程，alltoall 从1.10降到1.02
# ═══════════════════════════════════════════════════════
fig, axes = plt.subplots(1, 2, figsize=(13, 4.5))
fig.suptitle("PFC / CBFC Mean FCT Ratio vs BER  (>1 means CBFC faster)",
             fontsize=12, fontweight='bold')

for ax, traffic, title in zip(axes,
        ["allreduce","alltoall"],
        ["Allreduce","Alltoall"]):
    for sz, slbl, clr in zip(SIZES, SZ_LABELS, COLORS):
        sr_v  = np.array(get(sr,  traffic, sz, "mean"))
        pfc_v = np.array(get(pfc, traffic, sz, "mean"))
        with np.errstate(invalid='ignore', divide='ignore'):
            ratio = np.where(sr_v > 0, pfc_v / sr_v, np.nan)
        ax.plot(x, ratio, color=clr, lw=2, marker='o', ms=5, label=slbl)

    ax.axhline(1.0, color='black', ls=':', lw=1.2)
    ax.set_title(title, fontsize=11, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels(BER_TICKS, rotation=40, ha='right', fontsize=8)
    ax.set_xlabel("BER", fontsize=10)
    ax.set_ylabel("PFC / CBFC ratio", fontsize=10)
    ax.grid(True, alpha=0.25)

    # allreduce y轴缩小到能看出变化
    if traffic == "allreduce":
        ax.set_ylim(0.97, 1.05)
        ax.set_yticks([0.97, 0.99, 1.00, 1.01, 1.03, 1.05])
    else:
        ax.set_ylim(0.95, 1.25)
    ax.legend(fontsize=8, ncol=2)

plt.tight_layout()
out2 = os.path.join(OUT_DIR, "cbfc_vs_pfc_ratio.png")
fig.savefig(out2, dpi=150, bbox_inches='tight')
plt.close(fig)
print(f"saved: {out2}")

# ═══════════════════════════════════════════════════════
# 图3: alltoall 只看 mean+p99，突出尾部差异
#   两行：top=mean, bottom=p99
#   选 4 个有代表性的 size
# ═══════════════════════════════════════════════════════
KEY_SIZES  = ["4mb","16mb","64mb","256mb"]
KEY_LABELS = ["4MB","16MB","64MB","256MB"]
KEY_COLORS = ["#388E3C","#F57C00","#D32F2F","#7B1FA2"]

fig, axes = plt.subplots(1, 2, figsize=(13, 4.5))
fig.suptitle("Alltoall: CBFC vs PFC — Mean FCT and P99 FCT",
             fontsize=12, fontweight='bold')

for ax, (metric, ylabel) in zip(axes,
        [("mean","Mean FCT (μs)"),("p99","P99 FCT (μs)")]):
    for sz, slbl, clr in zip(KEY_SIZES, KEY_LABELS, KEY_COLORS):
        y_sr  = get(sr,  "alltoall", sz, metric)
        y_pfc = get(pfc, "alltoall", sz, metric)
        ax.plot(x, y_sr,  color=clr, ls='-',  lw=2.2, marker='o', ms=5,
                label=f"CBFC {slbl}")
        ax.plot(x, y_pfc, color=clr, ls='--', lw=2.2, marker='s', ms=5,
                label=f"PFC  {slbl}")
    ax.set_xticks(x)
    ax.set_xticklabels(BER_TICKS, rotation=40, ha='right', fontsize=8)
    ax.set_xlabel("BER", fontsize=10)
    ax.set_ylabel(ylabel, fontsize=10)
    ax.set_yscale('log')
    ax.set_title(ylabel, fontsize=11, fontweight='bold')
    ax.grid(True, which='both', alpha=0.25)
    ax.legend(fontsize=8, ncol=2)

plt.tight_layout()
out3 = os.path.join(OUT_DIR, "cbfc_vs_pfc_alltoall_detail.png")
fig.savefig(out3, dpi=150, bbox_inches='tight')
plt.close(fig)
print(f"saved: {out3}")
