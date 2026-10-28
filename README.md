# CS F372: Operating Systems — Assignment 2

## Group Details
| Name | ID Number |
|---|---|
| Paras Balani | 2023B4A70738H |
| Varun Ravichandran | 2023B4A70797H |
| Priyansh Ramrakhyani | 2023B4A70842H |
| Shreya Bhaskar | 2023B4A70970H |

## Repository Structure

```
CS_F372_Assignment2/
├── README.md                          (this file)
├── Makefile                           (builds all problem executables)
├── Prob_1/
│   ├── student_report.c               (multithreaded aggregation, sort & query)
│   ├── input.txt                      (sample marks matrix)
├── Prob_2/
│   ├── prog_cpu.c                     (CPU-bound benchmark program)
│   ├── prog_io.c                      (I/O-bound benchmark program)
│   ├── bench_common.h                 (shared timing, pipe IPC & rusage harness)
│   ├── workload_cpu.h                 (CPU-bound computation workload)
│   ├── workload_io.h                  (I/O-bound disk write/fsync workload)
│   ├── run_experiments.sh             (automated benchmarking script)
│   └── results.txt                    (experimental statistics across all 14 runs)
├── Prob_3/
│   ├── sudoku_analyzer.c              (27 validator threads + 9 heatmap threads)
│   ├── grid_valid.txt                 (valid 9x9 test grid)
│   ├── grid_invalid.txt               (invalid grid with duplicate conflicts)
│   ├── grid_repair.txt                (grid with 1 blank cell for repair advisor)
└── Prob_4/
    
```


## Problem 1: Multithreaded Student Report Card Generator

Reads a subject $\times$ student marks matrix from `input.txt`. In **Phase 1**, the parent spawns one worker thread per student to compute their minimum, maximum, average, and full mark list. In **Phase 2**, the parent partitions the student list into $k$ contiguous segments; $k$ sorting threads sort their segments in-place in descending order by the chosen sort key (breaking ties by lower student ID). The parent joins them and executes a $k$-way merge into a globally sorted array. In **Phase 3**, an interactive query menu answers student lookups and prints sorted rankings.

Concurrency constraint: **Strictly zero synchronization primitives** (no mutexes, semaphores, or condition variables); race-freedom is achieved entirely through disjoint array partitioning.

**Build:**
```bash
cd Prob_1
gcc -Wall -Wextra -O2 -pthread -o student_report student_report.c
```

**Run:**
```bash
./student_report
```

Sample session:
```text
Enter sort key (-3=max, -2=min, -1=avg, 0..3=subject index): -1
Enter number of sorting threads (k): 3
--- Report Card Menu ---
Enter command (1 <id> = view student, 2 = view all, 0 = exit): 1 2
Student ID : 2
Average    : 58.75
Minimum    : 55
Maximum    : 62
Subject Marks: [55, 60, 58, 62]
Enter command (1 <id> = view student, 2 = view all, 0 = exit): 2
s3 90.00
s1 85.00
s5 79.00
s0 66.75
s4 66.75
s2 58.75
Enter command (1 <id> = view student, 2 = view all, 0 = exit): 0
```

---

## Problem 2: Real-Time Scheduling: Round-Robin vs FIFO

Investigates Linux real-time scheduling dynamics (`SCHED_RR` vs `SCHED_FIFO`) under CPU-bound and I/O-bound workloads. Each program forks 8 child processes pinned to CPU Core 0 via `taskset -c 0`. The parent establishes a reference timestamp with `clock_gettime(CLOCK_MONOTONIC)` before forking. Children record their start and end timestamps and transmit them via a pipe. The parent aggregates involuntary and voluntary context switches using `getrusage(RUSAGE_CHILDREN, &usage)` and calculates response time, turnaround time, and throughput.

The experiment evaluates 6 Round-Robin time quantums (1ms, 10ms, 50ms, 150ms, 300ms, 500ms) adjusted via `kernel.sched_rr_timeslice_ms`, plus a `SCHED_FIFO` run capped with `timeout 30`.

**Build:**
```bash
cd Prob_2
gcc -Wall -Wextra -O2 -o prog_cpu prog_cpu.c
gcc -Wall -Wextra -O2 -o prog_io  prog_io.c
```

