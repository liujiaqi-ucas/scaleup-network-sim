#!/usr/bin/env python3
"""
NVL72 高错误率 SR vs GBN 对比实验
- 串行运行，彻底避免两条流水线竞争修改拓扑文件
- 只跑 error_rate=0.001 一个档位
- 数据量 8MB（112KB/流，约480 flits，2.4个信用窗口，足够体现协议差异）
用法:
    python3 run_nvl72_highErr.py
"""
import os, subprocess, json, time, glob, re
from pathlib import Path

# ─── 参数 ──────────────────────────────────────────────────
TOPO       = "NVL72_72_800G_OS2"
TOPO_FILE  = "config/NVL72_72_800G_OS2.txt"
NGPUS      = 72
DATA_MB    = 8          # 总量8MB → 每流 8*1024*1024/72 ≈ 116KB ≈ 484 flits
BW         = "800"      # NVLink 800Gbps
ERROR_RATE = 0.001      # 只跑高错误率
SIMUL_TIME = "0.1"
RESULT_DIR = "comparison_results_nvl72_highErr"
BRANCH_GBN = "feature/cbfc-gbn"
BRANCH_SR  = "feature/dynamic-alpha"

os.makedirs(RESULT_DIR, exist_ok=True)

# ─── 工具函数 ─────────────────────────────────────────────
def set_error_rate(rate: float):
    lines = open(TOPO_FILE).readlines()
    out   = []
    for line in lines:
        parts = line.strip().split()
        if len(parts) == 5 and parts[2].endswith("Gbps"):
            parts[4] = str(rate)
            out.append(" ".join(parts) + "\n")
        else:
            out.append(line)
    open(TOPO_FILE, "w").writelines(out)
    print(f"  [topo] error_rate={rate}")

def set_traffic(name: str):
    content = open("run.py").read()
    content = re.sub(r'target_flow_name\s*=\s*"[^"]*"',
                     f'target_flow_name = "flow_{name}"', content)
    open("run.py", "w").write(content)
    print(f"  [run.py] traffic={name}")

def gen_traffic():
    subprocess.run(["python3", "gen_scaleup_traffic.py",
                    "-n", str(NGPUS), "-s", str(DATA_MB)],
                   check=True, capture_output=True)
    print(f"  [流量] {NGPUS}GPU {DATA_MB}MB alltoall生成完毕")

def switch_branch(branch: str):
    print(f"\n  [分支] 切换到 {branch} ...")
    subprocess.run(["git", "stash"], check=False, capture_output=True)
    subprocess.run(["git", "checkout", branch], check=True, capture_output=True)
    subprocess.run(["git", "stash", "pop"], check=False, capture_output=True)
    subprocess.run(["./waf", "build"], check=True, capture_output=True)
    print(f"  [分支] 编译完成")

