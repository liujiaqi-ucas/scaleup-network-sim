#!/bin/bash
# ==============================================================
# 分离架构 H100_8 BER 实验 (feature/separate-replay 分支)
# 6消息大小 × 2流量类型 × 10 BER错误率 = 120 组
# 存储配置: pool=6144/switch + replay=256×8端口=2048/switch → 总8192/switch
#           统一架构 pool=6144/switch → 总6144/switch (少25%)
# 用法: bash experiments_h100_ber/run_separate.sh
# ==============================================================
cd "$(dirname "$0")/.."

OUTDIR="experiments_h100_ber/separate"
mkdir -p "$OUTDIR"
CSV="${OUTDIR}/results.csv"

# 10 个 BER 错误率 (高→低)
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

MSG_SIZES="1mb 4mb 16mb 64mb 128mb 256mb"
TRAFFICS="allreduce alltoall"

total=0
for e in $BER_RATES; do
  [ -z "$e" ] && continue
  for m in $MSG_SIZES; do for t in $TRAFFICS; do
    total=$((total+1))
  done; done
done

> "$CSV"
done_count=0
fail_count=0

for ERRRATE in $BER_RATES; do
  [ -z "$ERRRATE" ] && continue

  TOPO="H100_8_${ERRRATE}_OS2"
  if [ ! -f "config/${TOPO}.txt" ]; then
    echo "  SKIP: config/${TOPO}.txt 不存在"
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
      echo "[${done_count}/${total}] separate | ${TRAFFIC} | ${MSGSIZE} | BER=${ERRRATE}"

      TMPLOG=$(mktemp /tmp/sim_XXXXXX.log)
      python3 run.py --topo "$TOPO" --flow "$FLOW" --simul_time "$SIMTIME" --pool 4096 --credit 256 > "$TMPLOG" 2>&1

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
        -v pr="separate" -v tr="$TRAFFIC" -v ms="$MSGSIZE" \
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
echo "separate: ${done_count} 完成, ${fail_count} 失败"
echo "结果: ${CSV}"
echo "============================================"
