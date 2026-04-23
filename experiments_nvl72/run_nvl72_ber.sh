#!/bin/bash
# ==============================================================
# NVL72 alltoall BER 实验 (16mb + 64mb, 11 错误率)
# 用法: bash experiments_nvl72/run_nvl72_ber.sh
# 手动切换分支后执行即可，数据追加到 experiments_nvl72/{协议}/results.csv
# ==============================================================
cd "$(dirname "$0")/.."

# ── 自动从分支名推断协议 ───────────────────────────────────────
BRANCH=$(git branch --show-current 2>/dev/null)
case "$BRANCH" in
  *dynamic-alpha*)  PROTOCOL="sr"       ; PFC_FLAG="" ;;
  *cbfc-gbn*)       PROTOCOL="gbn"      ; PFC_FLAG="" ;;
  *pfc-support*)    PROTOCOL="pfc"      ; PFC_FLAG="--pfc 1" ;;
  *separate-replay*)PROTOCOL="separate" ; PFC_FLAG="" ;;
  *)
    echo "ERROR: 无法从分支名推断协议，当前分支: $BRANCH"
    echo "支持的分支: dynamic-alpha / cbfc-gbn / pfc-support / separate-replay"
    exit 1
    ;;
esac

OUTDIR="experiments_nvl72/${PROTOCOL}"
mkdir -p "$OUTDIR"
CSV="${OUTDIR}/results.csv"

echo "分支: $BRANCH"
echo "协议: $PROTOCOL"
echo "结果: $CSV"
echo ""

# ── 实验参数 ──────────────────────────────────────────────────
BER_RATES="
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
0.00001
"
MSG_SIZES="16mb 64mb"
TRAFFIC="alltoall"

# 统计总数
total=0
for e in $BER_RATES; do
  [ -z "$e" ] && continue
  for m in $MSG_SIZES; do total=$((total+1)); done
done

done_count=0
fail_count=0

for MSGSIZE in $MSG_SIZES; do
  case $MSGSIZE in
    16mb) SIMTIME=2.0 ;;
    64mb) SIMTIME=5.0 ;;
  esac

  for ERRRATE in $BER_RATES; do
    [ -z "$ERRRATE" ] && continue
    done_count=$((done_count+1))

    TOPO="NVL72_${ERRRATE}_OS2"
    FLOW="flow_${TRAFFIC}_72gpu_${MSGSIZE}"

    if [ ! -f "config/${TOPO}.txt" ]; then
      echo "  SKIP: config/${TOPO}.txt 不存在"
      fail_count=$((fail_count+1))
      continue
    fi

    echo "[${done_count}/${total}] ${PROTOCOL} | NVL72 | ${TRAFFIC} | ${MSGSIZE} | BER=${ERRRATE}"

    TMPLOG=$(mktemp /tmp/sim_XXXXXX.log)
    python3 run.py --topo "$TOPO" --flow "$FLOW" \
                   --simul_time "$SIMTIME" --pool 18432 --credit 128 $PFC_FLAG > "$TMPLOG" 2>&1

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
    if [ "$ACTUAL_FLOW" != "config/${FLOW}.txt" ]; then
      echo "  FAIL: flow不匹配 (实际=${ACTUAL_FLOW})"
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
         avg=s/n/1000; p99=a[int(n*0.99)]/1000; jct=a[n-1]/1000;
         printf "%s,%s,%s,%s,%d,%d,%.2f,%.2f,%.2f\n",
           pr,tr,ms,er,fl,errs,avg,p99,jct
       }' >> "$CSV"

    echo "  OK: id=${CONFIG_ID} flows=${NUM_FLOWS} errors=${NUM_ERRORS} ✓"
  done
done

echo ""
echo "============================================"
echo "${PROTOCOL}: ${done_count} 完成, ${fail_count} 失败"
echo "结果: ${CSV}  ($(wc -l < "$CSV") 行)"
echo "============================================"
