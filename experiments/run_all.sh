#!/bin/bash
# ==============================================================
# GBN vs SR 全量对比实验 (顺序执行)
# 用法: bash experiments/run_all.sh <sr|gbn>
# 输出: experiments/<protocol>/results.csv (覆盖式)
# CSV 格式与 plot_results.py 兼容:
#   protocol,traffic,msgsize,errrate,flows,error_pkts,avg_fct_us,p99_fct_us,jct_us
# ==============================================================
set -e
cd "$(dirname "$0")/.."

PROTOCOL=${1:?用法: bash experiments/run_all.sh <sr|gbn>}
OUTDIR="experiments/${PROTOCOL}"
mkdir -p "$OUTDIR"
CSV="${OUTDIR}/results.csv"

ERR_RATES="0.00001 0.00005 0.0001 0.0005 0.001"
MSG_SIZES="1mb 4mb 16mb 64mb"
TRAFFICS="allreduce alltoall"

# 清空旧结果
> "$CSV"

total=0
for e in $ERR_RATES; do for m in $MSG_SIZES; do for t in $TRAFFICS; do
  total=$((total+1))
done; done; done

done_count=0
fail_count=0

for ERRRATE in $ERR_RATES; do
  for MSGSIZE in $MSG_SIZES; do
    for TRAFFIC in $TRAFFICS; do
      done_count=$((done_count+1))
      TOPO="H100_8_${ERRRATE}_OS2"
      FLOW="flow_${TRAFFIC}_8gpu_${MSGSIZE}"

      case $MSGSIZE in
        1mb)  SIMTIME=0.1 ;;
        4mb)  SIMTIME=0.1 ;;
        16mb) SIMTIME=0.2 ;;
        64mb) SIMTIME=0.5 ;;
        *)    SIMTIME=0.2 ;;
      esac

      echo ""
      echo "[${done_count}/${total}] ${PROTOCOL} | ${TRAFFIC} | ${MSGSIZE} | err=${ERRRATE} | simT=${SIMTIME}s"

      TMPLOG=$(mktemp /tmp/sim_XXXXXX.log)
      python3 run.py --topo "$TOPO" --flow "$FLOW" --simul_time "$SIMTIME" > "$TMPLOG" 2>&1
      RC=$?

      CONFIG_ID=$(grep -oP '(?<=/output/)\d{7,12}(?=/)' "$TMPLOG" | tail -1)
      rm -f "$TMPLOG"

      if [ -z "$CONFIG_ID" ]; then
        echo "  ERROR: 无法提取 config_ID (rc=$RC)"
        fail_count=$((fail_count+1))
        continue
      fi

      FCT="mix/output/${CONFIG_ID}/${CONFIG_ID}_out_fct.txt"
      LOG="mix/output/${CONFIG_ID}/config.log"

      if [ ! -f "$FCT" ] || [ "$(wc -l < "$FCT")" -eq 0 ]; then
        echo "  ERROR: FCT 为空 (id=${CONFIG_ID})"
        fail_count=$((fail_count+1))
        continue
      fi

      NUM_FLOWS=$(wc -l < "$FCT")
      NUM_ERRORS=$(grep -c "发生了错误" "$LOG" 2>/dev/null || echo 0)

      # 计算 avg / p99 / max(=JCT)，单位 ns -> us
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

      echo "  OK: id=${CONFIG_ID} flows=${NUM_FLOWS} errors=${NUM_ERRORS}"
    done
  done
done

echo ""
echo "============================================"
echo "${PROTOCOL}: ${done_count} 组实验完成 (${fail_count} 失败)"
echo "结果: ${CSV}"
echo "============================================"
