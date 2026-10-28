#include "bench_common.h"
#include "workload_io.h"

int main(void)
{
    /* Runs 8 children, each executing io_task from workload_io.h */
    return run_benchmark("Program-B (I/O-Bound)", io_task);
}
