import argparse
import os

def generate_file(filename, flows):
    """将生成的流列表写入文件"""
    if not os.path.exists("config"):
        os.makedirs("config")
    path = os.path.join("config", filename)
    with open(path, "w") as f:
        # 第一行必须是总流数，对应 C++ 中的 flowf >> flow_num
        f.write(f"{len(flows)}\n")
        for flow in flows:
            f.write(f"{flow}\n")
    print(f"[Success] Generated {path} with {len(flows)} flows.")

def gen_all_to_all(num_gpus, total_size_bytes):
    """生成 All-to-All 流量 (MoE 场景)"""
    flows = []
    chunk_size = total_size_bytes // num_gpus
    step_id = 0  # All-to-All 是所有流并发，因此全部属于 Step 0
    
    for src in range(num_gpus):
        for dst in range(num_gpus):
            if src == dst: continue
            # 格式: src dst pg(3) size step_id
            flows.append(f"{src} {dst} 3 {chunk_size} {step_id}")
    generate_file("flow_alltoall.txt", flows)

def gen_all_reduce_ring(num_gpus, total_size_bytes):
    """生成 Ring All-Reduce 流量 (梯度同步场景)"""
    flows = []
    chunk_size = total_size_bytes // num_gpus
    # Ring 算法总步骤数为 2 * (N-1)
    total_steps = 2 * (num_gpus - 1)
    
    for step_id in range(total_steps):
        for src in range(num_gpus):
            # 环形拓扑：每个 GPU 发给它的下一个邻居
            dst = (src + 1) % num_gpus
            # 这里的 step_id 将作为控制 Barrier 的红绿灯
            flows.append(f"{src} {dst} 3 {chunk_size} {step_id}")
    generate_file("flow_allreduce.txt", flows)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-n', '--ngpus', type=int, default=8, help="GPU 数量")
    parser.add_argument('-s', '--size', type=float, default=64, help="总数据量 (MB)")
    args = parser.parse_args()

    total_bytes = args.size * 1024 * 1024
    print(f"Generating for {args.ngpus} GPUs, Total Size: {args.size} MB")
    
    gen_all_to_all(args.ngpus, total_bytes)
    gen_all_reduce_ring(args.ngpus, total_bytes)