#!/usr/bin/env python3
"""
H100_8 BER 实验对比图
对比: SR vs GBN | SR vs PFC | SR vs Separate (有数据时)
每个对比生成 2 张图 (allreduce + alltoall)
每张图: 3行(Mean/P99/JCT) × 6列(消息大小), BER 为 x 轴
"""
import os, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR    = os.path.join(SCRIPT_DIR, "plots")
os.makedirs(OUT_DIR, exist_ok=True)

BER_VALS = [1e-15, 1e-14, 1e-13, 1e-12, 1e-11,
            1e-10,  1e-9,  1e-8,  1e-7,  1e-6]
MSG_SIZES   = ["1mb","4mb","16mb","64mb","128mb","256mb"]
SIZE_LABELS = ["1 MB","4 MB","16 MB","64 MB","128 MB","256 MB"]
TRAFFICS    = ["allreduce","alltoall"]
METRICS     = [("mean","Mean FCT (μs)"),("p99","P99 FCT (μs)"),("jct","JCT (μs)")]

# ── 配色：6 种消息大小 ──────────────────────────────────────
SIZE_COLORS = ["#1976D2","#388E3C","#F57C00","#D32F2F","#7B1FA2","#00838F"]


def load_csv(path, proto_override=None):
    """加载 CSV，兼容两种格式：
      旧格式(9列): proto,traffic,size,ber,nflow,biterr,mean,p99,jct
      新格式(10列): proto,traffic,size,ber,nflow,biterr,retrans,mean,p99,jct
    """
    data = {}
    if not os.path.exists(path):
        return data
    with open(path) as f:
        for line in f:
            p = line.strip().split(',')
            if len(p) < 9:
                continue
            try:
                traffic = p[1]
                msgsize = p[2]
                ber     = float(p[3])
                # 10列新格式有retrans列，mean/p99/jct后移一位
                off = 1 if len(p) >= 10 else 0
                data[(traffic, msgsize, ber)] = {
                    'mean': float(p[6 + off]),
                    'p99':  float(p[7 + off]),
                    'jct':  float(p[8 + off]),
                }
            except Exception:
                pass
    return data


def get_vals(data, traffic, msgsize, metric):
    return [data.get((traffic, msgsize, er), {}).get(metric, np.nan)
            for er in BER_VALS]


