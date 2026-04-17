#!/bin/bash
# ==============================================================
# 补充超低错误率实验 (1e-15 ~ 1e-6)，追加到 experiments_v2 结果 CSV
# 用法: bash experiments_v2/run_new_errrates.sh <sr|gbn> [--pfc 0|1]
# 不清空已有 CSV，只追加新行
# ==============================================================
set -e
cd "$(dirname "$0")/.."

PROTOCOL=${1:?用法: bash experiments_v2/run_new_errrates.sh <sr|gbn> [--pfc 0|1]}
PFC_FLAG=${2:-}   # 可选，如 "--pfc 1" 用于 pfc 分支

CSV="experiments_v2/${PROTOCOL}/results.csv"
mkdir -p "experiments_v2/${PROTOCOL}"

# 新增错误率: 1e-15 到 1e-6 (10个，与拓扑文件名对应)
NEW_ERR_RATES="
0.000000000000001
0.00000000000001
0.0000000000001
0.000000000001
0.00000000001
0.0000000001
0.000000001
0.00000001
0.0000001
0.000001
"

MSG_SIZES="1mb 4mb 16mb 64mb 128mb 256mb"
TRAFFICS="allreduce alltoall"

# 统计总数
total=0
for e in $NEW_ERR_RATES; do
  [ -z "$e" ] && continue
  for m in $MSG_SIZES; do for t in $TRAFFICS; do
    total=$((total+1))
  done; done
done

done_count=0
fail_count=0

for ERRRATE in $NEW_ERR_RATES; do
  [ -z "$ERRRATE" ] && continue

  # 检查对应拓扑文件是否存在
  TOPO="H100_8_${ERRRATE}_OS2"
  if [ ! -f "config/${TOPO}.txt" ]; then
    echo "  SKIP: 拓扑文件不存在: config/${TOPO}.txt"
    continue
  fi

  for MSGSIZE in $MSG_SIZES; do
    for TRAFFIC in $TRAFFICS; do
      done_count=$((done_count+1))
      FLOW="flow_${TRAFFIC}_8gpu_${MSGSIZE}"

      case $MSGSIZE in
        1mb)   SIMTIME=0.1 ;;
        4mb)   SIMTIME=0.1 ;;
        16mb)  SIMTIME=0.2 ;;
        64mb)  SIMTIME=0.5 ;;
        128mb) SIMTIME=1.0 ;;
        256mb) SIMTIME=2.0 ;;
        *)     SIMTIME=0.2 ;;
      esac

      echo ""
      echo "[${done_count}/${total}] ${PROTOCOL} | ${TRAFFIC} | ${MSGSIZE} | err=${ERRRATE}"

      TMPLOG=$(mktemp /tmp/sim_XXXXXX.log)
      python3 run.py --topo "$TOPO" --flow "$FLOW" --simul_time "$SIMTIME" $PFC_FLAG > "$TMPLOG" 2>&1
      RC=$?

      CONFIG_ID=$(grep -oP '(?<=/output/)\d{7,12}(?=/)' "$TMPLOG" | tail -1)
      rm -f "$TMPLOG"

      if [ -z "$CONFIG_ID" ]; then
        echo "  FAIL: 无法提取 config_ID"
        fail_count=$((fail_count+1))
        continue
      fi

      FCT="mix/output/${CONFIG_ID}/${CONFIG_ID}_out_fct.txt"
      CFG="mix/output/${CONFIG_ID}/config.txt"
      LOG="mix/output/${CONFIG_ID}/config.log"

      # 校验 flow 文件
      ACTUAL_FLOW=$(grep "^FLOW_FILE" "$CFG" | awk '{print $2}')
      EXPECTED_FLOW="config/${FLOW}.txt"
      if [ "$ACTUAL_FLOW" != "$EXPECTED_FLOW" ]; then
        echo "  FAIL: flow不匹配! 期望=${EXPECTED_FLOW} 实际=${ACTUAL_FLOW}"
        fail_count=$((fail_count+1))
        continue
      fi

      if [ ! -f "$FCT" ] || [ "$(wc -l < "$FCT")" -eq 0 ]; then
        echo "  FAIL: FCT 为空 (id=${CONFIG_ID})"
        fail_count=$((fail_count+1))
        continue
      fi

      NUM_FLOWS=$(wc -l < "$FCT")
      NUM_ERRORS=$(grep -c "发生了错误" "$LOG" 2>/dev/null || echo 0)

      awk '{print $7}' "$FCT" | sort -n | awk \
        -v pr="$PROTOCOL" -v tr="$TRAFFIC" -v ms="$MSGSIZE" \
        -v er="$ERRRATE" -v fl="$NUM_FLOWS" -v errs="$NUM_ERRORS" \
        'BEGIN{s=0;n=0}
         {a[n]=$1; s+=$1; n++}
         END{
           avg = s/n/1000;
           p99 = a[int(n*0.99)]/1000;
           jct = a[n-1]/1000;
           printf "%s,%s,%s,%s,%d,%d,%.2f,%.2f,%.2f\n",
             pr, tr, ms, er, fl, errs, avg, p99, jct
         }' >> "$CSV"

      echo "  OK: id=${CONFIG_ID} flows=${NUM_FLOWS} errors=${NUM_ERRORS} ✓"
    done
  done
done

echo ""
echo "============================================"
echo "${PROTOCOL}: ${done_count} 完成, ${fail_count} 失败"
echo "结果追加到: ${CSV}"
echo "============================================"
