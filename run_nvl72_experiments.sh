#!/bin/bash
# ==============================================================
# NVL72 GBN vs SR 全量对比实验
#
# 用法:
#   1. 在 SR 分支 (feature/dynamic-alpha) 编译后运行:
#      bash run_nvl72_experiments.sh sr
#
#   2. commit 后切到 GBN 分支 (feature/cbfc-gbn) 编译后运行:
#      bash run_nvl72_experiments.sh gbn
#
#   3. 两组都跑完后, 将两个CSV拷到一起运行画图:
#      python3 plot_nvl72_results.py
#
# 实验矩阵: 5错误率 × 4消息大小 × alltoall = 20组/协议
# 总计: 40组 (SR 20 + GBN 20)
# ==============================================================
set -e
cd "$(dirname "$0")"

PROTOCOL=${1:?用法: bash run_nvl72_experiments.sh <sr|gbn>}
OUTDIR="experiments_nvl72/${PROTOCOL}"
mkdir -p "$OUTDIR"
CSV="${OUTDIR}/results.csv"

ERR_RATES="0.00001 0.00005 0.0001 0.0005 0.001"
MSG_SIZES="1mb 4mb 16mb 64mb"

# 清空旧结果
> "$CSV"

total=20
done_count=0
fail_count=0

echo "============================================"
echo " NVL72 实验: ${PROTOCOL}"
echo " 矩阵: 5 错误率 × 4 消息大小 × alltoall"
echo " 总计: ${total} 组"
echo "============================================"
echo ""

for ERRRATE in $ERR_RATES; do
  for MSGSIZE in $MSG_SIZES; do
    done_count=$((done_count+1))
    TOPO="NVL72_${ERRRATE}_OS2"
    FLOW="flow_alltoall_72gpu_${MSGSIZE}"

    # NVL72 规模大, 仿真时间需要更长
    case $MSGSIZE in
      1mb)  SIMTIME=0.5 ;;
      4mb)  SIMTIME=1.0 ;;
      16mb) SIMTIME=2.0 ;;
      64mb) SIMTIME=5.0 ;;
    esac

    echo "[${done_count}/${total}] ${PROTOCOL} | alltoall | ${MSGSIZE} | err=${ERRRATE} | simT=${SIMTIME}s"

    TMPLOG=$(mktemp /tmp/nvl72_sim_XXXXXX.log)
    START_TIME=$(date +%s)

    python3 run.py --topo "$TOPO" --flow "$FLOW" --simul_time "$SIMTIME" > "$TMPLOG" 2>&1
    RC=$?

    END_TIME=$(date +%s)
    ELAPSED=$((END_TIME - START_TIME))

    CONFIG_ID=$(grep -oP '(?<=/output/)\d{7,12}(?=/)' "$TMPLOG" | tail -1)
    rm -f "$TMPLOG"

    if [ -z "$CONFIG_ID" ]; then
      echo "  FAIL: 无法提取 config_ID (rc=$RC, ${ELAPSED}s)"
      fail_count=$((fail_count+1))
      continue
    fi

    FCT="mix/output/${CONFIG_ID}/${CONFIG_ID}_out_fct.txt"
    LOG="mix/output/${CONFIG_ID}/config.log"
    CFG="mix/output/${CONFIG_ID}/config.txt"

    # 校验 flow 文件
    ACTUAL_FLOW=$(grep "^FLOW_FILE" "$CFG" 2>/dev/null | awk '{print $2}')
    EXPECTED_FLOW="config/${FLOW}.txt"
    if [ "$ACTUAL_FLOW" != "$EXPECTED_FLOW" ]; then
      echo "  FAIL: flow不匹配! 期望=${EXPECTED_FLOW} 实际=${ACTUAL_FLOW} (${ELAPSED}s)"
      fail_count=$((fail_count+1))
      continue
    fi

    if [ ! -f "$FCT" ] || [ "$(wc -l < "$FCT")" -eq 0 ]; then
      echo "  FAIL: FCT为空 (id=${CONFIG_ID}, ${ELAPSED}s)"
      fail_count=$((fail_count+1))
      continue
    fi

    NUM_FLOWS=$(wc -l < "$FCT")
    NUM_ERRORS=$(grep -c "发生了错误" "$LOG" 2>/dev/null || echo 0)

    # 计算 mean, p99, JCT=max(start+fct)-min(start)
    python3 -c "
import sys
fcts, starts = [], []
for line in open('${FCT}'):
    parts = line.strip().split()
    if len(parts) >= 8:
        fcts.append(int(parts[6]))
        starts.append(int(parts[5]))
n = len(fcts)
fcts_sorted = sorted(fcts)
mean = sum(fcts)/n/1000
p99 = fcts_sorted[int(n*0.99)]/1000
ends = [s+f for s,f in zip(starts, fcts)]
jct = (max(ends)-min(starts))/1000
row = '${PROTOCOL},alltoall,${MSGSIZE},${ERRRATE},${NUM_FLOWS},${NUM_ERRORS},'
row += str(round(mean,2)) + ',' + str(round(p99,2)) + ',' + str(round(jct,2))
print(row)
" >> "$CSV"

    echo "  OK: id=${CONFIG_ID} flows=${NUM_FLOWS} errors=${NUM_ERRORS} (${ELAPSED}s) ✓"
  done
done

echo ""
echo "============================================"
echo "${PROTOCOL}: ${done_count} 完成, ${fail_count} 失败"
echo "结果: ${CSV}"
echo "============================================"
echo ""
echo "CSV 内容:"
cat "$CSV"
echo ""
echo "下一步:"
if [ "$PROTOCOL" = "sr" ]; then
  echo "  1. git add experiments_nvl72/ && git commit -m 'test: SR NVL72实验'"
  echo "  2. git stash push -- mix/.history"
  echo "  3. git checkout feature/cbfc-gbn"
  echo "  4. make -j\$(nproc)"
  echo "  5. bash run_nvl72_experiments.sh gbn"
else
  echo "  1. git add experiments_nvl72/ && git commit -m 'test: GBN NVL72实验'"
  echo "  2. git checkout feature/dynamic-alpha"
  echo "  3. 将 GBN CSV 合并后运行: python3 plot_nvl72_results.py"
fi
