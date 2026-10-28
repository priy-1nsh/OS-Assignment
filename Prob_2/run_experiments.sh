#!/bin/bash
# Usage: ./run_experiments.sh   (needs sudo; run from this directory)
# Produces results.txt with 6 SCHED_RR runs + 1 SCHED_FIFO run per program.
PRIO=20
OUT=results.txt
gcc -Wall -Wextra -O2 -o prog_cpu prog_cpu.c || exit 1
gcc -Wall -Wextra -O2 -o prog_io  prog_io.c  || exit 1
ORIG=$(cat /proc/sys/kernel/sched_rr_timeslice_ms)
: > "$OUT"
for prog in prog_cpu prog_io; do
  for q in 1 10 50 150 300 500; do
    sudo sysctl -w kernel.sched_rr_timeslice_ms=$q > /dev/null
    echo "### sched_rr_timeslice_ms = $q (requested)" | tee -a "$OUT"
    sudo taskset -c 0 chrt -r $PRIO ./$prog | tee -a "$OUT"
  done
  echo "### FIFO run" | tee -a "$OUT"
  sudo timeout 30 taskset -c 0 chrt -f $PRIO ./$prog | tee -a "$OUT"
done
sudo sysctl -w kernel.sched_rr_timeslice_ms=$ORIG > /dev/null
echo "Done. Results in $OUT"
