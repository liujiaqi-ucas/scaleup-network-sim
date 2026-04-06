#!/usr/bin/env python3
"""
SR vs GBN 对比实验脚本
用法:
  python3 run_comparison_experiments.py --protocol gbn   # 在 feature/cbfc-gbn 分支运行
  python3 run_comparison_experiments.py --protocol sr    # 在 feature/dynamic-alpha 分支运行
  python3 run_comparison_experiments.py --plot           # 两者都跑完后生成对比图
"""
import os, sys, subprocess, argparse, json, time
from pathlib import Path

TOPO      = "NVL72_72_800G_OS2"  # 完整NVL72：72GPU×18Switch，800Gbps/100ns
TOPO_FILE = "config/NVL72_72_800G_OS2.txt"
NGPUS     = 72
BW        = "800"      # NVLink带宽 800Gbps
DATA_MB   = 1          # 总量1MB → 每流14KB/59flits，每次仿真约20分钟
                       # AllReduce有142步太慢，只跑AlltoAll
SIMUL_TIME = "0.1"
ERROR_RATES = [0.0001, 0.001]  # 0.00001已由timing test提供
TRAFFICS    = ["alltoall"]   # 跳过AllReduce(142步×72流=10224流，耗时过长)
RESULT_DIR  = "comparison_results_nvl72"

os.makedirs(RESULT_DIR, exist_ok=True)

# ─── 工具函数 ──────────────────────────────────────────────

def set_error_rate(rate: float):
    """修改拓扑文件中的链路错误率"""
    lines = open(TOPO_FILE).readlines()
    new_lines = []
    for line in lines:
        parts = line.strip().split()
        # 链路行格式: src dst bw delay error_rate
        if len(parts) == 5 and parts[2].endswith("Gbps"):
            parts[4] = str(rate)
            new_lines.append(" ".join(parts) + "\n")
        else:
            new_lines.append(line)
    open(TOPO_FILE, "w").writelines(new_lines)
    print(f"  [设置] error_rate={rate}")

def set_traffic(traffic: str, run_py="run.py"):
    """修改 run.py 中的 target_flow_name"""
    content = open(run_py).read()
    import re
    content = re.sub(r'target_flow_name\s*=\s*"[^"]*"',
                     f'target_flow_name = "flow_{traffic}"', content)
    open(run_py, "w").write(content)
    print(f"  [设置] traffic={traffic}")

def gen_traffic(ngpus: int, size_mb: float):
    subprocess.run(["python3", "gen_scaleup_traffic.py",
                    "-n", str(ngpus), "-s", str(size_mb)],
                   check=True, capture_output=True)

def run_sim(protocol: str, traffic: str, error_rate: float):
    """运行一次仿真，返回输出目录路径"""
    tag = f"{protocol}_{traffic}_err{error_rate:.5f}".replace(".", "p")
    print(f"  [运行] {tag} ...")
    t0 = time.time()
    cmd = ["python3", "run.py", "--topo", TOPO, "--simul_time", SIMUL_TIME]
    if BW != "200":  # 非默认带宽时传入--bw参数
        cmd += ["--bw", BW]
    result = subprocess.run(cmd, capture_output=True, text=True)
    elapsed = time.time() - t0

    # 找最新的输出目录
    out_dirs = sorted(Path("mix/output").iterdir(), key=lambda p: p.stat().st_mtime, reverse=True)
    latest = str(out_dirs[0]) if out_dirs else ""
    # 找FCT文件
    fct_files = list(Path(latest).glob("*_out_fct.txt")) if latest else []
    fct_path = str(fct_files[0]) if fct_files else ""

    n_flows = sum(1 for _ in open(fct_path)) if fct_path and os.path.getsize(fct_path) > 0 else 0
    print(f"  [完成] {tag}  flows={n_flows}  耗时={elapsed:.1f}s")

    # 保存元数据
    meta = {
        "protocol": protocol,
        "traffic": traffic,
        "error_rate": error_rate,
        "fct_file": fct_path,
        "n_flows": n_flows,
        "elapsed": elapsed,
    }
    meta_file = f"{RESULT_DIR}/{tag}.json"
    json.dump(meta, open(meta_file, "w"), indent=2)
    return meta


# ─── 主逻辑 ──────────────────────────────────────────────

