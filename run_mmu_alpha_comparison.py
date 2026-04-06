#!/usr/bin/env python3
"""
MMU α 策略对比实验：全局固定 α vs 每端口动态 α (AIMD)
- 拓扑：H100_8_300G_OS2（8GPU, 4交换机, 300Gbps）
- 流量：alltoall
- 错误率档位：0.0001, 0.001, 0.01
- 完全串行运行，避免文件竞争
用法:
    python3 run_mmu_alpha_comparison.py
"""
import os, subprocess, json, time, re
from pathlib import Path

# ─── 参数 ──────────────────────────────────────────────────
TOPO           = "H100_8_300G_OS2"
TOPO_FILE      = "config/H100_8_300G_OS2.txt"
NGPUS          = 8
DATA_MB        = 4      # 总量4MB → 每流约 4*1024KB/56 ≈ 73KB
BW             = "200"
SIMUL_TIME     = "0.1"
RESULT_DIR     = "comparison_results_mmu_alpha"
BRANCH_DYNAMIC = "feature/cbfc-gbn"    # 每端口动态 α (AIMD)
BRANCH_GLOBAL  = "feature/mmu-global-alpha"  # 全局固定 α

ERROR_RATES = [0.0001, 0.001, 0.01]

# MMU 参数配置：
# - pool=4096: 有限共享池，会触发准入拒绝
# - min_guarantee=32: 每端口最低保底
# - credit=192: 每流最多192 flits in-flight
# 阈值: 固定α=0.5 → 32+0.5×4096=2080, 动态α→0.1 → 32+0.1×4096=442
# 每端口最大load = 7×192 = 1344, 在两个阈值之间 → 能看到差异
MMU_POOL_SIZE     = 4096   # 覆盖 run.py 默认值 (65536)
MMU_MIN_GUARANTEE = 32     # 覆盖默认值 (64)
CREDIT_INIT       = 192    # 阈值: 固定α允许, 动态α拒绝

os.makedirs(RESULT_DIR, exist_ok=True)

# H100 8-GPU alltoall: 8*(8-1)=56 flows
N_FLOWS = 56

# ─── 工具函数 ─────────────────────────────────────────────
def set_error_rate(rate: float):
    lines = open(TOPO_FILE).readlines()
    out = []
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

def set_mmu_params(pool_size: int, min_guarantee: int, credit_init: int):
    lines = open("run.py").readlines()
    out = []
    for line in lines:
        if '"H100_8_300G_OS2"' in line and "mmu_pool_size" in line:
            line = re.sub(r'"mmu_pool_size"\s*:\s*\d+', f'"mmu_pool_size": {pool_size}', line)
            line = re.sub(r'"mmu_min_guarantee"\s*:\s*\d+', f'"mmu_min_guarantee": {min_guarantee}', line)
            line = re.sub(r'"credit_init"\s*:\s*\d+', f'"credit_init": {credit_init}', line)
        out.append(line)
    open("run.py", "w").writelines(out)
    print(f"  [MMU params] pool_size={pool_size}, min_guarantee={min_guarantee}, credit_init={credit_init}")

def gen_traffic():
    subprocess.run(["python3", "gen_scaleup_traffic.py",
                    "-n", str(NGPUS), "-s", str(DATA_MB)],
                   check=True, capture_output=True)
    print(f"  [流量] {NGPUS}GPU {DATA_MB}MB alltoall生成完毕")

MMU_FILES = [
    "src/point-to-point/model/switch-mmu.cc",
    "src/point-to-point/model/switch-mmu.h",
]

def switch_mmu_impl(branch: str):
    """只替换 switch-mmu 两个文件，其余文件（config/run.py）保持当前状态"""
    print(f"  [MMU切换] 使用 {branch} 的 switch-mmu 实现 ...")
    subprocess.run(["git", "checkout", branch, "--"] + MMU_FILES,
                   check=True, capture_output=True)
    subprocess.run(["./waf", "build"], check=True, capture_output=True)
    print(f"  [MMU切换] 编译完成")

