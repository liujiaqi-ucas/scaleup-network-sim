#!/usr/bin/env python3
"""
重传包数量分析图
对比 SR vs GBN，维度：traffic × msgsize × BER
生成 4 张图：
  1. allreduce: 绝对重传数 vs BER
  2. alltoall : 绝对重传数 vs BER
  3. GBN/SR 重传比率 vs BER (allreduce + alltoall)
  4. 单位数据量重传率 (retrans/flit) vs BER
"""
import os
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR    = os.path.join(SCRIPT_DIR, "plots", "sr_vs_gbn", "retrans")
os.makedirs(OUT_DIR, exist_ok=True)

BER_VALS    = [1e-15, 1e-14, 1e-13, 1e-12, 1e-11,
               1e-10,  1e-9,  1e-8,  1e-7,  1e-6]
BER_LABELS  = ["1e-15","1e-14","1e-13","1e-12","1e-11",
               "1e-10","1e-9", "1e-8", "1e-7", "1e-6"]
MSG_SIZES   = ["1mb","4mb","16mb","64mb","128mb","256mb"]
SIZE_LABELS = ["1 MB","4 MB","16 MB","64 MB","128 MB","256 MB"]
# 每种消息大小的 flit 数（256 bytes/flit）
SIZE_FLITS  = {
    "1mb":   1*1024*1024//256,
    "4mb":   4*1024*1024//256,
    "16mb": 16*1024*1024//256,
    "64mb": 64*1024*1024//256,
    "128mb":128*1024*1024//256,
    "256mb":256*1024*1024//256,
}

SIZE_COLORS = ["#1976D2","#388E3C","#F57C00","#D32F2F","#7B1FA2","#00838F"]
LS_SR  = '-'
LS_GBN = '--'

# ── 读数据 ────────────────────────────────────────────────────
def load_csv(path):
    """新格式: proto,traffic,size,ber,nflow,biterr,retrans,mean,p99,jct"""
    data = {}
    if not os.path.exists(path):
        return data
    with open(path) as f:
        for line in f:
            p = line.strip().split(',')
            if len(p) < 10:
                continue
            try:
                traffic = p[1]; size = p[2]; ber = float(p[3])
                nflow   = int(p[4])
                retrans = int(p[6])
                data[(traffic, size, ber)] = {
                    'retrans': retrans,
                    'nflow':   nflow,
                    'mean':    float(p[7]),
                    'p99':     float(p[8]),
                    'jct':     float(p[9]),
                }
            except Exception:
                pass
    return data


def get_retrans(data, traffic, size):
    return [data.get((traffic, size, b), {}).get('retrans', np.nan)
            for b in BER_VALS]


def get_retrans_per_flit(data, traffic, size):
    flits = SIZE_FLITS[size]
    nf    = data.get((traffic, size, BER_VALS[0]), {}).get('nflow', 1)
    total = flits * nf  # 理论总 flit 数
    vals  = get_retrans(data, traffic, size)
    return [v / total if not np.isnan(v) else np.nan for v in vals]


sr_path  = os.path.join(SCRIPT_DIR, "sr",  "results.csv")
gbn_path = os.path.join(SCRIPT_DIR, "gbn", "results.csv")
sr  = load_csv(sr_path)
gbn = load_csv(gbn_path)

x = np.arange(len(BER_VALS))

# ═══════════════════════════════════════════════════════════════
# 图1 & 图2: 绝对重传数 vs BER (allreduce / alltoall 各一张)
# ═══════════════════════════════════════════════════════════════
for traffic in ["allreduce", "alltoall"]:
    fig, axes = plt.subplots(1, 2, figsize=(14, 5), sharey=False)
    fig.suptitle(f"Retransmissions vs BER — {traffic}", fontsize=13, fontweight='bold')

    for ax, (data, label, ls) in zip(axes, [(sr, "SR", LS_SR), (gbn, "GBN", LS_GBN)]):
        for i, (sz, slbl, clr) in enumerate(zip(MSG_SIZES, SIZE_LABELS, SIZE_COLORS)):
            y = get_retrans(data, traffic, sz)
            ax.plot(x, y, color=clr, ls=ls, marker='o', ms=4,
                    label=slbl, linewidth=1.8)
        ax.set_title(label, fontsize=11)
        ax.set_yscale('log')
        ax.set_xticks(x)
        ax.set_xticklabels(BER_LABELS, rotation=45, ha='right', fontsize=8)
        ax.set_xlabel("BER")
        ax.set_ylabel("Total retransmitted flits")
        ax.grid(True, which='both', alpha=0.3)
        ax.legend(fontsize=8, ncol=2)

    plt.tight_layout()
    out = os.path.join(OUT_DIR, f"retrans_abs_{traffic}.png")
    fig.savefig(out, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f"saved: {out}")

# ═══════════════════════════════════════════════════════════════
# 图3: GBN/SR 重传比率 vs BER (allreduce + alltoall 并排)
# ═══════════════════════════════════════════════════════════════
fig, axes = plt.subplots(1, 2, figsize=(14, 5), sharey=False)
fig.suptitle("GBN / SR Retransmission Ratio vs BER", fontsize=13, fontweight='bold')

