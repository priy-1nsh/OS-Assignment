#include "bench_common.h"
#include "workload_cpu.h"

int main(void)
{
    /* Runs 8 children, each executing cpu_task from workload_cpu.h */
    return run_benchmark("Program-A (CPU-Bound)", cpu_task);
}
