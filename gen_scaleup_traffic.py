import sys
import argparse
import os

def generate_file(filename, flows):
    """辅助函数：将生成的流列表写入文件"""
    # 确保输出目录存在
    output_dir = os.path.dirname(filename)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir)
        
    with open(filename, "w") as f:
        # 第一行写总流数 (这是 C++ 代码 flowf >> flow_num 读取的)
        f.write(f"{len(flows)}\n")
        for flow in flows:
            f.write(f"{flow}\n")
    print(f"[Success] Generated {filename} with {len(flows)} flows.")

def gen_all_to_all(num_gpus, total_size_bytes, output_path):
    """生成 All-to-All 流量"""
    flows = []
    chunk_size = total_size_bytes // num_gpus
    if chunk_size < 1: chunk_size = 1
    
    step_id = 0
    for src in range(num_gpus):
        for dst in range(num_gpus):
            if src == dst: continue
            # 格式: src dst pg(3) size step_id
            line = f"{src} {dst} 3 {chunk_size} {step_id}"
            flows.append(line)
            
    generate_file(output_path, flows)

def gen_all_reduce_ring(num_gpus, total_size_bytes, output_path):
    """生成 Ring All-Reduce 流量"""
    flows = []
    chunk_size = total_size_bytes // num_gpus
    if chunk_size < 1: chunk_size = 1
    
    total_steps = 2 * (num_gpus - 1)
    
    for step_id in range(total_steps):
        for src in range(num_gpus):
            dst = (src + 1) % num_gpus
            # 格式: src dst pg(3) size step_id
            line = f"{src} {dst} 3 {chunk_size} {step_id}"
            flows.append(line)
            
    generate_file(output_path, flows)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate Scale-up traffic traces (All-Reduce / All-to-All)")
    
    # 添加命令行参数
    parser.add_argument('-n', '--ngpus', type=int, default=8, help="Number of GPUs (default: 8)")
    parser.add_argument('-s', '--size', type=int, default=128, help="Total data size in MB (default: 128)")
    parser.add_argument('-o', '--outdir', type=str, default="config", help="Output directory (default: config)")
    
    args = parser.parse_args()

    # 参数转换
    NUM_GPUS = args.ngpus
    TOTAL_DATA = args.size * 1024 * 1024 # 转换为字节
    OUT_DIR = args.outdir
    
    print(f"Generating traffic for {NUM_GPUS} GPUs, Total Data: {args.size} MB")
    
    # 自动加上路径前缀
    file_a2a = os.path.join(OUT_DIR, "flow_alltoall.txt")
    file_ar = os.path.join(OUT_DIR, "flow_allreduce.txt")
    
    # 1. 生成 All-to-All
    gen_all_to_all(NUM_GPUS, TOTAL_DATA, file_a2a)
    
    # 2. 生成 All-Reduce
    gen_all_reduce_ring(NUM_GPUS, TOTAL_DATA, file_ar)