#!/usr/bin/env python3
"""
Generate NVL72 topology file for ns-3 simulation.

NVL72 architecture:
- 72 Blackwell GPUs connected via 18 NVLink Switch chips
- Each GPU has a link to every switch chip (full bipartite graph)
- Total NVLink bandwidth per GPU: ~1.8 TB/s
- Per link: 1.8 TB/s / 18 = 100 GB/s = 800 Gbps
"""

import argparse

def gen_nvl72_topo(n_gpu=72, n_switch=18, bw="800Gbps", delay="100ns", error_rate=0.000000, oversub=2):
    n_total = n_gpu + n_switch
    n_link = n_gpu * n_switch  # each GPU connects to every switch

    # Switch IDs: n_gpu ~ n_gpu + n_switch - 1
    switch_ids = list(range(n_gpu, n_gpu + n_switch))

    lines = []

    # Line 1: total_nodes switch_count link_count
    lines.append(f"{n_total} {n_switch} {n_link}")

    # Line 2: switch IDs
    lines.append(" ".join(str(s) for s in switch_ids))

    # Links: every GPU connects to every switch
    for gpu in range(n_gpu):
        for sw in switch_ids:
            lines.append(f"{gpu} {sw} {bw} {delay} {error_rate}")

    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate NVL72 topology")
    parser.add_argument("--n_gpu", type=int, default=72, help="Number of GPUs (default: 72)")
    parser.add_argument("--n_switch", type=int, default=18, help="Number of NVLink switch chips (default: 18)")
    parser.add_argument("--bw", type=str, default="800Gbps", help="Per-link bandwidth (default: 800Gbps)")
    parser.add_argument("--delay", type=str, default="100ns", help="Per-link delay (default: 100ns)")
    parser.add_argument("--error_rate", type=float, default=0.0, help="Error rate per link (default: 0.0)")
    parser.add_argument("--oversub", type=int, default=2, help="Oversubscription ratio for filename (default: 2)")
    parser.add_argument("-o", "--output", type=str, default=None, help="Output file path")
    args = parser.parse_args()

    topo = gen_nvl72_topo(
        n_gpu=args.n_gpu,
        n_switch=args.n_switch,
        bw=args.bw,
        delay=args.delay,
        error_rate=args.error_rate,
        oversub=args.oversub,
    )

    if args.output is None:
        bw_short = args.bw.replace("Gbps", "G")
        args.output = f"NVL72_{args.n_gpu}_{bw_short}_OS{args.oversub}.txt"

    with open(args.output, "w") as f:
        f.write(topo)

    print(f"Generated topology: {args.output}")
    print(f"  GPUs: {args.n_gpu} (ID 0-{args.n_gpu-1})")
    print(f"  Switches: {args.n_switch} (ID {args.n_gpu}-{args.n_gpu+args.n_switch-1})")
    print(f"  Links: {args.n_gpu * args.n_switch}")
    print(f"  Bandwidth: {args.bw}, Delay: {args.delay}")
