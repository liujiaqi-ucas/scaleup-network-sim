#!/bin/bash
# ==============================================================
# NVL72 alltoall 1MB BER 实验
# 1消息大小 × 10 BER错误率 = 10 组
#
# 用法: bash experiments_nvl72/run_nvl72_1mb.sh <协议名> <simtime_s> [选项]
#
# 参数:
#   <协议名>      输出标识，如 gbn / sr / pfc / separate
#   <simtime_s>   仿真时长（秒）
#   --pfc 0|1     是否启用PFC (default: 0)
#   --pool N      MMU pool大小 flit数 (default: 18432)
#   --credit N    credit_init flit数  (default: 128)
#
# 示例:
#   bash experiments_nvl72/run_nvl72_1mb.sh gbn 0.5
#   bash experiments_nvl72/run_nvl72_1mb.sh pfc 0.5 --pfc 1
#   bash experiments_nvl72/run_nvl72_1mb.sh sr  0.5 --pool 18432 --credit 128
# ==============================================================
cd "$(dirname "$0")/.."

PROTOCOL=${1:?用法: bash experiments_nvl72/run_nvl72_1mb.sh <协议名> <simtime_s> [--pfc 0|1] [--pool N] [--credit N]}
SIMTIME=${2:?缺少 simtime_s 参数}
shift 2

PFC=0
POOL=18432
CREDIT=128

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pfc)    PFC="$2";    shift 2 ;;
    --pool)   POOL="$2";   shift 2 ;;
    --credit) CREDIT="$2"; shift 2 ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

OUTDIR="experiments_nvl72/${PROTOCOL}"
mkdir -p "$OUTDIR"
CSV="${OUTDIR}/results_1mb.csv"

echo "协议: ${PROTOCOL}  PFC=${PFC}  pool=${POOL}  credit=${CREDIT}"
echo "消息: alltoall 1MB  simtime=${SIMTIME}s"
echo "结果: ${CSV}"
echo ""

# ── BER 错误率 (1e-6 ~ 1e-15，共10个) ────────────────────────
BER_RATES="
0.000001
0.0000001
0.00000001
0.000000001
0.0000000001
0.00000000001
0.000000000001
0.0000000000001
0.00000000000001
0.000000000000001
"

MSGSIZE="1mb"
TRAFFIC="alltoall"
FLOW="flow_${TRAFFIC}_72gpu_${MSGSIZE}"

if [ ! -f "config/${FLOW}.txt" ]; then
  echo "ERROR: config/${FLOW}.txt 不存在"; exit 1
fi

total=$(echo "$BER_RATES" | grep -c '[0-9]')
done_count=0
fail_count=0

for ERRRATE in $BER_RATES; do
  [ -z "$ERRRATE" ] && continue
  done_count=$((done_count+1))

  TOPO="NVL72_${ERRRATE}_OS2"
  if [ ! -f "config/${TOPO}.txt" ]; then
    echo "  SKIP: config/${TOPO}.txt 不存在"
    fail_count=$((fail_count+1))
    continue
  fi

  echo "[${done_count}/${total}] ${PROTOCOL} | NVL72 | ${TRAFFIC} | ${MSGSIZE} | BER=${ERRRATE}"

  TMPLOG=$(mktemp /tmp/sim_XXXXXX.log)
  python3 run.py --topo "$TOPO" --flow "$FLOW" \
                 --simul_time "$SIMTIME" --pool "$POOL" --credit "$CREDIT" \
                 --pfc "$PFC" > "$TMPLOG" 2>&1

  CONFIG_ID=$(grep -oP '(?<=/output/)\d{7,12}(?=/)' "$TMPLOG" | tail -1)
  rm -f "$TMPLOG"

  if [ -z "$CONFIG_ID" ]; then
    echo "  FAIL: 无法提取 config_ID"
    fail_count=$((fail_count+1)); continue
  fi

  FCT="mix/output/${CONFIG_ID}/${CONFIG_ID}_out_fct.txt"
  CFG="mix/output/${CONFIG_ID}/config.txt"
  LOG="mix/output/${CONFIG_ID}/config.log"

  ACTUAL_FLOW=$(grep "^FLOW_FILE" "$CFG" | awk '{print $2}')
  if [ "$ACTUAL_FLOW" != "config/${FLOW}.txt" ]; then
    echo "  FAIL: flow不匹配 (实际=${ACTUAL_FLOW})"
    fail_count=$((fail_count+1)); continue
  fi

  if [ ! -f "$FCT" ] || [ "$(wc -l < "$FCT")" -eq 0 ]; then
    echo "  FAIL: FCT 为空 (id=${CONFIG_ID})"
    fail_count=$((fail_count+1)); continue
  fi

  NUM_FLOWS=$(wc -l < "$FCT")
  NUM_ERRORS=$(grep -c "发生了错误" "$LOG" 2>/dev/null || echo 0)
  NUM_RETRANS=$(grep -oP '(?<=\[RETRANS\] total_retrans=)\d+' "$LOG" 2>/dev/null | tail -1)
  NUM_RETRANS="${NUM_RETRANS:-N/A}"

  awk '{print $7}' "$FCT" | sort -n | awk \
    -v pr="$PROTOCOL" -v tr="$TRAFFIC" -v ms="$MSGSIZE" \
    -v er="$ERRRATE"  -v fl="$NUM_FLOWS" \
    -v errs="$NUM_ERRORS" -v retrans="$NUM_RETRANS" \
    'BEGIN{s=0;n=0}
     {a[n]=$1; s+=$1; n++}
     END{
       avg=s/n/1000; p99=a[int(n*0.99)]/1000; jct=a[n-1]/1000;
       printf "%s,%s,%s,%s,%d,%d,%s,%.2f,%.2f,%.2f\n",
         pr,tr,ms,er,fl,errs,retrans,avg,p99,jct
     }' >> "$CSV"

  echo "  OK: id=${CONFIG_ID} flows=${NUM_FLOWS} errors=${NUM_ERRORS} retrans=${NUM_RETRANS} ✓"
done

echo ""
echo "============================================"
echo "${PROTOCOL} 1MB: ${done_count} 完成, ${fail_count} 失败"
echo "CSV格式: protocol,traffic,msgsize,errrate,flows,errors,retrans,avg_fct_us,p99_fct_us,jct_us"
echo "结果: ${CSV}  ($(wc -l < "$CSV" 2>/dev/null || echo 0) 行)"
echo "============================================"
