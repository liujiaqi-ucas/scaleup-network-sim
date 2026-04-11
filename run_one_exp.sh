#!/bin/bash
# 运行单个实验并保存结果
# 用法: bash run_one_exp.sh <protocol> <traffic> <msgsize> <errrate>

PROTOCOL=$1
TRAFFIC=$2
MSGSIZE=$3
ERRRATE=$4

TOPO="H100_8_${ERRRATE}_OS2"
FLOW="flow_${TRAFFIC}_8gpu_${MSGSIZE}"
OUTDIR="experiments/${PROTOCOL}"
OUTFILE="${OUTDIR}/${TRAFFIC}_${MSGSIZE}_${ERRRATE}.csv"

case $MSGSIZE in
  1mb)  SIMTIME=0.1 ;;
  4mb)  SIMTIME=0.1 ;;
  16mb) SIMTIME=0.2 ;;
  64mb) SIMTIME=0.5 ;;
  *) SIMTIME=0.2 ;;
esac

echo "[$(date +%H:%M:%S)] Running: $PROTOCOL $TRAFFIC $MSGSIZE err=$ERRRATE"

TMPLOG=$(mktemp /tmp/exp_XXXXXX.log)

# 运行仿真，捕获全部 stdout 到临时文件
python3 run.py --topo $TOPO --flow $FLOW --simul_time $SIMTIME > "$TMPLOG" 2>&1

# 从日志提取 config_ID（找"The new directory is created"那行里的数字）
CONFIG_ID=$(grep "The new directory is created" "$TMPLOG" | grep -oP '(?<=/output/)\d+' | head -1)

rm -f "$TMPLOG"

if [ -z "$CONFIG_ID" ]; then
  echo "ERROR: Cannot find config_ID for $PROTOCOL $TRAFFIC $MSGSIZE $ERRRATE"
  exit 1
fi

FCT_FILE="mix/output/${CONFIG_ID}/${CONFIG_ID}_out_fct.txt"

if [ ! -f "$FCT_FILE" ] || [ $(wc -l < "$FCT_FILE") -eq 0 ]; then
  echo "ERROR: No FCT output at $FCT_FILE"
  exit 1
fi

FLOWS=$(wc -l < "$FCT_FILE")
ERRORS=$(grep -c "发生了错误" "mix/output/${CONFIG_ID}/config.log" 2>/dev/null || echo 0)

awk '{print $7}' "$FCT_FILE" | sort -n | awk \
  -v prot="$PROTOCOL" -v traf="$TRAFFIC" -v msg="$MSGSIZE" \
  -v err="$ERRRATE" -v errs="$ERRORS" -v flows="$FLOWS" \
  'BEGIN{s=0;n=0}{a[n]=$1;s+=$1;n++}END{
    avg=s/n/1000; p99=a[int(n*0.99)]/1000; jct=a[n-1]/1000;
    printf "%s,%s,%s,%s,%d,%d,%.1f,%.1f,%.1f\n",
      prot,traf,msg,err,flows,errs,avg,p99,jct
  }' >> "$OUTFILE"

echo "[$(date +%H:%M:%S)] Done: id=$CONFIG_ID flows=$FLOWS err=$ERRORS -> $OUTFILE"
