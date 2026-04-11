#!/usr/bin/env python3
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

error_rates = [0.00001, 0.0001, 0.0005, 0.001, 0.005]
labels = ['1e-5', '1e-4', '5e-4', '1e-3', '5e-3']

unified_avg = [464.0, 460.1, 468.1, 475.6, 525.9]
unified_p99 = [851.5, 772.7, 802.9, 823.7, 876.6]
separate_avg = [467.4, 465.7, 478.0, 484.5, 645.1]
separate_p99 = [767.1, 747.1, 809.7, 842.7, 7242.9]

x = np.arange(len(labels))
width = 0.35

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
fig.suptitle('Unified vs Separate Replay Buffer: FCT Comparison\n(16MB alltoall, H100_8, credit=128, total budget=4096 flits)', fontsize=12)

b1 = ax1.bar(x - width/2, unified_avg, width, label='Unified (pool=4096)', color='steelblue', alpha=0.85)
b2 = ax1.bar(x + width/2, separate_avg, width, label='Separate (pool=3072+replay)', color='coral', alpha=0.85)
ax1.set_xlabel('Link Error Rate', fontsize=11)
ax1.set_ylabel('Average FCT (us)', fontsize=11)
ax1.set_title('Average FCT', fontsize=12)
ax1.set_xticks(x); ax1.set_xticklabels(labels)
ax1.legend(fontsize=9); ax1.grid(axis='y', alpha=0.3)
for i, (u, s) in enumerate(zip(unified_avg, separate_avg)):
    diff = (s - u) / u * 100
    color = 'red' if diff > 0 else 'green'
    ax1.annotate(f'{diff:+.1f}%', xy=(x[i]+width/2, s), xytext=(0,5), textcoords='offset points', ha='center', fontsize=8, color=color)

b3 = ax2.bar(x - width/2, unified_p99, width, label='Unified (pool=4096)', color='steelblue', alpha=0.85)
b4 = ax2.bar(x + width/2, separate_p99, width, label='Separate (pool=3072+replay)', color='coral', alpha=0.85)
ax2.set_xlabel('Link Error Rate', fontsize=11)
ax2.set_ylabel('P99 FCT (us)', fontsize=11)
ax2.set_title('P99 FCT (Tail Latency)', fontsize=12)
ax2.set_xticks(x); ax2.set_xticklabels(labels)
ax2.set_yscale('log'); ax2.legend(fontsize=9)
ax2.grid(axis='y', alpha=0.3, which='both'); ax2.set_ylim(100, 20000)
for i, (u, s) in enumerate(zip(unified_p99, separate_p99)):
    diff = (s - u) / u * 100
    color = 'red' if diff > 5 else ('green' if diff < -5 else 'gray')
    ax2.annotate(f'{diff:+.0f}%', xy=(x[i]+width/2, max(s,200)), xytext=(0,5), textcoords='offset points', ha='center', fontsize=8, color=color)

plt.tight_layout()
out = 'mix/output/unified_vs_separate_comparison.png'
plt.savefig(out, dpi=150, bbox_inches='tight')
print(f"Chart saved: {out}")

print("\n======= Summary =======")
print(f"{'ErrRate':>8} {'Uni-avg':>8} {'Sep-avg':>8} {'avg-diff':>9} {'Uni-p99':>8} {'Sep-p99':>8} {'p99-diff':>9}")
print("-"*65)
for i, err in enumerate(error_rates):
    da = (separate_avg[i]-unified_avg[i])/unified_avg[i]*100
    dp = (separate_p99[i]-unified_p99[i])/unified_p99[i]*100
    print(f"{err:>8.5f} {unified_avg[i]:>8.1f} {separate_avg[i]:>8.1f} {da:>+9.1f}% {unified_p99[i]:>8.1f} {separate_p99[i]:>8.1f} {dp:>+9.1f}%")
