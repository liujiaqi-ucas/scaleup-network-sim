#!/bin/bash
# ==============================================================
# 补丁脚本：针对 SR 协议补测 64MB Alltoall (BER=1e-8)
# ==============================================================

# 1. 基础配置
PROTOCOL="sr"
TRAFFIC="alltoall"
MSGSIZE="64mb"
ERRRATE="0.00000001"   # 即 1e-8
SIMTIME=0.5            # 对应原脚本中 64mb 的时长

# 定位路径
SCRIPT_DIR="$(dirname "$0")"
cd "$SCRIPT_DIR/.."
OUTDIR="experiments_h100_ber/${PROTOCOL}"
CSV="${OUTDIR}/results.csv"

# 2. 检查环境
TOPO="H100_8_${ERRRATE}_OS2"
FLOW="flow_${TRAFFIC}_8gpu_${MSGSIZE}"

if [ ! -f "config/${TOPO}.txt" ]; then
    echo "错误: 找不到拓扑文件 config/${TOPO}.txt"
    exit 1
fi

echo "开始补测: ${PROTOCOL} | ${TRAFFIC} | ${MSGSIZE} | BER=${ERRRATE}"

# 3. 运行仿真
TMPLOG=$(mktemp /tmp/patch_sim_XXXXXX.log)
python3 run.py --topo "$TOPO" --flow "$FLOW" --simul_time "$SIMTIME" > "$TMPLOG" 2>&1

# 4. 提取 Config ID
CONFIG_ID=$(grep -oP '(?<=/output/)\d{7,12}(?=/)' "$TMPLOG" | tail -1)
rm -f "$TMPLOG"

if [ -z "$CONFIG_ID" ]; then
    echo "失败: 无法从仿真日志中提取 config_ID"
    exit 1
fi

# 5. 数据处理与追加
FCT="mix/output/${CONFIG_ID}/${CONFIG_ID}_out_fct.txt"
LOG="mix/output/${CONFIG_ID}/config.log"

if [ ! -f "$FCT" ]; then
    echo "失败: 找不到结果文件 $FCT"
    exit 1
fi

NUM_FLOWS=$(wc -l < "$FCT")
NUM_ERRORS=$(grep -c "发生了错误" "$LOG" 2>/dev/null || echo 0)

# 使用 awk 计算并追加到 CSV
awk '{print $7}' "$FCT" | sort -n | awk \
    -v pr="$PROTOCOL" -v tr="$TRAFFIC" -v ms="$MSGSIZE" \
    -v er="$ERRRATE" -v fl="$NUM_FLOWS" -v errs="$NUM_ERRORS" \
    'BEGIN{s=0;n=0}
     {a[n]=$1; s+=$1; n++}
     END{
        if(n==0) exit;
        avg = s/n/1000;
        p99 = a[int(n*0.99)]/1000;
        jct = a[n-1]/1000;
        printf "%s,%s,%s,%s,%d,%d,%.2f,%.2f,%.2f\n",
          pr, tr, ms, er, fl, errs, avg, p99, jct
     }' >> "$CSV"

echo "============================================"
echo "补测完成！"
echo "ID: ${CONFIG_ID}"
echo "结果已追加至: ${CSV}"
echo "============================================"