for ax, traffic in zip(axes, ["allreduce", "alltoall"]):
    for i, (sz, slbl, clr) in enumerate(zip(MSG_SIZES, SIZE_LABELS, SIZE_COLORS)):
        sr_v  = np.array(get_retrans(sr,  traffic, sz), dtype=float)
        gbn_v = np.array(get_retrans(gbn, traffic, sz), dtype=float)
        with np.errstate(invalid='ignore', divide='ignore'):
            ratio = np.where((sr_v > 0) & ~np.isnan(sr_v) & ~np.isnan(gbn_v),
                             gbn_v / sr_v, np.nan)
        ax.plot(x, ratio, color=clr, marker='o', ms=4,
                label=slbl, linewidth=1.8)
    ax.axhline(1.0, color='gray', ls=':', linewidth=1, label='ratio=1')
    ax.set_title(traffic, fontsize=11)
    ax.set_xticks(x)
    ax.set_xticklabels(BER_LABELS, rotation=45, ha='right', fontsize=8)
    ax.set_xlabel("BER")
    ax.set_ylabel("GBN retrans / SR retrans")
    ax.set_yscale('log')
    ax.grid(True, which='both', alpha=0.3)
    ax.legend(fontsize=8, ncol=2)

plt.tight_layout()
out = os.path.join(OUT_DIR, "retrans_ratio.png")
fig.savefig(out, dpi=150, bbox_inches='tight')
plt.close(fig)
print(f"saved: {out}")

# ═══════════════════════════════════════════════════════════════
# 图4: 单位 flit 重传率 vs BER (SR vs GBN，allreduce + alltoall)
# ═══════════════════════════════════════════════════════════════
fig, axes = plt.subplots(2, 2, figsize=(14, 10))
fig.suptitle("Retransmission Rate (retrans / total flits) vs BER", fontsize=13, fontweight='bold')

for col, traffic in enumerate(["allreduce", "alltoall"]):
    for row, (data, label) in enumerate([(sr, "SR"), (gbn, "GBN")]):
        ax = axes[row][col]
        for sz, slbl, clr in zip(MSG_SIZES, SIZE_LABELS, SIZE_COLORS):
            y = get_retrans_per_flit(data, traffic, sz)
            ax.plot(x, y, color=clr, marker='o', ms=4,
                    label=slbl, linewidth=1.8)
        ax.set_title(f"{label} — {traffic}", fontsize=10)
        ax.set_yscale('log')
        ax.set_xticks(x)
        ax.set_xticklabels(BER_LABELS, rotation=45, ha='right', fontsize=8)
        ax.set_xlabel("BER")
        ax.set_ylabel("retrans / total flits")
        ax.grid(True, which='both', alpha=0.3)
        ax.legend(fontsize=8, ncol=2)

plt.tight_layout()
out = os.path.join(OUT_DIR, "retrans_rate.png")
fig.savefig(out, dpi=150, bbox_inches='tight')
plt.close(fig)
print(f"saved: {out}")

# ═══════════════════════════════════════════════════════════════
# 图5: 重传数 vs 消息大小（固定若干 BER）
# ═══════════════════════════════════════════════════════════════
KEY_BERS   = [1e-6, 1e-7, 1e-8, 1e-9]
BER_COLORS = ["#D32F2F","#F57C00","#388E3C","#1976D2"]

fig, axes = plt.subplots(1, 2, figsize=(14, 5))
fig.suptitle("Retransmissions vs Message Size (SR vs GBN)", fontsize=13, fontweight='bold')

xs = np.arange(len(MSG_SIZES))
width = 0.35

for ax, traffic in zip(axes, ["allreduce", "alltoall"]):
    for i, (ber, clr) in enumerate(zip(KEY_BERS, BER_COLORS)):
        sr_v  = [sr.get( (traffic, sz, ber), {}).get('retrans', 0) for sz in MSG_SIZES]
        gbn_v = [gbn.get((traffic, sz, ber), {}).get('retrans', 0) for sz in MSG_SIZES]
        offset = (i - len(KEY_BERS)/2 + 0.5) * width / 2
        ax.bar(xs + offset - width/4, sr_v,  width/4, color=clr, alpha=0.9,
               label=f"SR  BER={ber:.0e}")
        ax.bar(xs + offset + width/4, gbn_v, width/4, color=clr, alpha=0.5,
               hatch='//', label=f"GBN BER={ber:.0e}")
    ax.set_title(traffic, fontsize=11)
    ax.set_yscale('log')
    ax.set_xticks(xs)
    ax.set_xticklabels(SIZE_LABELS, fontsize=9)
    ax.set_xlabel("Message Size")
    ax.set_ylabel("Total retransmitted flits")
    ax.grid(True, axis='y', alpha=0.3)
    ax.legend(fontsize=7, ncol=2)

plt.tight_layout()
out = os.path.join(OUT_DIR, "retrans_vs_size.png")
fig.savefig(out, dpi=150, bbox_inches='tight')
plt.close(fig)
print(f"saved: {out}")

# ── 文字摘要 ─────────────────────────────────────────────────
print("\n── GBN/SR 重传比率摘要（256mb，高BER）──")
for traffic in ["allreduce", "alltoall"]:
    for ber in [1e-6, 1e-7, 1e-8]:
        sv = sr.get( (traffic,"256mb",ber),{}).get('retrans',0)
        gv = gbn.get((traffic,"256mb",ber),{}).get('retrans',0)
        ratio = gv/sv if sv > 0 else float('nan')
        print(f"  {traffic:10s} BER={ber:.0e}  SR={sv:>10,}  GBN={gv:>10,}  ratio={ratio:.2f}x")
