#!/usr/bin/env python3
"""
H100_8 BER 对比图: SR vs GBN
6消息大小 × 2流量类型 × 3指标 (Mean FCT / P99 FCT / JCT)
"""
import csv, os, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SR_CSV  = os.path.join(SCRIPT_DIR, "sr",  "results.csv")
GBN_CSV = os.path.join(SCRIPT_DIR, "gbn", "results.csv")
OUT_DIR = os.path.join(SCRIPT_DIR, "plots")
os.makedirs(OUT_DIR, exist_ok=True)

BER_VALS = [1e-15, 1e-14, 1e-13, 1e-12, 1e-11,
            1e-10, 1e-9,  1e-8,  1e-7,  1e-6]
MSG_SIZES   = ["1mb", "4mb", "16mb", "64mb", "128mb", "256mb"]
SIZE_LABELS = ["1 MB", "4 MB", "16 MB", "64 MB", "128 MB", "256 MB"]
TRAFFICS    = ["allreduce", "alltoall"]
TRAFFIC_LABELS = {"allreduce": "Allreduce", "alltoall": "Alltoall"}

def load_csv(path):
    data = {}
    if not os.path.exists(path):
        print(f"WARNING: {path} not found")
        return data
    with open(path) as f:
        for line in f:
            parts = line.strip().split(',')
            if len(parts) < 9:
                continue
            try:
                key = (parts[1], parts[2], float(parts[3]))
                data[key] = {
                    'mean':   float(parts[6]),
                    'p99':    float(parts[7]),
                    'jct':    float(parts[8]),
                    'flows':  int(parts[4]),
                    'errors': int(parts[5]),
                }
            except Exception:
                pass
    return data

def get_vals(data, traffic, msgsize, metric):
    return [data.get((traffic, msgsize, er), {}).get(metric, np.nan)
            for er in BER_VALS]

