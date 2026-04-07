#!/usr/bin/env python3
"""
GBN vs SR 对比图（含 JCT）
每个子图：均值FCT + P99 FCT + JCT  三个指标
"""
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import os

DATA_DIR = "/tmp/exp_results"
SIZES = ["1mb", "4mb", "16mb"]
SIZE_LABELS = ["1MB", "4MB", "16MB"]

def load_records(path):
    """返回 [(start_ns, fct_ns), ...] 列表"""
    records = []
    if not os.path.exists(path):
        return records
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 7:
                try:
                    start_ns = int(parts[5])
                    fct_ns   = int(parts[6])
                    records.append((start_ns, fct_ns))
                except ValueError:
                    pass
    return records

def compute_metrics(records):
    """返回 (mean_fct_us, p99_fct_us, jct_us)"""
    if not records:
        return 0, 0, 0
    fcts = np.array([r[1] for r in records]) / 1000.0          # ns -> us
    finish_times = np.array([r[0] + r[1] for r in records])    # ns
    start_times  = np.array([r[0] for r in records])           # ns
    jct_us = (finish_times.max() - start_times.min()) / 1000.0
    return np.mean(fcts), np.percentile(fcts, 99), jct_us

flow_types = [
    ("allreduce", "Allreduce", [f"flow_allreduce_8gpu_{s}" for s in SIZES]),
    ("alltoall",  "Alltoall",  [f"flow_alltoall_8gpu_{s}"  for s in SIZES]),
]

# ──────────────────────────────────────────────
# 每个子图：每组消息大小显示 6 根柱
#   GBN-Mean  GBN-P99  GBN-JCT | SR-Mean  SR-P99  SR-JCT
# ──────────────────────────────────────────────
fig, axes = plt.subplots(1, 2, figsize=(15, 5.5))
fig.suptitle("GBN vs SR — H100 8-GPU, Error Rate = 0.001",
             fontsize=14, fontweight='bold', y=1.02)

BW = 0.13   # 单柱宽度

# 颜色方案
C = {
    'GBN_mean': '#1565C0',   # 深蓝
    'GBN_p99' : '#64B5F6',   # 中蓝
    'GBN_jct' : '#B3E5FC',   # 浅蓝
    'SR_mean' : '#B71C1C',   # 深红
    'SR_p99'  : '#EF5350',   # 中红
    'SR_jct'  : '#FFCDD2',   # 浅红
}
LABELS = ['GBN Mean', 'GBN P99', 'GBN JCT', 'SR Mean', 'SR P99', 'SR JCT']
COLORS = [C['GBN_mean'], C['GBN_p99'], C['GBN_jct'],
          C['SR_mean'],  C['SR_p99'],  C['SR_jct']]
# 6根柱的偏移量（中心对齐）
OFFSETS = np.array([-2.5, -1.5, -0.5, 0.5, 1.5, 2.5]) * BW

print("=" * 65)
print(f"{'指标':<12} {'GBN mean':>10} {'GBN P99':>10} {'GBN JCT':>10} "
      f"{'SR mean':>10} {'SR P99':>10} {'SR JCT':>10}")
print("=" * 65)

for ax, (ftype, ftitle, files) in zip(axes, flow_types):
    n = len(SIZES)
    x = np.arange(n)

    data = {}  # size -> [gbn_mean, gbn_p99, gbn_jct, sr_mean, sr_p99, sr_jct]
    for i, (size_label, ffile) in enumerate(zip(SIZE_LABELS, files)):
        gm, gp, gj = compute_metrics(load_records(f"{DATA_DIR}/gbn/{ffile}.txt"))
        sm, sp, sj = compute_metrics(load_records(f"{DATA_DIR}/sr/{ffile}.txt"))
        data[i] = [gm, gp, gj, sm, sp, sj]
        print(f"[{ftype}] {size_label:<8} "
              f"{gm:>10.1f} {gp:>10.1f} {gj:>10.1f} "
              f"{sm:>10.1f} {sp:>10.1f} {sj:>10.1f}")

    print("-" * 65)

    # 绘制每根柱
    bars_list = []
    for bi, (label, color, offset) in enumerate(zip(LABELS, COLORS, OFFSETS)):
        vals = [data[i][bi] for i in range(n)]
        bars = ax.bar(x + offset, vals, BW, color=color, label=label, zorder=3,
                      edgecolor='white', linewidth=0.5)
        bars_list.append(bars)

        # 柱顶数值标签（JCT 柱加粗字体）
        for bar in bars:
            h = bar.get_height()
            if h > 0:
                fw = 'bold' if 'JCT' in label else 'normal'
                fs = 7 if h < 50 else 7
                ax.text(bar.get_x() + bar.get_width()/2, h * 1.015,
                        f'{h:.0f}', ha='center', va='bottom',
                        fontsize=fs, fontweight=fw, rotation=0)

    # 每组中间画分隔虚线
    for xi in x:
        ax.axvline(xi + OFFSETS[2] + BW/2, color='gray',
                   linewidth=1.0, linestyle='--', alpha=0.5, zorder=2)

    ax.set_xticks(x)
    ax.set_xticklabels(SIZE_LABELS, fontsize=11)
    ax.set_xlabel('Message Size', fontsize=11)
    ax.set_ylabel('Time (μs)', fontsize=11)
    ax.set_title(ftitle, fontsize=12, fontweight='bold')
    ax.legend(fontsize=8.5, ncol=2, loc='upper left',
              framealpha=0.85, edgecolor='gray')
    ax.grid(axis='y', alpha=0.35, zorder=0)
    ax.set_ylim(bottom=0)

    # 在图内标注 GBN | SR 区域
    ymax = ax.get_ylim()[1]
    ax.text(x[0] + OFFSETS[0] - BW*0.3, ymax * 0.96, '←GBN',
            fontsize=8, color='navy', ha='left')
    ax.text(x[0] + OFFSETS[3] - BW*0.3, ymax * 0.96, 'SR→',
            fontsize=8, color='darkred', ha='left')

print()
print("图例说明:")
print("  深色柱 = Mean FCT  (均值流完成时间)")
print("  中色柱 = P99  FCT  (尾延迟，最慢1%的流)")
print("  浅色柱 = JCT       (整个集合通信作业完成时间)")
print("  虚线   = GBN区 / SR区 分界线")

plt.tight_layout()
out = "/home/liujiaqi/conweave-ns3-CBFC+GBN/gbn_vs_sr_v3.png"
plt.savefig(out, dpi=150, bbox_inches='tight')
print(f"\n图表已保存: {out}")
