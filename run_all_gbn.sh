#!/bin/bash
ERRS="0.00001 0.00005 0.0001 0.0005 0.001"
SIZES="1mb 4mb 16mb 64mb"
TRAFFICS="alltoall allreduce"
for traffic in $TRAFFICS; do
  for size in $SIZES; do
    for err in $ERRS; do
      outfile="experiments/gbn/${traffic}_${size}_${err}.csv"
      if [ -f "$outfile" ] && [ -s "$outfile" ]; then
        echo "[SKIP] gbn/$traffic/$size/$err"; continue
      fi
      bash run_one_exp.sh gbn $traffic $size $err
    done
  done
done
echo "=== All GBN experiments done ==="
