#!/bin/bash
# ==============================================================
# 补充单个 BER 错误率实验，追加到已有 CSV
# 用法: bash experiments_h100_ber/run_extra_ber.sh <sr|gbn|pfc|separate> <BER>
# 示例: bash experiments_h100_ber/run_extra_ber.sh sr 0.00001
# ==============================================================
cd "$(dirname "$0")/.."

PROTOCOL=${1:?用法: bash experiments_h100_ber/run_extra_ber.sh <sr|gbn|pfc|separate> <BER>}
ERRRATE=${2:?缺少 BER 参数，例如: 0.00001}

CSV="experiments_h100_ber/${PROTOCOL}/results.csv"
if [ ! -f "$CSV" ]; then
  echo "ERROR: $CSV 不存在，请先运行主实验脚本"
  exit 1
fi

TOPO="H100_8_${ERRRATE}_OS2"
if [ ! -f "config/${TOPO}.txt" ]; then
  echo "ERROR: config/${TOPO}.txt 不存在"
  exit 1
fi

MSG_SIZES="1mb 4mb 16mb 64mb 128mb 256mb"
TRAFFICS="allreduce alltoall"
PFC_FLAG=""
if [ "$PROTOCOL" = "pfc" ]; then PFC_FLAG="--pfc 1"; fi

total=12
done_count=0
fail_count=0

echo "协议: $PROTOCOL | BER: $ERRRATE | 共 $total 组"
echo "追加到: $CSV"
echo ""

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
    esac

    echo "[${done_count}/${total}] ${TRAFFIC} | ${MSGSIZE} | BER=${ERRRATE}"

    TMPLOG=$(mktemp /tmp/sim_XXXXXX.log)
    python3 run.py --topo "$TOPO" --flow "$FLOW" --simul_time "$SIMTIME" $PFC_FLAG > "$TMPLOG" 2>&1

    CONFIG_ID=$(grep -oP '(?<=/output/)\d{7,12}(?=/)' "$TMPLOG" | tail -1)
    rm -f "$TMPLOG"

    if [ -z "$CONFIG_ID" ]; then
      echo "  FAIL: 无法提取 config_ID"; fail_count=$((fail_count+1)); continue
    fi

    FCT="mix/output/${CONFIG_ID}/${CONFIG_ID}_out_fct.txt"
    CFG="mix/output/${CONFIG_ID}/config.txt"
    LOG="mix/output/${CONFIG_ID}/config.log"

    ACTUAL_FLOW=$(grep "^FLOW_FILE" "$CFG" | awk '{print $2}')
    if [ "$ACTUAL_FLOW" != "config/${FLOW}.txt" ]; then
      echo "  FAIL: flow不匹配"; fail_count=$((fail_count+1)); continue
    fi

    if [ ! -f "$FCT" ] || [ "$(wc -l < "$FCT")" -eq 0 ]; then
      echo "  FAIL: FCT 为空"; fail_count=$((fail_count+1)); continue
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

    echo "  OK: flows=${NUM_FLOWS} errors=${NUM_ERRORS} ✓"
  done
done

echo ""
echo "完成: ${done_count} 组, ${fail_count} 失败"
echo "CSV 现在有 $(wc -l < $CSV) 行"
