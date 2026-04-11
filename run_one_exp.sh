#!/bin/bash
# 运行单个实验并追加结果到 CSV
# 用法: bash run_one_exp.sh <protocol> <traffic> <msgsize> <errrate>
PROTOCOL=$1 TRAFFIC=$2 MSGSIZE=$3 ERRRATE=$4

TOPO="H100_8_${ERRRATE}_OS2"
FLOW="flow_${TRAFFIC}_8gpu_${MSGSIZE}"
OUTFILE="experiments/${PROTOCOL}/${TRAFFIC}_${MSGSIZE}_${ERRRATE}.csv"

case $MSGSIZE in
  1mb)  SIMTIME=0.1 ;;
  4mb)  SIMTIME=0.1 ;;
  16mb) SIMTIME=0.2 ;;
  64mb) SIMTIME=0.5 ;;
  *)    SIMTIME=0.2 ;;
esac

echo "[$(date +%H:%M:%S)] START $PROTOCOL/$TRAFFIC/$MSGSIZE/err=$ERRRATE"

# 捕获全部输出到临时文件
TMPLOG=$(mktemp /tmp/sim_XXXXXX.log)
python3 run.py --topo $TOPO --flow $FLOW --simul_time $SIMTIME > "$TMPLOG" 2>&1

# 从日志提取 config_ID（格式: .../mix/output/NNNNN/）
CONFIG_ID=$(grep -oP '(?<=/output/)\d{7,12}(?=/)' "$TMPLOG" | tail -1)
rm -f "$TMPLOG"

if [ -z "$CONFIG_ID" ]; then
  echo "ERROR: No config_ID found" && exit 1
fi

FCT="mix/output/${CONFIG_ID}/${CONFIG_ID}_out_fct.txt"
if [ ! -f "$FCT" ] || [ $(wc -l < "$FCT") -eq 0 ]; then
  echo "ERROR: No FCT file at $FCT" && exit 1
fi

FLOWS=$(wc -l < "$FCT")
ERRS=$(grep -c "发生了错误" "mix/output/${CONFIG_ID}/config.log" 2>/dev/null || echo 0)

awk '{print $7}' "$FCT" | sort -n | awk \
  -v pr="$PROTOCOL" -v tr="$TRAFFIC" -v ms="$MSGSIZE" \
  -v er="$ERRRATE" -v errs="$ERRS" -v fl="$FLOWS" \
  'BEGIN{s=0;n=0}{a[n]=$1;s+=$1;n++}END{
    printf "%s,%s,%s,%s,%d,%d,%.1f,%.1f,%.1f\n",
      pr,tr,ms,er,fl,errs,s/n/1000,a[int(n*0.99)]/1000,a[n-1]/1000
  }' >> "$OUTFILE"

echo "[$(date +%H:%M:%S)] DONE  id=$CONFIG_ID flows=$FLOWS errs=$ERRS → $OUTFILE"
