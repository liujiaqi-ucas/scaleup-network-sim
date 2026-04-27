#!/bin/bash
# ==============================================================
# H100_8 BER 通用实验脚本 — 四个分支均可使用
#
# 9消息大小 × 2流量类型 × 10 BER错误率 = 180 组/次
#
# 用法:
#   bash experiments_h100_ber/run_all.sh <协议名> [选项]
#
# 必填:
#   <协议名>   输出目录与CSV标识，例如: gbn / sr / pfc / separate
#
# 可选:
#   --pfc 0|1        是否启用PFC (default: 0，pfc分支传 1)
#   --pool <n>       MMU pool大小 flit数 (default: 4096)
#   --credit <n>     credit_init flit数  (default: 256)
#   --outdir <dir>   输出目录 (default: experiments_h100_ber/<协议名>)
#
# 示例:
#   bash experiments_h100_ber/run_all.sh gbn
#   bash experiments_h100_ber/run_all.sh sr
#   bash experiments_h100_ber/run_all.sh pfc --pfc 1
#   bash experiments_h100_ber/run_all.sh separate --pool 4096 --credit 256
# ==============================================================
cd "$(dirname "$0")/.."

# ---- 参数解析 ------------------------------------------------
PROTOCOL=${1:?用法: bash experiments_h100_ber/run_all.sh <协议名> [--pfc 0|1] [--pool N] [--credit N] [--outdir DIR]}
shift

PFC=0
POOL=4096
CREDIT=256
OUTDIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pfc)    PFC="$2";    shift 2 ;;
    --pool)   POOL="$2";   shift 2 ;;
    --credit) CREDIT="$2"; shift 2 ;;
    --outdir) OUTDIR="$2"; shift 2 ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

[ -z "$OUTDIR" ] && OUTDIR="experiments_h100_ber/${PROTOCOL}"
mkdir -p "$OUTDIR"
CSV="${OUTDIR}/results.csv"

# ---- 实验矩阵 ------------------------------------------------
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

MSG_SIZES="0.5mb 1mb 4mb 16mb 64mb 128mb 256mb 512mb 1024mb"
TRAFFICS="allreduce alltoall"

# simtime: 流量全部完成所需的仿真时长 (秒)
simtime_of() {
  case "$1" in
    0.5mb)  echo 0.05 ;;
    1mb)    echo 0.1  ;;
    4mb)    echo 0.1  ;;
    16mb)   echo 0.2  ;;
    64mb)   echo 0.5  ;;
    128mb)  echo 1.0  ;;
    256mb)  echo 2.0  ;;
    512mb)  echo 4.0  ;;
    1024mb) echo 8.0  ;;
    *)      echo 0.2  ;;
  esac
}

# ---- 统计总实验数 --------------------------------------------
total=0
for e in $BER_RATES; do
  [ -z "$e" ] && continue
  for m in $MSG_SIZES; do
    for t in $TRAFFICS; do
      total=$((total+1))
    done
  done
done

echo "协议: ${PROTOCOL}  PFC=${PFC}  pool=${POOL}  credit=${CREDIT}"
NUM_BER=$(echo "$BER_RATES" | grep -c '[0-9]')
echo "实验总数: ${total} (${NUM_BER} BER × 9 消息 × 2 流量)"
echo "输出: ${CSV}"
echo ""

# ---- CSV 表头 ------------------------------------------------
# 带重传计数的分支 (cbfc-gbn / dynamic-alpha): 10列含retrans
# 不带重传计数的分支 (pfc-support / separate-replay): 9列, retrans=N/A
> "$CSV"
done_count=0
fail_count=0

# ---- 主循环 --------------------------------------------------
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
      SIMTIME=$(simtime_of "$MSGSIZE")

      # 检查 flow 文件是否存在
      if [ ! -f "config/${FLOW}.txt" ]; then
        echo "  SKIP: config/${FLOW}.txt 不存在"
        continue
      fi

      echo ""
      echo "[${done_count}/${total}] ${PROTOCOL} | ${TRAFFIC} | ${MSGSIZE} | BER=${ERRRATE}"

      # 组装 run.py 命令（pfc 分支多传 --pfc 1）
      TMPLOG=$(mktemp /tmp/sim_XXXXXX.log)
      python3 run.py \
        --topo "$TOPO" \
        --flow "$FLOW" \
        --simul_time "$SIMTIME" \
        --pool "$POOL" \
        --credit "$CREDIT" \
        --pfc "$PFC" \
        > "$TMPLOG" 2>&1

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

      # 验证 flow 文件一致性
      ACTUAL_FLOW=$(grep "^FLOW_FILE" "$CFG" 2>/dev/null | awk '{print $2}')
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

      # 重传计数：有 [RETRANS] 行则读取，否则为 N/A
      NUM_RETRANS=$(grep -oP '(?<=\[RETRANS\] total_retrans=)\d+' "$LOG" 2>/dev/null | tail -1)
      NUM_RETRANS="${NUM_RETRANS:-N/A}"

      awk '{print $7}' "$FCT" | sort -n | awk \
        -v pr="$PROTOCOL" -v tr="$TRAFFIC" -v ms="$MSGSIZE" \
        -v er="$ERRRATE"  -v fl="$NUM_FLOWS" \
        -v errs="$NUM_ERRORS" -v retrans="$NUM_RETRANS" \
        'BEGIN{s=0;n=0}
         {a[n]=$1; s+=$1; n++}
         END{
           avg = s/n/1000;
           p99 = a[int(n*0.99)]/1000;
           jct = a[n-1]/1000;
           printf "%s,%s,%s,%s,%d,%d,%s,%.2f,%.2f,%.2f\n",
             pr, tr, ms, er, fl, errs, retrans, avg, p99, jct
         }' >> "$CSV"

      echo "  OK: id=${CONFIG_ID} flows=${NUM_FLOWS} errors=${NUM_ERRORS} retrans=${NUM_RETRANS} ✓"
    done
  done
done

echo ""
echo "============================================================"
echo "${PROTOCOL}: ${done_count} 完成, ${fail_count} 失败"
echo "CSV格式: protocol,traffic,msgsize,errrate,flows,errors,retrans,avg_fct_us,p99_fct_us,jct_us"
echo "结果: ${CSV}"
echo "============================================================"
