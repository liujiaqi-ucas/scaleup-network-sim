#!/bin/bash
# ==============================================================
# NVL72 alltoall 16mb 对比实验
# 用法: bash experiments_nvl72/run_nvl72.sh <sr|gbn>
#   sr  → 在 feature/dynamic-alpha 分支运行
#   gbn → 在 feature/cbfc-gbn 分支运行
# ==============================================================
cd "$(dirname "$0")/.."

PROTOCOL=${1:?用法: bash experiments_nvl72/run_nvl72.sh <sr|gbn>}
OUTDIR="experiments_nvl72/${PROTOCOL}"
mkdir -p "$OUTDIR"
CSV="${OUTDIR}/results.csv"

# 10 个错误率 (BER, 1e-15 ~ 1e-6)
ERR_RATES="
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

MSGSIZE="16mb"
TRAFFIC="alltoall"
SIMTIME="0.5"   # NVL72 带宽高，16mb 完成很快

> "$CSV"  # 清空/新建

total=0
for e in $ERR_RATES; do [ -z "$e" ] && continue; total=$((total+1)); done
done_count=0
fail_count=0

for ERRRATE in $ERR_RATES; do
  [ -z "$ERRRATE" ] && continue
  done_count=$((done_count+1))
  TOPO="NVL72_${ERRRATE}_OS2"
  FLOW="flow_${TRAFFIC}_72gpu_${MSGSIZE}"

  if [ ! -f "config/${TOPO}.txt" ]; then
    echo "  SKIP: config/${TOPO}.txt 不存在"
    fail_count=$((fail_count+1))
    continue
  fi

  echo ""
  echo "[${done_count}/${total}] ${PROTOCOL} | NVL72 | ${TRAFFIC} | ${MSGSIZE} | BER=${ERRRATE}"

  TMPLOG=$(mktemp /tmp/sim_XXXXXX.log)
  python3 run.py --topo "$TOPO" --flow "$FLOW" --simul_time "$SIMTIME" > "$TMPLOG" 2>&1

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
       avg  = s/n/1000;
       p99  = a[int(n*0.99)]/1000;
       jct  = a[n-1]/1000;
       printf "%s,%s,%s,%s,%d,%d,%.2f,%.2f,%.2f\n",
         pr, tr, ms, er, fl, errs, avg, p99, jct
     }' >> "$CSV"

  echo "  OK: id=${CONFIG_ID} flows=${NUM_FLOWS} errors=${NUM_ERRORS} ✓"
done

echo ""
echo "============================================"
echo "${PROTOCOL}: ${done_count} 完成, ${fail_count} 失败"
echo "结果: ${CSV}"
echo "============================================"
cat "$CSV"
