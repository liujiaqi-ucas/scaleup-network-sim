#!/usr/bin/env python3
import os, numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR    = os.path.join(SCRIPT_DIR, "plots", "sr_vs_gbn", "retrans")
os.makedirs(OUT_DIR, exist_ok=True)

def load(path):
    data = {}
    with open(path) as f:
        for line in f:
            p = line.strip().split(',')
            if len(p) < 10: continue
            try:
                data[(p[1], p[2], float(p[3]))] = int(p[6])
            except: pass
    return data

sr  = load(os.path.join(SCRIPT_DIR, "sr",  "results.csv"))
gbn = load(os.path.join(SCRIPT_DIR, "gbn", "results.csv"))

# 只取有意义的 BER 范围（重传数 > 0 的部分）
BERS       = [1e-9, 1e-8, 1e-7, 1e-6]
BER_LABELS = ["1e-9", "1e-8", "1e-7", "1e-6"]
SIZES      = ["4mb", "16mb", "64mb", "256mb"]
COLORS     = ["#1976D2", "#388E3C", "#F57C00", "#D32F2F"]

# ════════════════════════════════════════════════════
# 图1：SR vs GBN 重传量（allreduce + alltoall）
# ════════════════════════════════════════════════════
fig, axes = plt.subplots(1, 2, figsize=(13, 5))
fig.suptitle("SR vs GBN: Total Retransmissions", fontsize=14, fontweight='bold', y=1.01)

for ax, traffic, title in zip(axes,
        ["allreduce", "alltoall"],
        ["Allreduce", "Alltoall"]):

    x = np.arange(len(BERS))
    for sz, clr in zip(SIZES, COLORS):
        sr_v  = [sr.get( (traffic, sz, b), 0) for b in BERS]
        gbn_v = [gbn.get((traffic, sz, b), 0) for b in BERS]
        # SR: 实线粗；GBN: 虚线同色
        ax.plot(x, sr_v,  color=clr, ls='-',  lw=2.5, marker='o', ms=6,
                label=f"SR  {sz.upper()}")
        ax.plot(x, gbn_v, color=clr, ls='--', lw=2.5, marker='s', ms=6,
                label=f"GBN {sz.upper()}")

    ax.set_title(title, fontsize=12, fontweight='bold')
    ax.set_yscale('log')
    ax.set_xticks(x)
    ax.set_xticklabels(BER_LABELS, fontsize=11)
    ax.set_xlabel("Bit Error Rate (BER)", fontsize=11)
    ax.set_ylabel("Retransmitted flits (log)", fontsize=11)
    ax.grid(True, which='both', alpha=0.25)

    # 图例分两列：SR / GBN
    handles, labels = ax.get_legend_handles_labels()
    ax.legend(handles, labels, fontsize=9, ncol=2,
              title="─── SR    ─ ─ GBN", title_fontsize=9)

plt.tight_layout()
out1 = os.path.join(OUT_DIR, "simple_retrans_count.png")
fig.savefig(out1, dpi=150, bbox_inches='tight')
plt.close(fig)
print(f"saved: {out1}")

# ════════════════════════════════════════════════════
# 图2：GBN / SR 重传比值热图（行=消息大小，列=BER）
# ════════════════════════════════════════════════════
ALL_SIZES  = ["1mb", "4mb", "16mb", "64mb", "128mb", "256mb"]

fig, axes = plt.subplots(1, 2, figsize=(12, 4))
fig.suptitle("GBN / SR Retransmission Ratio  (target: ~2×)", fontsize=13,
             fontweight='bold', y=1.02)

for ax, traffic, title in zip(axes,
        ["allreduce", "alltoall"],
        ["Allreduce", "Alltoall"]):

    matrix = []
    for sz in ALL_SIZES:
        row = []
        for b in BERS:
            sv = sr.get( (traffic, sz, b), 0)
            gv = gbn.get((traffic, sz, b), 0)
            row.append(gv / sv if sv > 0 else np.nan)
        matrix.append(row)
    matrix = np.array(matrix, dtype=float)

    im = ax.imshow(matrix, vmin=1.0, vmax=3.0, cmap='RdYlGn_r', aspect='auto')
    ax.set_xticks(range(len(BERS)))
    ax.set_xticklabels(BER_LABELS, fontsize=10)
    ax.set_yticks(range(len(ALL_SIZES)))
    ax.set_yticklabels([s.upper() for s in ALL_SIZES], fontsize=10)
    ax.set_xlabel("BER", fontsize=11)
    ax.set_title(title, fontsize=12, fontweight='bold')

    # 在每个格子里写数值
    for i in range(len(ALL_SIZES)):
        for j in range(len(BERS)):
            v = matrix[i, j]
            if not np.isnan(v):
                ax.text(j, i, f"{v:.1f}×", ha='center', va='center',
                        fontsize=10, fontweight='bold',
                        color='white' if v > 2.2 else 'black')

    plt.colorbar(im, ax=ax, label="GBN / SR ratio", shrink=0.85)

plt.tight_layout()
out2 = os.path.join(OUT_DIR, "simple_retrans_ratio.png")
fig.savefig(out2, dpi=150, bbox_inches='tight')
plt.close(fig)
print(f"saved: {out2}")