def run_one(protocol: str, error_rate: float) -> dict:
    tag = f"{protocol}_alltoall_err{error_rate:.4f}"
    print(f"\n  [实验] 开始 {tag} ...")
    t0 = time.time()

    # 记录实验开始前的时间戳，用于后续找到本次创建的目录
    start_ts = time.time()

    result = subprocess.run(
        ["python3", "run.py", "--topo", TOPO,
         "--simul_time", SIMUL_TIME, "--bw", BW],
        capture_output=True, text=True, timeout=600
    )

    elapsed = time.time() - t0

    if result.returncode != 0:
        print(f"  [错误] 仿真失败 (returncode={result.returncode})")
        print(result.stderr[-2000:] if result.stderr else "(no stderr)")
        fct_path = None
    else:
        # 找到本次运行创建的最新目录（mtime >= start_ts）
        fct_path = None
        dirs = sorted(Path("mix/output").iterdir(),
                      key=lambda p: p.stat().st_mtime, reverse=True)
        for d in dirs:
            if d.stat().st_mtime >= start_ts - 5:
                fct_files = list(d.glob("*_out_fct.txt"))
                if fct_files:
                    n = sum(1 for _ in open(fct_files[0]))
                    if n > 0:
                        fct_path = str(fct_files[0])
                        print(f"    FCT={n} flows  dir={d.name}")
                        break

    print(f"  [完成] {tag}  elapsed={elapsed:.1f}s  fct={fct_path}")

    meta = {
        "protocol":    protocol,
        "traffic":     "alltoall",
        "error_rate":  error_rate,
        "n_gpus":      NGPUS,
        "data_mb":     DATA_MB,
        "fct_file":    fct_path,
        "n_flows":     N_FLOWS,
        "elapsed_s":   elapsed,
    }
    out_json = f"{RESULT_DIR}/{tag}.json"
    json.dump(meta, open(out_json, "w"), indent=2)
    return meta

# ─── 主流程 ───────────────────────────────────────────────
def main():
    print("=" * 60)
    print("MMU α 策略对比实验")
    print(f"  拓扑: {TOPO}")
    print(f"  数据量: {DATA_MB}MB ({DATA_MB*1024//NGPUS}KB/flow)")
    print(f"  错误率: {ERROR_RATES}")
    print(f"  动态α分支: {BRANCH_DYNAMIC}")
    print(f"  固定α分支: {BRANCH_GLOBAL}")
    print("=" * 60)

    gen_traffic()
    set_traffic("alltoall")
    set_mmu_params(MMU_POOL_SIZE, MMU_MIN_GUARANTEE, CREDIT_INIT)

    all_results = []

    for error_rate in ERROR_RATES:
        print(f"\n{'='*50}")
        print(f"error_rate = {error_rate}")
        print(f"{'='*50}")

        # --- 固定 α ---
        print(f"\n[固定α] error_rate={error_rate}")
        switch_mmu_impl(BRANCH_GLOBAL)
        set_error_rate(error_rate)
        set_traffic("alltoall")
        meta_global = run_one("global_alpha", error_rate)
        all_results.append(meta_global)

        # --- 动态 α ---
        print(f"\n[动态α] error_rate={error_rate}")
        switch_mmu_impl(BRANCH_DYNAMIC)
        set_error_rate(error_rate)
        set_traffic("alltoall")
        meta_dynamic = run_one("dynamic_alpha", error_rate)
        all_results.append(meta_dynamic)

    # 保存汇总
    summary_path = f"{RESULT_DIR}/summary.json"
    json.dump(all_results, open(summary_path, "w"), indent=2)
    print(f"\n[汇总] {summary_path}")

    # 生成图表
    make_plots(all_results)

    print("\n全部完成！")
    print(f"结果目录: {RESULT_DIR}/")


