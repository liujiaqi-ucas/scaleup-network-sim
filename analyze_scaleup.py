import argparse
import matplotlib.pyplot as plt
import os
import numpy as np

def analyze_scaleup_trace(config_id, fct_file):
    if not os.path.exists(fct_file):
        print(f"Error: {fct_file} not found")
        return

    # 存储结构: steps[step_id] = [fct_flow1, fct_flow2, ...]
    steps_data = {}
    
    print(f"Reading {fct_file}...")
    with open(fct_file, 'r') as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) < 8: continue
            
            # 解析列 (根据你的 C++ fprintf)
            # src dst sport dport size start fct step_id
            fct_ns = int(parts[6])
            step_id = int(parts[7])
            
            fct_us = fct_ns / 1000.0
            
            if step_id not in steps_data:
                steps_data[step_id] = []
            steps_data[step_id].append(fct_us)

    # --- 数据分析 ---
    sorted_steps = sorted(steps_data.keys())
    step_ids = []
    avg_fcts = []
    tail_fcts = [] # Max FCT per step
    
    print(f"\n--- Analysis Report ({len(sorted_steps)} Steps) ---")
    
    for sid in sorted_steps:
        fcts = steps_data[sid]
        avg = np.mean(fcts)
        tail = np.max(fcts)
        
        step_ids.append(sid)
        avg_fcts.append(avg)
        tail_fcts.append(tail)
        
        print(f"[Step {sid}] Avg: {avg:.1f}us | Tail: {tail:.1f}us")

    # --- 🎨 核心绘图逻辑 ---
    plt.figure(figsize=(10, 6))
    
    # 1. 画柱状图：表示每一步的“拖后腿”时间 (Tail Latency)
    # 这就是 Barrier 的实际等待时间
    bars = plt.bar(step_ids, tail_fcts, color='#ff9999', label='Tail Latency (Barrier Time)', alpha=0.7)
    
    # 2. 画折线图：表示大家的“平均水平” (Avg FCT)
    # 如果红柱子比蓝线高很多，说明重传严重
    plt.plot(step_ids, avg_fcts, color='#3333ff', marker='o', linewidth=2, label='Average FCT')

    # 图表美化
    plt.xlabel('Step ID (Synchronized Phase)')
    plt.ylabel('Completion Time (us)')
    plt.title(f'Scale-up Performance Analysis (Config {config_id})')
    plt.grid(axis='y', linestyle='--', alpha=0.5)
    plt.legend()
    
    # 在最高的柱子上标出数值
    if len(tail_fcts) > 0:
        max_val = np.max(tail_fcts)
        max_idx = np.argmax(tail_fcts)
        plt.text(step_ids[max_idx], max_val, f'{max_val:.0f}us', ha='center', va='bottom', fontweight='bold')

    # 保存图片
    output_png = f"scaleup_analysis_{config_id}.png"
    plt.savefig(output_png)
    print(f"\n✅ 图表已生成: {output_png}")
    print("请下载或打开该图片查看拥塞情况。")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-id', '--id', required=True, help="Config ID")
    parser.add_argument('-fdir', default='mix', help="Output folder")
    args = parser.parse_args()
    
    file_path = f"{args.fdir}/output/{args.id}/{args.id}_out_fct.txt"
    analyze_scaleup_trace(args.id, file_path)