def run_protocol(protocol: str):
    print(f"\n{'='*60}")
    print(f"  开始运行 {protocol.upper()} 实验组 (共 {len(ERROR_RATES)*len(TRAFFICS)} 组)")
    print(f"{'='*60}")

    # 生成流量文件（只需一次）
    gen_traffic(NGPUS, DATA_MB)
    print(f"  [已生成] {NGPUS}GPU {DATA_MB}MB 流量文件")

    results = []
    for traffic in TRAFFICS:
        set_traffic(traffic)
        for err in ERROR_RATES:
            set_error_rate(err)
            meta = run_sim(protocol, traffic, err)
            results.append(meta)

    # 保存该协议的汇总
    json.dump(results, open(f"{RESULT_DIR}/{protocol}_summary.json", "w"), indent=2)
    print(f"\n  [{protocol.upper()}] 全部完成，结果保存至 {RESULT_DIR}/")
    return results


def plot_results():
    """读取 SR 和 GBN 的结果，生成对比图"""
    import pandas as pd
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.patches as mpatches
    import numpy as np

    # ── 加载数据 ──────────────────────────────────────────
    all_rows = []
    for proto_file in [f"{RESULT_DIR}/sr_summary.json", f"{RESULT_DIR}/gbn_summary.json"]:
        if not os.path.exists(proto_file):
            print(f"  缺少 {proto_file}，跳过")
            continue
        for meta in json.load(open(proto_file)):
            if not meta["fct_file"] or not os.path.exists(meta["fct_file"]):
                continue
            try:
                df = pd.read_csv(meta["fct_file"], sep=" ", header=None,
                                 names=["src","dst","sport","dport","size","startTs","fct","stepId"])
                df["protocol"]   = meta["protocol"].upper()
                df["traffic"]    = meta["traffic"]
                df["error_rate"] = meta["error_rate"]
                df["fct_us"]     = df["fct"] / 1000.0   # ns → μs
                all_rows.append(df)
            except Exception as e:
                print(f"  读取 {meta['fct_file']} 失败: {e}")

    if not all_rows:
        print("没有可用数据，退出")
        return

    data = pd.concat(all_rows, ignore_index=True)

    # ── 图表配置 ──────────────────────────────────────────
    COLORS   = {"SR": "#2196F3", "GBN": "#FF5722"}
    ERR_LBLS = {0.00001: "1e-5", 0.0001: "1e-4", 0.001: "1e-3"}
    traffics = sorted(data["traffic"].unique())
    err_rates = sorted(data["error_rate"].unique())

    fig = plt.figure(figsize=(22, 26))
    fig.suptitle("CBFC+SR vs CBFC+GBN\nH100 8-GPU AlltoAll & AllReduce (8MB) - FCT 对比",
                 fontsize=16, fontweight="bold", y=0.99)

    panel_row = 0

    # ════════════════════════════════════════════════════════
    # 图1-2: 均值 FCT 柱状图（alltoall & allreduce）
    # ════════════════════════════════════════════════════════
    for col_idx, traffic in enumerate(traffics):
        ax = fig.add_subplot(5, 2, panel_row*2 + col_idx + 1)
        sub = data[data["traffic"] == traffic]
        x = np.arange(len(err_rates))
        width = 0.35
        for pi, proto in enumerate(["SR", "GBN"]):
            means = [sub[(sub["protocol"]==proto) & (sub["error_rate"]==e)]["fct_us"].mean()
                     for e in err_rates]
            stds  = [sub[(sub["protocol"]==proto) & (sub["error_rate"]==e)]["fct_us"].std()
                     for e in err_rates]
            bars = ax.bar(x + pi*width, means, width, label=proto,
                          color=COLORS[proto], alpha=0.85,
                          yerr=stds, capsize=4, error_kw={"elinewidth":1.5})
            # 数值标注
            for rect, m in zip(bars, means):
                if not np.isnan(m):
                    ax.text(rect.get_x()+rect.get_width()/2., rect.get_height()+max(stds)*0.05,
                            f"{m:.0f}", ha="center", va="bottom", fontsize=8, fontweight="bold")
        ax.set_xticks(x + width/2)
        ax.set_xticklabels([ERR_LBLS[e] for e in err_rates])
        ax.set_xlabel("链路错误率", fontsize=10)
        ax.set_ylabel("平均 FCT (μs)", fontsize=10)
        ax.set_title(f"{'AlltoAll' if traffic=='alltoall' else 'AllReduce'} - 平均 FCT", fontsize=11)
        ax.legend(fontsize=10)
        ax.grid(axis="y", alpha=0.3)
    panel_row += 1

    # ════════════════════════════════════════════════════════
    # 图3-4: 最大 FCT（Tail Latency）
    # ════════════════════════════════════════════════════════
    for col_idx, traffic in enumerate(traffics):
        ax = fig.add_subplot(5, 2, panel_row*2 + col_idx + 1)
        sub = data[data["traffic"] == traffic]
        x = np.arange(len(err_rates))
        for pi, proto in enumerate(["SR", "GBN"]):
            p99s = [sub[(sub["protocol"]==proto) & (sub["error_rate"]==e)]["fct_us"].quantile(0.99)
                    for e in err_rates]
            maxes = [sub[(sub["protocol"]==proto) & (sub["error_rate"]==e)]["fct_us"].max()
                     for e in err_rates]
            ax.plot(x, p99s,  "o-", color=COLORS[proto], label=f"{proto} P99", linewidth=2, markersize=7)
            ax.plot(x, maxes, "s--", color=COLORS[proto], label=f"{proto} Max", linewidth=1.5, markersize=6, alpha=0.7)
        ax.set_xticks(x)
        ax.set_xticklabels([ERR_LBLS[e] for e in err_rates])
        ax.set_xlabel("链路错误率", fontsize=10)
        ax.set_ylabel("FCT (μs)", fontsize=10)
        ax.set_title(f"{'AlltoAll' if traffic=='alltoall' else 'AllReduce'} - Tail Latency (P99 & Max)", fontsize=11)
        ax.legend(fontsize=9)
        ax.grid(alpha=0.3)
    panel_row += 1

    # ════════════════════════════════════════════════════════
    # 图5-6: CDF（error_rate=0.0001 下的FCT分布）
    # ════════════════════════════════════════════════════════
    target_err = 0.0001
    for col_idx, traffic in enumerate(traffics):
        ax = fig.add_subplot(5, 2, panel_row*2 + col_idx + 1)
        for proto in ["SR", "GBN"]:
            sub = data[(data["traffic"]==traffic) & (data["protocol"]==proto) &
                       (data["error_rate"]==target_err)]["fct_us"].dropna()
            if len(sub) == 0: continue
            sorted_vals = np.sort(sub)
            cdf = np.arange(1, len(sorted_vals)+1) / len(sorted_vals)
            ax.plot(sorted_vals, cdf, color=COLORS[proto], label=proto, linewidth=2)
        ax.set_xlabel("FCT (μs)", fontsize=10)
        ax.set_ylabel("CDF", fontsize=10)
        ax.set_title(f"{'AlltoAll' if traffic=='alltoall' else 'AllReduce'} - CDF (err=1e-4)", fontsize=11)
        ax.legend(fontsize=10)
        ax.grid(alpha=0.3)
        ax.set_ylim(0, 1.05)
    panel_row += 1

    # ════════════════════════════════════════════════════════
    # 图7-8: Box Plot（各错误率下FCT分布）
    # ════════════════════════════════════════════════════════
    for col_idx, traffic in enumerate(traffics):
        ax = fig.add_subplot(5, 2, panel_row*2 + col_idx + 1)
        box_data, box_labels, box_colors = [], [], []
        for err in err_rates:
            for proto in ["SR", "GBN"]:
                vals = data[(data["traffic"]==traffic) & (data["protocol"]==proto) &
                            (data["error_rate"]==err)]["fct_us"].dropna().values
                box_data.append(vals)
                box_labels.append(f"{ERR_LBLS[err]}\n{proto}")
                box_colors.append(COLORS[proto])
        bp = ax.boxplot(box_data, labels=box_labels, patch_artist=True,
                        medianprops={"color":"black","linewidth":2})
        for patch, color in zip(bp["boxes"], box_colors):
            patch.set_facecolor(color)
            patch.set_alpha(0.7)
        ax.set_xlabel("错误率 / 协议", fontsize=9)
        ax.set_ylabel("FCT (μs)", fontsize=10)
        ax.set_title(f"{'AlltoAll' if traffic=='alltoall' else 'AllReduce'} - FCT Box Plot", fontsize=11)
        ax.grid(axis="y", alpha=0.3)
        # 图例
        patches = [mpatches.Patch(color=COLORS[p], label=p) for p in ["SR","GBN"]]
        ax.legend(handles=patches, fontsize=9)
    panel_row += 1

    # ════════════════════════════════════════════════════════
    # 图9-10: GBN/SR FCT 比值热力图
    # ════════════════════════════════════════════════════════
    for col_idx, traffic in enumerate(traffics):
        ax = fig.add_subplot(5, 2, panel_row*2 + col_idx + 1)
        metrics = ["Mean", "Median", "P99", "Max"]
        ratio_matrix = np.zeros((len(metrics), len(err_rates)))
        for ei, err in enumerate(err_rates):
            for mi, metric in enumerate(metrics):
                sub_sr  = data[(data["traffic"]==traffic) & (data["protocol"]=="SR")  & (data["error_rate"]==err)]["fct_us"]
                sub_gbn = data[(data["traffic"]==traffic) & (data["protocol"]=="GBN") & (data["error_rate"]==err)]["fct_us"]
                if len(sub_sr)==0 or len(sub_gbn)==0:
                    ratio_matrix[mi, ei] = np.nan
                    continue
                funcs = {"Mean": np.mean, "Median": np.median,
                         "P99": lambda x: np.quantile(x,0.99), "Max": np.max}
                sr_val  = funcs[metric](sub_sr)
                gbn_val = funcs[metric](sub_gbn)
                ratio_matrix[mi, ei] = gbn_val / sr_val if sr_val > 0 else np.nan

        im = ax.imshow(ratio_matrix, cmap="RdYlGn_r", vmin=0.8, vmax=1.5, aspect="auto")
        ax.set_xticks(range(len(err_rates)))
        ax.set_xticklabels([ERR_LBLS[e] for e in err_rates])
        ax.set_yticks(range(len(metrics)))
        ax.set_yticklabels(metrics)
        for i in range(len(metrics)):
            for j in range(len(err_rates)):
                val = ratio_matrix[i, j]
                if not np.isnan(val):
                    color = "white" if val > 1.3 or val < 0.85 else "black"
                    ax.text(j, i, f"{val:.2f}×", ha="center", va="center",
                            fontsize=10, fontweight="bold", color=color)
        plt.colorbar(im, ax=ax, label="GBN/SR 比值")
        ax.set_xlabel("链路错误率", fontsize=10)
        ax.set_title(f"{'AlltoAll' if traffic=='alltoall' else 'AllReduce'}\n"
                     f"GBN/SR FCT 比值热力图 (<1.0=GBN更优)", fontsize=10)

    plt.tight_layout(rect=[0, 0, 1, 0.98])
    out_file = f"{RESULT_DIR}/sr_vs_gbn_comparison.png"
    plt.savefig(out_file, dpi=150, bbox_inches="tight")
    print(f"\n[完成] 对比图已保存: {out_file}")
    plt.close()

    # ── 打印汇总表格 ──────────────────────────────────────
    print("\n" + "="*70)
    print("  SR vs GBN FCT 对比汇总（单位：μs）")
    print("="*70)
    for traffic in traffics:
        print(f"\n  【{traffic.upper()}】")
        print(f"  {'错误率':<10} {'协议':<6} {'Mean':>8} {'Median':>8} {'P99':>8} {'Max':>8} {'n':>5}")
        print(f"  {'-'*56}")
        for err in err_rates:
            for proto in ["SR", "GBN"]:
                sub = data[(data["traffic"]==traffic) & (data["protocol"]==proto) &
                           (data["error_rate"]==err)]["fct_us"]
                if len(sub) == 0:
                    print(f"  {ERR_LBLS[err]:<10} {proto:<6} {'N/A':>8}")
                    continue
                print(f"  {ERR_LBLS[err]:<10} {proto:<6} "
                      f"{sub.mean():>8.1f} {sub.median():>8.1f} "
                      f"{sub.quantile(0.99):>8.1f} {sub.max():>8.1f} {len(sub):>5}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--protocol", choices=["sr","gbn"], help="运行指定协议的实验")
    parser.add_argument("--plot",     action="store_true",  help="生成对比图（需要SR和GBN都跑完）")
    args = parser.parse_args()

    if args.protocol:
        run_protocol(args.protocol)
    elif args.plot:
        plot_results()
    else:
        print("请指定 --protocol sr/gbn 或 --plot")
        print("示例:")
        print("  python3 run_comparison_experiments.py --protocol gbn")
        print("  python3 run_comparison_experiments.py --protocol sr")
        print("  python3 run_comparison_experiments.py --plot")