def run_one_experiment(protocol: str) -> dict:
    """运行一次仿真，等FCT写完就kill，然后保存结果"""
    tag = f"{protocol}_alltoall_err{ERROR_RATE:.3f}"
    print(f"\n  [实验] 开始 {tag} ...")
    t0 = time.time()

    proc = subprocess.Popen(
        ["python3", "run.py", "--topo", TOPO,
         "--simul_time", SIMUL_TIME, "--bw", BW],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    fct_path = None
    # 轮询：等FCT文件出现且写满5112条
    while True:
        time.sleep(30)
        dirs = sorted(Path("mix/output").iterdir(),
                      key=lambda p: p.stat().st_mtime, reverse=True)
        if not dirs:
            continue
        candidate = dirs[0]
        fct_files = list(candidate.glob("*_out_fct.txt"))
        if not fct_files:
            continue
        n = sum(1 for _ in open(fct_files[0]))
        elapsed = time.time() - t0
        print(f"    FCT={n}/5112  已用时={elapsed/60:.1f}min", flush=True)
        if n >= 5112:
            fct_path = str(fct_files[0])
            print(f"  [完成] FCT写满！kill仿真进程，跳过Destroy()...")
            # kill ns-3 binary（跳过耗时的Simulator::Destroy）
            subprocess.run(
                ["pkill", "-f", "network-load-balance"],
                capture_output=True
            )
            time.sleep(5)
            proc.wait(timeout=30)
            break

    elapsed = time.time() - t0
    print(f"  [完成] {tag}  elapsed={elapsed/60:.1f}min  fct={fct_path}")

    meta = {
        "protocol":   protocol,
        "traffic":    "alltoall",
        "error_rate": ERROR_RATE,
        "n_gpus":     NGPUS,
        "data_mb":    DATA_MB,
        "fct_file":   fct_path,
        "n_flows":    5112,
        "elapsed_min": elapsed / 60,
    }
    out_json = f"{RESULT_DIR}/{tag}.json"
    json.dump(meta, open(out_json, "w"), indent=2)
    print(f"  [保存] {out_json}")
    return meta

# ─── 主流程（完全串行）────────────────────────────────────
def main():
    print("="*60)
    print(f"NVL72 高错误率对比实验")
    print(f"  error_rate={ERROR_RATE}, DATA_MB={DATA_MB}")
    print(f"  每流 ≈ {DATA_MB*1024*1024//NGPUS//240:.0f} flits")
    print("  完全串行，先GBN后SR")
    print("="*60)

    # 准备流量文件
    gen_traffic()
    set_traffic("alltoall")
    # 设置错误率（唯一一次，不再在中途改动）
    set_error_rate(ERROR_RATE)

    results = []

    # ── 1. 先跑 GBN ──────────────────────────────────────
    print("\n[阶段1] GBN实验")
    switch_branch(BRANCH_GBN)
    set_error_rate(ERROR_RATE)   # 切分支后再确认一次
    set_traffic("alltoall")
    meta_gbn = run_one_experiment("gbn")
    results.append(meta_gbn)

    # ── 2. 再跑 SR ───────────────────────────────────────
    print("\n[阶段2] SR实验")
    switch_branch(BRANCH_SR)
    set_error_rate(ERROR_RATE)   # 切分支后再确认一次
    set_traffic("alltoall")
    meta_sr = run_one_experiment("sr")
    results.append(meta_sr)

    # ── 3. 生成对比图 ─────────────────────────────────────
    print("\n[阶段3] 生成对比图...")
    make_plot(meta_gbn, meta_sr)

    print("\n全部完成！")
    print(f"结果目录: {RESULT_DIR}/")

def make_plot(meta_gbn, meta_sr):
    import pandas as pd
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.patches as mpatches
    import numpy as np

    COLORS = {"SR": "#2196F3", "GBN": "#FF5722"}

    def load(path):
        if not path or not os.path.exists(path):
            return pd.Series(dtype=float)
        df = pd.read_csv(path, sep=" ", header=None,
                         names=["src","dst","sport","dport","size","startTs","fct","stepId"])
        return df["fct"] / 1000.0

    sr  = load(meta_sr["fct_file"])
    gbn = load(meta_gbn["fct_file"])

    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    fig.suptitle(
        f"CBFC+SR vs CBFC+GBN  —  NVL72 (72-GPU, 800Gbps/100ns)\n"
        f"AlltoAll  {DATA_MB}MB total ({DATA_MB*1024*1024//NGPUS//1024}KB/flow)  "
        f"error_rate={ERROR_RATE}  (串行运行，error_rate完全一致)",
        fontsize=13, fontweight="bold", y=0.99)

    # CDF
    ax = axes[0][0]
    for data, proto in [(sr,"SR"),(gbn,"GBN")]:
        if len(data)==0: continue
        sv = np.sort(data.dropna()); cdf = np.arange(1,len(sv)+1)/len(sv)
        ax.plot(sv, cdf, color=COLORS[proto], lw=2.5, label=proto)
    ax.set_xlabel("FCT (us)"); ax.set_ylabel("CDF")
    ax.set_title("FCT CDF"); ax.legend(fontsize=12); ax.grid(alpha=0.3); ax.set_ylim(0,1.05)

    # Bar chart
    ax = axes[0][1]
    metrics = ["Mean","Median","P95","P99","Max"]
    x = np.arange(len(metrics))
    for pi, (data, proto) in enumerate([(sr,"SR"),(gbn,"GBN")]):
        if len(data)==0: continue
        vals = [data.mean(), data.median(),
                data.quantile(0.95), data.quantile(0.99), data.max()]
        bars = ax.bar(x+pi*0.35, vals, 0.35, label=proto,
                      color=COLORS[proto], alpha=0.88)
        for bar, v in zip(bars, vals):
            ax.text(bar.get_x()+bar.get_width()/2, bar.get_height()+0.005,
                    f"{v:.2f}", ha="center", va="bottom", fontsize=8)
    ax.set_xticks(x+0.175); ax.set_xticklabels(metrics)
    ax.set_ylabel("FCT (us)"); ax.set_title("Statistics")
    ax.legend(fontsize=12); ax.grid(axis="y", alpha=0.3)

    # Box
    ax = axes[1][0]
    bp = ax.boxplot([sr.dropna().values, gbn.dropna().values],
                    labels=["SR","GBN"], patch_artist=True,
                    medianprops={"color":"black","linewidth":2})
    for patch, proto in zip(bp["boxes"], ["SR","GBN"]):
        patch.set_facecolor(COLORS[proto]); patch.set_alpha(0.78)
    ax.set_ylabel("FCT (us)"); ax.set_title("FCT Distribution")
    ax.grid(axis="y", alpha=0.3)

    # Ratio + summary text
    ax = axes[1][1]
    ax.axis("off")
    lines = [
        "="*44,
        "  NVL72 SR vs GBN  Summary",
        "="*44, "",
        f"  {'Metric':<10} {'SR':>8} {'GBN':>8} {'GBN/SR':>8}",
        "  " + "-"*40,
    ]
    fns = {"Mean":lambda x:x.mean(), "Median":lambda x:x.median(),
           "P95":lambda x:x.quantile(0.95), "P99":lambda x:x.quantile(0.99),
           "Max":lambda x:x.max(), "Std":lambda x:x.std()}
    for m, fn in fns.items():
        sv = fn(sr); gv = fn(gbn)
        ratio = gv/sv if sv > 0 else float("nan")
        lines.append(f"  {m:<10} {sv:>8.3f} {gv:>8.3f} {ratio:>7.3f}x")
    lines += ["", f"  flows={meta_sr['n_flows']}",
              f"  error_rate={ERROR_RATE}  (单向)",
              f"  Data/flow: {DATA_MB*1024*1024//NGPUS//1024}KB",
              f"  SR elapsed: {meta_sr['elapsed_min']:.1f}min",
              f"  GBN elapsed: {meta_gbn['elapsed_min']:.1f}min"]
    ax.text(0.05, 0.95, "\n".join(lines), transform=ax.transAxes,
            fontsize=9, verticalalignment="top", fontfamily="monospace")

    plt.tight_layout(rect=[0,0,1,0.98])
    out = f"{RESULT_DIR}/sr_vs_gbn_nvl72_highErr.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    print(f"  [图表] 保存至 {out}")
    plt.close()

if __name__ == "__main__":
    main()