# ─── 图1/2: 综合对比 (每种流量一张, 3行×6列) ─────────────────────────────────
def plot_combined(traffic, sr, gbn):
    ncols = len(MSG_SIZES)
    fig, axes = plt.subplots(3, ncols, figsize=(ncols * 4, 11))
    fig.suptitle(
        f"{TRAFFIC_LABELS[traffic]} — SR vs GBN  (H100 8-GPU, BER错误率)",
        fontsize=14, fontweight='bold')

    metrics = [('p99', 'P99 FCT (μs)'), ('mean', 'Mean FCT (μs)'), ('jct', 'JCT (μs)')]
    for row, (metric, ylabel) in enumerate(metrics):
        for col, (ms, slabel) in enumerate(zip(MSG_SIZES, SIZE_LABELS)):
            ax = axes[row][col]
            sr_v  = get_vals(sr,  traffic, ms, metric)
            gbn_v = get_vals(gbn, traffic, ms, metric)

            ax.plot(BER_VALS, sr_v,  'o-', color='#FF5722', lw=2, ms=6, label='SR')
            ax.plot(BER_VALS, gbn_v, 's-', color='#2196F3', lw=2, ms=6, label='GBN')

            all_v = [v for v in sr_v + gbn_v if not np.isnan(v) and v > 0]
            if all_v:
                ax.set_ylim(min(all_v) * 0.88, max(all_v) * 1.12)

            ax.set_xscale('log')
            if row == 2:
                ax.set_xlabel('BER', fontsize=10)
                ax.set_xticks(BER_VALS)
                ax.set_xticklabels([f'{e:.0e}' for e in BER_VALS],
                                   fontsize=6, rotation=40, ha='right')
            else:
                ax.set_xticks(BER_VALS)
                ax.set_xticklabels([])
            if row == 0:
                ax.set_title(slabel, fontsize=12, fontweight='bold')
            if col == 0:
                ax.set_ylabel(ylabel, fontsize=10)
            ax.legend(fontsize=8, loc='upper left')
            ax.grid(True, alpha=0.3)

    plt.tight_layout()
    path = os.path.join(OUT_DIR, f"h100_ber_{traffic}.png")
    plt.savefig(path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  -> {path}")

# ─── 图3: SR/GBN 加速比 ────────────────────────────────────────────────────────
def plot_ratio(sr, gbn):
    colors = ['#1976D2', '#388E3C', '#F57C00', '#D32F2F', '#7B1FA2', '#00838F']
    fig, axes = plt.subplots(2, 3, figsize=(18, 8))
    fig.suptitle("SR / GBN Ratio — H100 8-GPU BER  (< 1 表示 SR 更快)",
                 fontsize=14, fontweight='bold')

    for row, traffic in enumerate(TRAFFICS):
        for col, (metric, mlabel) in enumerate(
                [('mean', 'Mean FCT'), ('p99', 'P99 FCT'), ('jct', 'JCT')]):
            ax = axes[row][col]
            for i, (ms, slabel) in enumerate(zip(MSG_SIZES, SIZE_LABELS)):
                sr_v  = get_vals(sr,  traffic, ms, metric)
                gbn_v = get_vals(gbn, traffic, ms, metric)
                ratio = [s/g if g > 0 and not np.isnan(s) and not np.isnan(g)
                         else np.nan for s, g in zip(sr_v, gbn_v)]
                ax.plot(BER_VALS, ratio, 'o-', color=colors[i],
                        lw=2, ms=6, label=slabel)

            ax.axhline(y=1.0, color='gray', ls='--', lw=1.2, alpha=0.6)
            ax.set_xscale('log')
            ax.set_xlabel('BER', fontsize=10)
            ax.set_xticks(BER_VALS)
            ax.set_xticklabels([f'{e:.0e}' for e in BER_VALS],
                               fontsize=6, rotation=40, ha='right')
            if col == 0:
                ax.set_ylabel(f'SR/GBN ({TRAFFIC_LABELS[traffic]})', fontsize=10)
            ax.set_title(f"{TRAFFIC_LABELS[traffic]} — {mlabel}",
                         fontsize=11, fontweight='bold')
            ax.legend(fontsize=7, ncol=2)
            ax.grid(True, alpha=0.3)

    plt.tight_layout()
    path = os.path.join(OUT_DIR, "h100_ber_ratio.png")
    plt.savefig(path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  -> {path}")

# ─── 汇总表 ────────────────────────────────────────────────────────────────────
def print_summary(sr, gbn):
    print("\n" + "="*100)
    print("SR vs GBN 汇总 (H100_8 BER, Δ% = (SR-GBN)/GBN×100, 负数=SR更快)")
    print("="*100)
    for traffic in TRAFFICS:
        print(f"\n--- {TRAFFIC_LABELS[traffic]} ---")
        print(f"{'Size':<7} {'BER':<12} {'SR mean':>10} {'GBN mean':>10} {'Δ%':>7}  "
              f"{'SR p99':>10} {'GBN p99':>10} {'Δ%':>7}  "
              f"{'SR jct':>10} {'GBN jct':>10} {'Δ%':>7}")
        print("-"*100)
        for ms in MSG_SIZES:
            for er in BER_VALS:
                s = sr.get((traffic, ms, er), {})
                g = gbn.get((traffic, ms, er), {})
                sm, gm = s.get('mean', 0), g.get('mean', 0)
                sp, gp = s.get('p99',  0), g.get('p99',  0)
                sj, gj = s.get('jct',  0), g.get('jct',  0)
                dm = (sm-gm)/gm*100 if gm > 0 else 0
                dp = (sp-gp)/gp*100 if gp > 0 else 0
                dj = (sj-gj)/gj*100 if gj > 0 else 0
                print(f"{ms:<7} {er:<12.2e} "
                      f"{sm:>10.1f} {gm:>10.1f} {dm:>+6.1f}%  "
                      f"{sp:>10.1f} {gp:>10.1f} {dp:>+6.1f}%  "
                      f"{sj:>10.1f} {gj:>10.1f} {dj:>+6.1f}%")
    print("="*100)

if __name__ == '__main__':
    print("加载数据...")
    sr  = load_csv(SR_CSV)
    gbn = load_csv(GBN_CSV)
    print(f"SR: {len(sr)} 条  GBN: {len(gbn)} 条")

    if not sr and not gbn:
        print("ERROR: 无数据"); sys.exit(1)

    print("\n生成图表...")
    for t in TRAFFICS:
        plot_combined(t, sr, gbn)
    plot_ratio(sr, gbn)
    print_summary(sr, gbn)
    print("\n完成!")
