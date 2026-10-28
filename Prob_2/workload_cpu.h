#ifndef WORKLOAD_CPU_H
#define WORKLOAD_CPU_H
// this serves as an include guard, together with the final #endif.
// Read them as - "if `WORKLOAD_CPU_H` has not been defined yet, define it and process the following code."
// If this header gets included again within the same compilation unit, the macro is already defined, so its contents are skipped. This prevents duplicate definitions.
// These prevent the header’s contents from being processed multiple times in the same compilation unit.

#define CPU_ITERATIONS 150000000L

static void cpu_task(int id)
{
volatile double acc = 0.0;
// Why volatile?
// Without it, the compiler could notice that the final acc is never used and remove the otherwise unobservable calculation : thus an optimizing compiler would remove these calculations.
// volatile requires the relevant accesses to remain observable according to C’s rules, preserving the intended repeated work.
// declaring acc as volatile makes its reads and writes observable under C's rules, so that the compiler can't simply discard all those updates as unused work. Volatile is simply used to preserve the workload.
long i;

(void) id; // explicitly acknowledges that id is unused; this helps avoid an unused-parameter warning.

for (i = 0; i < CPU_ITERATIONS; i++)
acc += (double) i * 1.000001;
}

#endif