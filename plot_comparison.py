#!/usr/bin/env python3
"""
GBN vs SR 对比图
H100_8_0.001_OS2 拓扑，0.001 错误率
两个子图：allreduce 和 alltoall 各三种消息大小（1mb/4mb/16mb）
指标：平均 FCT（归一化到消息大小的传输时间）和 p99 FCT
"""
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import os

DATA_DIR = "/tmp/exp_results"

def load_fct(path):
    """加载 FCT 文件，返回 fct (ns) 列表"""
    fcts = []
    if not os.path.exists(path):
        return fcts
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 7:
                try:
                    fct_ns = int(parts[6])
                    fcts.append(fct_ns)
                except ValueError:
                    pass
    return fcts

def fct_stats(fcts):
    """返回 (mean_us, p50_us, p99_us)"""
    if not fcts:
        return 0, 0, 0
    arr = np.array(fcts) / 1000.0  # ns -> us
    return np.mean(arr), np.percentile(arr, 50), np.percentile(arr, 99)

# 配置
SIZES = ["1mb", "4mb", "16mb"]
SIZE_LABELS = ["1MB", "4MB", "16MB"]

flow_types = {
    "allreduce": {
        "files": [f"flow_allreduce_8gpu_{s}" for s in SIZES],
        "title": "Allreduce (H100, error=0.001)",
    },
    "alltoall": {
        "files": [f"flow_alltoall_8gpu_{s}" for s in SIZES],
        "title": "Alltoall (H100, error=0.001)",
    }
}

# 保存实验数据到文本
summary_lines = ["GBN vs SR 对比实验数据", "="*60,
                 "拓扑: H100_8_0.001_OS2  错误率: 0.001",
                 "指标: 平均 FCT (μs) / P50 FCT (μs) / P99 FCT (μs)", ""]

fig, axes = plt.subplots(1, 2, figsize=(14, 6))
fig.suptitle("GBN vs SR: FCT Comparison\nH100 8-GPU, Error Rate=0.001",
             fontsize=14, fontweight='bold')

colors = {'GBN': '#2196F3', 'SR': '#FF5722'}
bar_width = 0.35

for ax_idx, (flow_type, cfg) in enumerate(flow_types.items()):
    ax = axes[ax_idx]
    summary_lines.append(f"[{flow_type.upper()}]")

    gbn_means, gbn_p99s = [], []
    sr_means, sr_p99s = [], []

    for flow_file in cfg["files"]:
        # GBN
        gbn_fcts = load_fct(f"{DATA_DIR}/gbn/{flow_file}.txt")
        gbn_m, gbn_p50, gbn_p99 = fct_stats(gbn_fcts)
        gbn_means.append(gbn_m)
        gbn_p99s.append(gbn_p99)

        # SR
        sr_fcts = load_fct(f"{DATA_DIR}/sr/{flow_file}.txt")
        sr_m, sr_p50, sr_p99 = fct_stats(sr_fcts)
        sr_means.append(sr_m)
        sr_p99s.append(sr_p99)

        # 保存数据
        size = flow_file.split("_")[-1].upper()
        summary_lines.append(f"  {size}: GBN mean={gbn_m:.1f}μs p50={gbn_p50:.1f}μs p99={gbn_p99:.1f}μs  |  "
                              f"SR mean={sr_m:.1f}μs p50={sr_p50:.1f}μs p99={sr_p99:.1f}μs")

    summary_lines.append("")

    x = np.arange(len(SIZES))

    # 绘制均值柱状图
    b1 = ax.bar(x - bar_width/2, gbn_means, bar_width,
                label='GBN (mean)', color=colors['GBN'], alpha=0.85, zorder=3)
    b2 = ax.bar(x + bar_width/2, sr_means, bar_width,
                label='SR (mean)', color=colors['SR'], alpha=0.85, zorder=3)

    # 在柱上加 P99 误差线（P99 - mean）
    ax.errorbar(x - bar_width/2, gbn_means,
                yerr=[[0]*len(SIZES), [max(0, p-m) for m, p in zip(gbn_means, gbn_p99s)]],
                fmt='none', color='navy', capsize=4, linewidth=1.5, label='GBN p99')
    ax.errorbar(x + bar_width/2, sr_means,
                yerr=[[0]*len(SIZES), [max(0, p-m) for m, p in zip(sr_means, sr_p99s)]],
                fmt='none', color='darkred', capsize=4, linewidth=1.5, label='SR p99')

    # 在柱顶加数值标签
    for bar in b1:
        h = bar.get_height()
        if h > 0:
            ax.text(bar.get_x() + bar.get_width()/2, h + max(gbn_means)*0.02,
                    f'{h:.0f}', ha='center', va='bottom', fontsize=8, color='navy')
    for bar in b2:
        h = bar.get_height()
        if h > 0:
            ax.text(bar.get_x() + bar.get_width()/2, h + max(sr_means)*0.02,
                    f'{h:.0f}', ha='center', va='bottom', fontsize=8, color='darkred')

    ax.set_xlabel('Message Size', fontsize=11)
    ax.set_ylabel('FCT (μs)', fontsize=11)
    ax.set_title(cfg["title"], fontsize=12, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels(SIZE_LABELS, fontsize=10)
    ax.legend(fontsize=9)
    ax.grid(axis='y', alpha=0.4, zorder=0)
    ax.set_ylim(bottom=0)

plt.tight_layout()
out_fig = "/tmp/gbn_vs_sr_comparison.png"
plt.savefig(out_fig, dpi=150, bbox_inches='tight')
print(f"图表已保存: {out_fig}")

# 保存实验数据摘要
summary_path = "/tmp/exp_results/experiment_summary.txt"
with open(summary_path, 'w') as f:
    f.write('\n'.join(summary_lines))
print(f"数据摘要已保存: {summary_path}")
print('\n'.join(summary_lines))
