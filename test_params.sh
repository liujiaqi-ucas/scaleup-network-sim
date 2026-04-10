#!/bin/bash
# 测试不同参数配置
# 用法: bash test_params.sh <config_name> <pool> <ming> <credit> <rto>

CONFIG_NAME=$1
POOL=$2
MING=$3
CREDIT=$4
RTO=$5

echo "=========================================="
echo "Testing config: $CONFIG_NAME"
echo "  pool=$POOL ming=$MING credit=$CREDIT rto=$RTO"
echo "=========================================="

# 临时修改 _H100_LL
sed -i "s/^_H100_LL  = .*/_H100_LL  = {\"mmu_pool_size\": $POOL, \"mmu_min_guarantee\": $MING, \"credit_init\": $CREDIT, \"rto_us\": $RTO}  # TEST: $CONFIG_NAME/" run.py

# 跑 16MB + 0.001 错误率
python3 run.py --topo H100_8_0.001_OS2 --flow flow_alltoall_8gpu_16mb --simul_time 0.1 2>&1 | tail -5

# 找最新输出
D=$(ls -t mix/output/ | head -1)
FLOWS=$(wc -l < mix/output/$D/${D}_out_fct.txt 2>/dev/null || echo "0")
ERRORS=$(grep -c "发生了错误" mix/output/$D/config.log 2>/dev/null || echo "0")
RTOS=$(grep -c "RTO" mix/output/$D/config.log 2>/dev/null || echo "0")
FCT=$(awk '{s+=$7;n++}END{if(n>0) printf "%.1f", s/n/1000; else print "N/A"}' mix/output/$D/${D}_out_fct.txt 2>/dev/null)

# 计算存储
TX_MB=$(echo "scale=2; $POOL * 256 / 1048576" | bc)
RX_MB=$(echo "scale=2; 8 * $CREDIT * 1.25 * 256 / 1048576" | bc)
TOTAL_MB=$(echo "scale=2; $TX_MB + $RX_MB" | bc)

echo ""
echo "[$CONFIG_NAME] flows=$FLOWS/56 | errors=$ERRORS | RTOs=$RTOS | FCT=${FCT}us"
echo "[$CONFIG_NAME] Storage: TX=${TX_MB}MB + RX=${RX_MB}MB = ${TOTAL_MB}MB/switch"
echo ""
