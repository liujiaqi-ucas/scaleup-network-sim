#!/bin/bash
# 顺序跑所有 SR 实验
ERRS="0.00001 0.00005 0.0001 0.0005 0.001"
SIZES="1mb 4mb 16mb 64mb"
TRAFFICS="alltoall allreduce"

for traffic in $TRAFFICS; do
  for size in $SIZES; do
    for err in $ERRS; do
      outfile="experiments/sr/${traffic}_${size}_${err}.csv"
      if [ -f "$outfile" ] && [ -s "$outfile" ]; then
        echo "[SKIP] $traffic/$size/$err already done"
        continue
      fi
      bash run_one_exp.sh sr $traffic $size $err
    done
  done
done
echo "=== All SR experiments done ==="
