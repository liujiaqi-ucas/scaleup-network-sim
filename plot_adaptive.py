#!/usr/bin/env python3
"""
实验一：动态 α 自适应过程可视化
H100_8_0.005_OS2 + alltoall_4mb
体现 AIMD 控制器对突发错误的响应（乘法减 + 加法增）
"""
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import re, os

LOG_FILE = "/tmp/adaptive_exp/alpha_dynamic_0005_4mb.txt"

def load_alpha_log(path):
    """返回 {(sw_id, port): [(time_ns, alpha, ratio, retrans, used)]} """
    data = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'\[ALPHA_LOG\] (\d+) (\d+) (\d+) ([0-9.]+) ([0-9.e+-]+) (\d+) (\d+)', line)
            if m:
                t, sw, p, a, r, ret, used = (
                    int(m[1]), int(m[2]), int(m[3]),
                    float(m[4]), float(m[5]), int(m[6]), int(m[7]))
                key = (sw, p)
                data.setdefault(key, []).append((t, a, r, ret, used))
    return data

data = load_alpha_log(LOG_FILE)

# ── 找出发生 α 变化的端口 ──
adapting_ports = []
for (sw, p), recs in sorted(data.items()):
    alphas = [r[1] for r in recs]
    if min(alphas) < 0.499:   # α 曾经下降
        adapting_ports.append((sw, p, recs))

print(f"共 {len(data)} 个活跃端口，其中 {len(adapting_ports)} 个端口的 α 发生了下降")
for sw, p, recs in adapting_ports:
    alphas = [r[1] for r in recs]
    times  = [(r[0]-2000000000)/1000 for r in recs]   # offset to μs after job start
    print(f"  sw={sw} port={p}: min_α={min(alphas):.2f}  evaluations={len(recs)}")

# ── 绘图：双子图 ──
fig, axes = plt.subplots(1, 2, figsize=(13, 5))
fig.suptitle("Dynamic α AIMD Response to Burst Errors\n"
             "H100 8-GPU, Error Rate=0.005, Alltoall 4MB",
             fontsize=13, fontweight='bold', y=1.01)

# 颜色：每个端口一种
colors = ['#1565C0', '#C62828', '#2E7D32', '#6A1B9A', '#E65100']
ALPHA_MAX = 0.5
ALPHA_MIN = 0.1
THRESH    = 0.7   # retransThresh

# ── 子图 1：有 α 下降的端口的时间演化 ──
ax1 = axes[0]
ax1.axhline(ALPHA_MAX, color='gray', linewidth=1.5, linestyle='--',
            label=f'Global Fixed α={ALPHA_MAX}', zorder=2)

for i, (sw, p, recs) in enumerate(adapting_ports[:5]):
    times  = np.array([(r[0]-2000000000)/1000 for r in recs])
    alphas = np.array([r[1] for r in recs])
    ax1.plot(times, alphas, '-o', markersize=4,
             color=colors[i % len(colors)],
             label=f'Dynamic α  SW{sw}-Port{p}', zorder=3, linewidth=2)

# 标注 AIMD 事件
for sw, p, recs in adapting_ports[:1]:  # 只标注第一个
    for j, rec in enumerate(recs):
        t = (rec[0]-2000000000)/1000
        a = rec[1]
        if a < 0.499:   # 乘法减事件
            ax1.annotate(f'×{0.5:.1f}\n(MD)', xy=(t, a),
                        xytext=(t+2, a+0.04),
                        fontsize=8, color='red',
                        arrowprops=dict(arrowstyle='->', color='red', lw=1.2))
        if j > 0 and recs[j-1][1] < 0.499 and a > recs[j-1][1]:  # 加法增事件
            ax1.annotate(f'+{0.05:.2f}\n(AI)', xy=(t, a),
                        xytext=(t+1, a+0.03),
                        fontsize=7.5, color='green',
                        arrowprops=dict(arrowstyle='->', color='green', lw=1.2))

ax1.set_xlabel('Time after job start (μs)', fontsize=11)
ax1.set_ylabel('Alpha value', fontsize=11)
ax1.set_title('α Evolution: Adapting Ports\n(乘法减 MD + 加法增 AI)', fontsize=11, fontweight='bold')
ax1.set_ylim(ALPHA_MIN - 0.05, ALPHA_MAX + 0.1)
ax1.set_yticks([0.1, 0.25, 0.3, 0.35, 0.4, 0.45, 0.5])
ax1.grid(alpha=0.3)
ax1.legend(fontsize=9)

# ── 子图 2：整体 α 值分布直方图 + ratio 分布 ──
ax2 = axes[1]
all_alphas = [r[1] for recs in data.values() for r in recs]
all_ratios = [r[2] for recs in data.values() for r in recs if r[2] > 0]

ax2_r = ax2.twinx()

# α 分布（左轴）
bins_a = [0.1, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45, 0.49, 0.51]
counts, edges, _ = ax2.hist(all_alphas, bins=bins_a,
                            color='#1565C0', alpha=0.75, label='α distribution',
                            edgecolor='white', linewidth=0.5)
ax2.axvline(ALPHA_MAX, color='gray', linestyle='--', linewidth=1.5, label=f'Fixed α={ALPHA_MAX}')
ax2.set_xlabel('α value', fontsize=11)
ax2.set_ylabel('Count (α)', fontsize=11, color='#1565C0')
ax2.tick_params(axis='y', labelcolor='#1565C0')

# retrans ratio 分布（右轴）
if all_ratios:
    ax2_r.hist(all_ratios, bins=30, color='#C62828', alpha=0.4,
               label='Retrans ratio', edgecolor='white')
    ax2_r.axvline(THRESH, color='orange', linestyle=':', linewidth=2,
                  label=f'Threshold={THRESH}')
    ax2_r.set_ylabel('Count (retrans ratio)', fontsize=11, color='#C62828')
    ax2_r.tick_params(axis='y', labelcolor='#C62828')

ax2.set_title('α Value & Retrans Ratio Distribution\n(全部端口统计)', fontsize=11, fontweight='bold')
lines1, labels1 = ax2.get_legend_handles_labels()
lines2, labels2 = ax2_r.get_legend_handles_labels()
ax2.legend(lines1+lines2, labels1+labels2, fontsize=9, loc='upper left')
ax2.grid(alpha=0.3)

plt.tight_layout()
out = "/home/liujiaqi/conweave-ns3-CBFC+GBN/alpha_adaptive_process.png"
plt.savefig(out, dpi=150, bbox_inches='tight')
print(f"\n图表已保存: {out}")

# ── 输出 AIMD 事件摘要 ──
print("\n=== AIMD 事件摘要 ===")
for sw, p, recs in adapting_ports:
    times  = [(r[0]-2000000000)/1000 for r in recs]
    alphas = [r[1] for r in recs]
    ratios = [r[2] for r in recs]
    print(f"\nSW{sw}-Port{p}:")
    for i, (t, a, r) in enumerate(zip(times, alphas, ratios)):
        prev_a = alphas[i-1] if i > 0 else 0.5
        if a < prev_a - 0.01:
            event = f"⬇ MD (×0.5): {prev_a:.2f}→{a:.2f}  ratio={r:.4f}"
        elif a > prev_a + 0.01:
            event = f"⬆ AI (+0.05): {prev_a:.2f}→{a:.2f}"
        else:
            continue
        print(f"  t={t:.0f}μs: {event}")
