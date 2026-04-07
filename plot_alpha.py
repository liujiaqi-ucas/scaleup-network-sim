#!/usr/bin/env python3
"""
全局固定 α vs 动态 α 对比图
H100_8_0.001_OS2，4MB 消息大小
子图1: allreduce_4mb   子图2: alltoall_4mb
指标: Mean FCT / P99 FCT / JCT
"""
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import os

BASE = "/tmp/alpha_exp"

def load_records(path):
    records = []
    if not os.path.exists(path):
        return records
    with open(path) as f:
        for line in f:
            p = line.strip().split()
            if len(p) >= 7:
                try:
                    records.append((int(p[5]), int(p[6])))
                except ValueError:
                    pass
    return records

def metrics(records):
    if not records:
        return 0, 0, 0
    fcts = np.array([r[1] for r in records]) / 1000.0
    jct = (max(r[0]+r[1] for r in records) - min(r[0] for r in records)) / 1000.0
    return np.mean(fcts), np.percentile(fcts, 99), jct

# 5 种配置
configs = [
    ("global_02", "Global\nα=0.2", "#E57373"),   # 浅红
    ("global_05", "Global\nα=0.5", "#EF5350"),   # 中红
    ("global_08", "Global\nα=0.8", "#B71C1C"),   # 深红
    ("dynamic",   "Dynamic\nα", "#1565C0"),       # 蓝（dynamic）
]

flows = [
    ("flow_allreduce_8gpu_4mb", "Allreduce 4MB"),
    ("flow_alltoall_8gpu_4mb",  "Alltoall 4MB"),
]

fig, axes = plt.subplots(1, 2, figsize=(13, 5.5))
fig.suptitle("Fixed Global α vs Dynamic α\nH100, Error Rate=0.001, Message Size=4MB",
             fontsize=13, fontweight='bold', y=1.02)

BW = 0.17
METRICS = ["Mean FCT", "P99 FCT", "JCT"]
OFFSETS = np.array([-1, 0, 1]) * BW  # 每组3根柱

print("=" * 70)
print(f"{'配置':<14} {'Mean FCT(μs)':>13} {'P99 FCT(μs)':>13} {'JCT(μs)':>13}")
print("=" * 70)

for ax, (flow_file, flow_title) in zip(axes, flows):
    print(f"\n[{flow_title}]")
    x = np.arange(len(configs))
    all_means, all_p99s, all_jcts = [], [], []

    for cfg_dir, label, color in configs:
        path = f"{BASE}/{cfg_dir}/{flow_file}.txt"
        m, p99, jct = metrics(load_records(path))
        all_means.append(m)
        all_p99s.append(p99)
        all_jcts.append(jct)
        print(f"  {label.replace(chr(10),' '):<12} {m:>13.1f} {p99:>13.1f} {jct:>13.1f}")

    # 每个配置画3根柱：mean / p99 / jct
    metric_vals = [all_means, all_p99s, all_jcts]
    metric_colors = ['#5C6BC0', '#AB47BC', '#26A69A']  # 蓝/紫/青绿
    metric_hatches = ['', '//', 'xx']

    for xi, (cfg_dir, label, base_color) in enumerate(configs):
        for mi, (mvals, mc, mh) in enumerate(zip(metric_vals, metric_colors, metric_hatches)):
            val = mvals[xi]
            bar = ax.bar(xi + OFFSETS[mi], val, BW,
                         color=mc, alpha=0.75 if cfg_dir != 'dynamic' else 1.0,
                         edgecolor='black' if cfg_dir == 'dynamic' else 'gray',
                         linewidth=1.5 if cfg_dir == 'dynamic' else 0.5,
                         hatch=mh, zorder=3)
            # 柱顶数值
            ax.text(xi + OFFSETS[mi], val * 1.015, f'{val:.0f}',
                    ha='center', va='bottom', fontsize=7.5,
                    fontweight='bold' if cfg_dir == 'dynamic' else 'normal')

    # x轴标签 = 配置名
    x_labels = [c[1] for c in configs]
    ax.set_xticks(x)
    ax.set_xticklabels(x_labels, fontsize=10)
    ax.set_ylabel('Time (μs)', fontsize=11)
    ax.set_title(flow_title, fontsize=12, fontweight='bold')
    ax.grid(axis='y', alpha=0.3, zorder=0)
    ax.set_ylim(bottom=0)

    # 突出 dynamic 列（添加背景色）
    dyn_idx = [i for i, c in enumerate(configs) if c[0]=='dynamic'][0]
    ax.axvspan(dyn_idx - 0.4, dyn_idx + 0.4, alpha=0.08, color='blue', zorder=0)

    # 图例（只在第一个子图显示）
    if ax == axes[0]:
        from matplotlib.patches import Patch
        legend_handles = [
            Patch(facecolor='#5C6BC0', hatch='',   label='Mean FCT'),
            Patch(facecolor='#AB47BC', hatch='//',  label='P99  FCT'),
            Patch(facecolor='#26A69A', hatch='xx', label='JCT'),
        ]
        ax.legend(handles=legend_handles, fontsize=9, loc='upper left')

print("\n" + "=" * 70)
print("说明: 蓝色阴影列 = Dynamic α（自适应）；其余三列 = 全局固定 α")

plt.tight_layout()
out = "/tmp/alpha_comparison.png"
plt.savefig(out, dpi=150, bbox_inches='tight')
print(f"\n图表已保存: {out}")