def make_plots(all_results):
    import pandas as pd
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np

    COLORS = {"global_alpha": "#FF5722", "dynamic_alpha": "#2196F3"}
    LABELS = {"global_alpha": "Fixed-alpha(0.5)", "dynamic_alpha": "Dynamic-alpha(AIMD)"}

    def load(path):
        if not path or not os.path.exists(path):
            return pd.Series(dtype=float)
        df = pd.read_csv(path, sep=" ", header=None,
                         names=["src","dst","sport","dport","size","startTs","fct","stepId"])
        return df["fct"] / 1000.0  # ns -> us

    # 按 error_rate 分组
    from collections import defaultdict
    grouped = defaultdict(dict)
    for m in all_results:
        grouped[m["error_rate"]][m["protocol"]] = m

    n_rates = len(ERROR_RATES)
    fig, axes = plt.subplots(n_rates, 3, figsize=(18, 5 * n_rates))
    if n_rates == 1:
        axes = [axes]

    fig.suptitle(
        f"MMU Alpha Policy: Global Fixed alpha=0.5 vs Per-Port Dynamic alpha (AIMD)\n"
        f"H100 8-GPU AlltoAll {DATA_MB}MB total  "
        f"(pool={MMU_POOL_SIZE}, credit={CREDIT_INIT})  CBFC+GBN",
        fontsize=13, fontweight="bold")

    for row, error_rate in enumerate(ERROR_RATES):
        g = grouped.get(error_rate, {})
        global_data   = load(g.get("global_alpha",  {}).get("fct_file"))
        dynamic_data  = load(g.get("dynamic_alpha",  {}).get("fct_file"))

        # CDF
        ax = axes[row][0]
        for data, proto in [(global_data, "global_alpha"), (dynamic_data, "dynamic_alpha")]:
            if len(data) == 0: continue
            sv = np.sort(data.dropna())
            cdf = np.arange(1, len(sv)+1) / len(sv)
            ax.plot(sv, cdf, color=COLORS[proto], lw=2.5, label=LABELS[proto])
        ax.set_xlabel("FCT (us)"); ax.set_ylabel("CDF")
        ax.set_title(f"FCT CDF  (error_rate={error_rate})")
        ax.legend(fontsize=11); ax.grid(alpha=0.3); ax.set_ylim(0, 1.05)

        # Bar: 统计指标
        ax = axes[row][1]
        metrics = ["Mean", "P95", "P99", "Max"]
        x = np.arange(len(metrics))
        for pi, (data, proto) in enumerate([(global_data,"global_alpha"),(dynamic_data,"dynamic_alpha")]):
            if len(data) == 0: continue
            vals = [data.mean(), data.quantile(0.95), data.quantile(0.99), data.max()]
            bars = ax.bar(x + pi*0.4, vals, 0.4, label=LABELS[proto],
                          color=COLORS[proto], alpha=0.85)
            for bar, v in zip(bars, vals):
                ax.text(bar.get_x()+bar.get_width()/2, bar.get_height(),
                        f"{v:.1f}", ha="center", va="bottom", fontsize=8)
        ax.set_xticks(x + 0.2); ax.set_xticklabels(metrics)
        ax.set_ylabel("FCT (us)"); ax.set_title(f"Statistics (error_rate={error_rate})")
        ax.legend(fontsize=11); ax.grid(axis="y", alpha=0.3)

        # Summary text
        ax = axes[row][2]
        ax.axis("off")
        fns = [("Mean", lambda x: x.mean()), ("Median", lambda x: x.median()),
               ("P95",  lambda x: x.quantile(0.95)), ("P99",  lambda x: x.quantile(0.99)),
               ("Max",  lambda x: x.max()), ("Std",  lambda x: x.std())]
        lines = [
            f"error_rate = {error_rate}",
            f"{'Metric':<8} {'Fixed':>8} {'Dynamic':>8} {'Dyn/Fix':>8}",
            "-" * 38,
        ]
        for name, fn in fns:
            gv = fn(global_data)  if len(global_data)  > 0 else float("nan")
            dv = fn(dynamic_data) if len(dynamic_data) > 0 else float("nan")
            ratio = dv / gv if gv > 0 else float("nan")
            lines.append(f"{name:<8} {gv:>8.2f} {dv:>8.2f} {ratio:>7.3f}x")
        lines += [
            "",
            f"flows={N_FLOWS}  data={DATA_MB}MB",
            f"({DATA_MB*1024//NGPUS}KB/flow)",
        ]
        ax.text(0.05, 0.95, "\n".join(lines), transform=ax.transAxes,
                fontsize=9, va="top", fontfamily="monospace",
                bbox=dict(boxstyle="round", facecolor="lightyellow", alpha=0.8))

    plt.tight_layout(rect=[0, 0, 1, 0.95])
    out = f"{RESULT_DIR}/mmu_alpha_comparison.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    print(f"  [图表] {out}")
    plt.close()


if __name__ == "__main__":
    main()