**Run (Automated 14-run benchmark):**
```bash
sudo bash run_experiments.sh
```

Sample excerpt from `results.txt` (CPU-bound under 1ms vs 500ms quantum):
```text
### sched_rr_timeslice_ms = 1 (requested)
Program-A (CPU-Bound)
--------------------------------------------------
Scheduling Policy: SCHED_RR
Time Quantum: 1ms
  Involuntary Context Switches: 2550
  Voluntary Context Switches: 8
  Average Response Time: 7.40ms
  Maximum Response Time: 31.27ms
  Average Turnaround Time: 4928.45ms
  Maximum Turnaround Time: 6056.86ms
  Throughput: 1.32 children/s
  Total Elapsed Time: 6.06s

### sched_rr_timeslice_ms = 500 (requested)
Program-A (CPU-Bound)
--------------------------------------------------
Scheduling Policy: SCHED_RR
Time Quantum: 500ms
  Involuntary Context Switches: 11
  Voluntary Context Switches: 8
  Average Response Time: 2872.99ms
  Maximum Response Time: 5614.09ms
  Average Turnaround Time: 4518.36ms
  Maximum Turnaround Time: 6406.79ms
  Throughput: 1.25 children/s
  Total Elapsed Time: 6.41s
```

---

## Problem 3: Sudoku Conflict Analyzer and Repair Advisor

A POSIX multithreaded analyzer that validates a $9 \times 9$ Sudoku grid from a text file passed via command line argument (`argv[1]`). 

* **Phase 1 (Validation)**: Spawns 27 worker threads (9 rows, 9 columns, 9 sub-grids). Each thread validates its unit using bitmasks (`missing_mask`, `dup_mask`), records its verdict in `validity[unit_id]`, and returns a heap-allocated `struct unit_result` via `pthread_exit()`. The parent joins all 27 threads and frees their memory.
* **Phase 2 (Analytics)**: If invalid, prints the validity score and spawns 9 analytics worker threads (one per $3 \times 3$ block). Each thread scores its 9 cells based on row, column, and sub-grid duplicate participation (empty cells score 3 outright). The parent outputs the conflict heatmap, lists score-3 hotspots, and generates a missing digit / repair advisor report.

Concurrency constraint: **Strictly zero synchronization primitives**; all shared array accesses (`grid`, `validity`, `dup_mask_g`, `heatmap`) are race-free by disjoint spatial partitioning. Passes `valgrind --leak-check=full` with zero memory leaks.

**Build:**
```bash
cd Prob_3
gcc -Wall -Wextra -O2 -pthread -o sudoku_analyzer sudoku_analyzer.c
```

**Run:**
```bash
# Test a valid grid
./sudoku_analyzer grid_valid.txt

# Test an invalid grid (conflicts & hotspots)
./sudoku_analyzer grid_invalid.txt

# Test repair advisor (with empty cell)
./sudoku_analyzer grid_repair.txt

# Memory leak verification
valgrind --leak-check=full ./sudoku_analyzer grid_invalid.txt
```

Sample session (`grid_invalid.txt`):
```text
Input Grid:
1 2 3 4 5 6 7 8 9
4 5 6 7 8 9 1 2 3
1 8 9 1 2 3 4 5 6
2 3 4 5 6 7 8 9 1
5 6 7 8 9 1 2 3 4
8 9 1 2 3 4 5 6 7
3 4 5 6 7 8 9 1 2
6 7 8 9 1 2 3 4 5
9 1 2 3 4 5 6 7 8

INVALID
Validity Score: 88.89%

Conflict Density Heatmap:
2 0 0 0 0 0 0 0 0
0 0 0 0 0 0 2 0 0
3 0 0 1 0 0 0 0 0
2 0 0 0 0 0 0 0 0
0 0 0 0 0 0 0 0 0
0 0 0 0 0 0 0 0 0
0 0 0 0 0 0 0 0 0
0 0 0 0 0 0 0 0 0
0 0 0 0 0 0 0 0 0

Hotspots (score 3):
  (2,0) value=1
Total Hotspots: 1

Missing Digit Report:
  Row      2 : missing {7}  blanks=0
  Column   0 : missing {7}  blanks=0
  Sub-grid 0 : missing {7}  blanks=0
```

---

## Problem 4: Fork Storm Containment & Tiered CPU Scheduling