def plot_one_metric(data_a, data_b, label_a, label_b,
                    color_a, color_b, traffic, metric, ylabel, out_path):
    """单行图：固定 traffic + metric，6 列消息大小"""
    from matplotlib.lines import Line2D
    ncols = len(MSG_SIZES)
    fig, axes = plt.subplots(1, ncols, figsize=(ncols * 3.2, 3.2))
    fig.suptitle(f"{label_a} vs {label_b}  ·  {traffic.capitalize()}  ·  {ylabel}  (H100 8-GPU)",
                 fontsize=11, fontweight='bold')

    for col, (ms, slabel) in enumerate(zip(MSG_SIZES, SIZE_LABELS)):
        ax = axes[col]
        va = get_vals(data_a, traffic, ms, metric)
        vb = get_vals(data_b, traffic, ms, metric)

        ax.plot(BER_VALS, va, 'o-',  color=color_a, lw=2, ms=6, label=label_a)
        ax.plot(BER_VALS, vb, 's--', color=color_b, lw=2, ms=6, label=label_b)

        all_v = [v for v in va+vb if not np.isnan(v) and v > 0]
        if all_v:
            ax.set_ylim(min(all_v)*0.85, max(all_v)*1.15)

        ax.set_xscale('log')
        ax.set_xlim(min(BER_VALS), max(BER_VALS))
        ax.set_xticks(BER_VALS)
        ax.set_xticklabels([f'{e:.0e}' for e in BER_VALS],
                           fontsize=6, rotation=40, ha='right')
        ax.set_xlabel('BER', fontsize=9)
        if col == 0:
            ax.set_ylabel(ylabel, fontsize=9)
        ax.set_title(slabel, fontsize=10, fontweight='bold')
        ax.grid(True, alpha=0.3)

    legend_elements = [
        Line2D([0],[0], color=color_a, marker='o', ls='-',  lw=2, ms=7, label=label_a),
        Line2D([0],[0], color=color_b, marker='s', ls='--', lw=2, ms=7, label=label_b),
    ]
    fig.legend(handles=legend_elements, loc='lower center',
               ncol=2, fontsize=10, frameon=True, bbox_to_anchor=(0.5, -0.12))

    plt.tight_layout()
    fig.savefig(out_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  → {out_path}")


def plot_ratio_one_metric(data_a, data_b, label_a, label_b,
                          traffic, metric, ylabel, out_path):
    """单行比值图：B/A，>1 表示 A 更快"""
    ncols = len(MSG_SIZES)
    fig, axes = plt.subplots(1, ncols, figsize=(ncols * 3.2, 3.0))
    fig.suptitle(f"Ratio {label_b}/{label_a}  ·  {traffic.capitalize()}  ·  {ylabel}"
                 f"  (> 1 ⟹ {label_a} faster)",
                 fontsize=11, fontweight='bold')

    for col, (ms, slabel) in enumerate(zip(MSG_SIZES, SIZE_LABELS)):
        ax = axes[col]
        va = get_vals(data_a, traffic, ms, metric)
        vb = get_vals(data_b, traffic, ms, metric)
        ratio = [b/a if a > 0 and not np.isnan(a) and not np.isnan(b)
                 else np.nan for a, b in zip(va, vb)]

        ax.plot(BER_VALS, ratio, 'D-', color='#333333', lw=2, ms=6)
        ax.axhline(1.0, color='#CC0000', ls='--', lw=1.2, alpha=0.8)

        ax.set_xscale('log')
        ax.set_xlim(min(BER_VALS), max(BER_VALS))
        ax.set_xticks(BER_VALS)
        ax.set_xticklabels([f'{e:.0e}' for e in BER_VALS],
                           fontsize=6, rotation=40, ha='right')
        ax.set_xlabel('BER', fontsize=9)
        if col == 0:
            ax.set_ylabel('Ratio', fontsize=9)
        ax.set_title(slabel, fontsize=10, fontweight='bold')
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    fig.savefig(out_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  → {out_path}")


# ══════════════════════════════════════════════════════════════════
# 主逻辑
# ══════════════════════════════════════════════════════════════════
if __name__ == '__main__':
    # 加载数据（用目录名判断协议，忽略 CSV 里的协议列）
    SR  = load_csv(os.path.join(SCRIPT_DIR, "sr",       "results.csv"))
    GBN = load_csv(os.path.join(SCRIPT_DIR, "gbn",      "results.csv"))
    PFC = load_csv(os.path.join(SCRIPT_DIR, "PFC",      "results.csv"))
    SEP = load_csv(os.path.join(SCRIPT_DIR, "separate",  "results.csv"))

    print(f"数据行数: SR={len(SR)}, GBN={len(GBN)}, PFC={len(PFC)}, SEP={len(SEP)}")

    comparisons = []

    if SR and GBN:
        comparisons.append((SR, GBN, "SR (CBFC)", "GBN (CBFC)",
                            "#1976D2", "#D32F2F", "sr_vs_gbn"))
    if SR and PFC:
        comparisons.append((SR, PFC, "SR (CBFC)", "SR (PFC)",
                            "#1976D2", "#F57C00", "sr_vs_pfc"))
    if SR and SEP:
        comparisons.append((SR, SEP, "Unified (SR)", "Separate (+25%)",
                            "#1976D2", "#388E3C", "unified_vs_separate"))

    metric_names = {"mean": "mean_fct", "p99": "p99_fct", "jct": "jct"}

    for (da, db, la, lb, ca, cb, tag) in comparisons:
        print(f"\n生成对比图: {la} vs {lb}")
        for traffic in TRAFFICS:
            # 每个 traffic 单独一个子文件夹
            sub_dir = os.path.join(OUT_DIR, tag, traffic)
            os.makedirs(sub_dir, exist_ok=True)
            ratio_dir = os.path.join(OUT_DIR, tag, traffic, "ratio")
            os.makedirs(ratio_dir, exist_ok=True)

            for metric, ylabel in METRICS:
                mname = metric_names[metric]
                # 绝对值图
                plot_one_metric(da, db, la, lb, ca, cb, traffic,
                                metric, ylabel,
                                os.path.join(sub_dir, f"{mname}.png"))
                # 比值图
                plot_ratio_one_metric(da, db, la, lb, traffic,
                                      metric, ylabel,
                                      os.path.join(ratio_dir, f"{mname}_ratio.png"))

    # ── 存储效率图：仅 unified vs separate 有意义 ──────────────────
    if SR and SEP:
        print("\n生成存储效率图: Unified vs Separate (+25%)")
        # 消息大小 → 字节
        SIZE_BYTES = {"1mb":   1*1024*1024,
                      "4mb":   4*1024*1024,
                      "16mb":  16*1024*1024,
                      "64mb":  64*1024*1024,
                      "128mb": 128*1024*1024,
                      "256mb": 256*1024*1024}
        STORAGE_UNIFIED  = 5376   # flit (H100_8, pool=4096 + rxbuf=1280)
        STORAGE_SEPARATE = 6720   # flit (+25%)

        for traffic in TRAFFICS:
            eff_dir = os.path.join(OUT_DIR, "unified_vs_separate", traffic)
            os.makedirs(eff_dir, exist_ok=True)

            ncols = len(MSG_SIZES)
            fig, axes = plt.subplots(1, ncols, figsize=(ncols * 3.2, 3.4))
            fig.suptitle(
                f"Storage Efficiency (algBW / total_storage)  ·  {traffic.capitalize()}  ·  H100 8-GPU\n"
                f"Unified: {STORAGE_UNIFIED} flit  vs  Separate +25%: {STORAGE_SEPARATE} flit",
                fontsize=10, fontweight='bold')

            from matplotlib.lines import Line2D
            for col, (ms, slabel) in enumerate(zip(MSG_SIZES, SIZE_LABELS)):
                ax = axes[col]
                nbytes = SIZE_BYTES[ms]
                jct_sr  = get_vals(SR,  traffic, ms, "jct")
                jct_sep = get_vals(SEP, traffic, ms, "jct")

                # algBW (GB/s) = msg_bytes / JCT_us / 1e3
                # efficiency (MB/s per flit) = algBW / storage_flit
                eff_sr  = [nbytes / (j*1e-6) / STORAGE_UNIFIED  / 1e6
                           if not np.isnan(j) and j > 0 else np.nan
                           for j in jct_sr]
                eff_sep = [nbytes / (j*1e-6) / STORAGE_SEPARATE / 1e6
                           if not np.isnan(j) and j > 0 else np.nan
                           for j in jct_sep]

                ax.plot(BER_VALS, eff_sr,  'o-',  color="#1976D2", lw=2, ms=6, label="Unified")
                ax.plot(BER_VALS, eff_sep, 's--', color="#388E3C", lw=2, ms=6, label="Separate +25%")

                all_v = [v for v in eff_sr+eff_sep if not np.isnan(v) and v > 0]
                if all_v:
                    ax.set_ylim(min(all_v)*0.85, max(all_v)*1.15)

                ax.set_xscale('log')
                ax.set_xlim(min(BER_VALS), max(BER_VALS))
                ax.set_xticks(BER_VALS)
                ax.set_xticklabels([f'{e:.0e}' for e in BER_VALS],
                                   fontsize=6, rotation=40, ha='right')
                ax.set_xlabel('BER', fontsize=9)
                if col == 0:
                    ax.set_ylabel('Efficiency\n(MB/s per flit)', fontsize=8)
                ax.set_title(slabel, fontsize=10, fontweight='bold')
                ax.grid(True, alpha=0.3)

            legend_elements = [
                Line2D([0],[0], color="#1976D2", marker='o', ls='-',  lw=2, ms=7,
                       label=f"Unified ({STORAGE_UNIFIED} flit)"),
                Line2D([0],[0], color="#388E3C", marker='s', ls='--', lw=2, ms=7,
                       label=f"Separate +25% ({STORAGE_SEPARATE} flit)"),
            ]
            fig.legend(handles=legend_elements, loc='lower center',
                       ncol=2, fontsize=9, frameon=True, bbox_to_anchor=(0.5, -0.12))
            plt.tight_layout()
            out = os.path.join(eff_dir, "storage_efficiency.png")
            fig.savefig(out, dpi=150, bbox_inches='tight')
            plt.close()
            print(f"  → {out}")

    print("\n全部完成！")
