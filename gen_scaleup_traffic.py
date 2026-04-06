import argparse
import os


def generate_file(filename, flows):
    """将生成的流列表写入文件"""
    if not os.path.exists("config"):
        os.makedirs("config")
    path = os.path.join("config", filename)
    with open(path, "w") as f:
        f.write(f"{len(flows)}\n")
        for flow in flows:
            f.write(f"{flow}\n")
    print(f"[Success] Generated {path} with {len(flows)} flows.")


def gen_alltoall(num_gpus, chunk_size_bytes, filename):
    """
    AllToAll: 每个 GPU 同时向其他所有 GPU 各发 chunk_size_bytes。
    chunk_size_bytes = M_chunk（每个 GPU pair 的发送量）。
    所有流 step_id=0，表示并发执行，无顺序依赖。
    """
    flows = []
    step_id = 0
    for src in range(num_gpus):
        for dst in range(num_gpus):
            if src == dst:
                continue
            flows.append(f"{src} {dst} 3 {chunk_size_bytes} {step_id}")
    generate_file(filename, flows)


def gen_allreduce_ring(num_gpus, total_size_bytes, filename):
    """
    Ring AllReduce: 2*(N-1) 个 step，每个 step 每个 GPU 向相邻 GPU
    发送 total_size_bytes/N 字节（per-step size）。
    total_size_bytes = M_total（张量总大小）。
    step_id 作为 Barrier 控制，同一 step_id 的流全部完成后才进入下一步。
    """
    flows = []
    chunk_size = total_size_bytes // num_gpus
    total_steps = 2 * (num_gpus - 1)
    for step_id in range(total_steps):
        for src in range(num_gpus):
            dst = (src + 1) % num_gpus
            flows.append(f"{src} {dst} 3 {chunk_size} {step_id}")
    generate_file(filename, flows)


def gen_all_experiments():
    """
    批量生成所有实验所需的流量文件。

    AllReduce（仅 H100 8-GPU）:
      M_total ∈ {1, 4, 16, 64} MB
      per-step size = M_total / 8
      命名: flow_allreduce_8gpu_<size>mb.txt

    AllToAll（H100 8-GPU + NVL72 72-GPU）:
      M_chunk ∈ {1, 4, 16, 64} MB（每 GPU pair 发送量）
      命名: flow_alltoall_<n>gpu_<size>mb.txt
    """
    # AllReduce: H100 8-GPU only
    allreduce_sizes_mb = [1, 4, 16, 64]
    for size_mb in allreduce_sizes_mb:
        total_bytes = size_mb * 1024 * 1024
        filename = f"flow_allreduce_8gpu_{size_mb}mb.txt"
        gen_allreduce_ring(8, total_bytes, filename)

    # AllToAll: H100 8-GPU and NVL72 72-GPU
    alltoall_sizes_mb = [1, 4, 16, 64]
    for size_mb in alltoall_sizes_mb:
        chunk_bytes = size_mb * 1024 * 1024
        gen_alltoall(8,  chunk_bytes, f"flow_alltoall_8gpu_{size_mb}mb.txt")
        gen_alltoall(72, chunk_bytes, f"flow_alltoall_72gpu_{size_mb}mb.txt")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="生成 Scale-up 集合通信流量文件"
    )
    parser.add_argument(
        "--all", action="store_true",
        help="批量生成所有实验流量文件"
    )
    parser.add_argument(
        "--type", choices=["allreduce", "alltoall"],
        help="单独生成指定类型的流量"
    )
    parser.add_argument(
        "-n", "--ngpus", type=int, default=8,
        help="GPU 数量"
    )
    parser.add_argument(
        "-s", "--size", type=float, default=16,
        help="AllReduce: M_total (MB)；AllToAll: M_chunk (MB，每 GPU pair)"
    )
    args = parser.parse_args()

    if args.all:
        gen_all_experiments()
    elif args.type == "allreduce":
        total_bytes = int(args.size * 1024 * 1024)
        filename = f"flow_allreduce_{args.ngpus}gpu_{int(args.size)}mb.txt"
        gen_allreduce_ring(args.ngpus, total_bytes, filename)
    elif args.type == "alltoall":
        chunk_bytes = int(args.size * 1024 * 1024)
        filename = f"flow_alltoall_{args.ngpus}gpu_{int(args.size)}mb.txt"
        gen_alltoall(args.ngpus, chunk_bytes, filename)
    else:
        parser.print_help()
