/* Problem 1: Multithreaded Student Report Card Generator
 * Build: gcc -Wall -Wextra -O2 -pthread -o student_report student_report.c
 * Race-freedom by partitioning only (no mutex/semaphore/condvar):
 *  Phase 1: worker i writes stats[i] only.
 *  Phase 2: sorter t writes only stats[start..start+size) (disjoint segments).
 *  Merge + Phase 3: single-threaded in the parent after pthread_join().
 */
#define _POSIX_C_SOURCE 200809L  // feature-test macro : asks supporting system headers to expose interfaces associated with the selected POSIX standard

// This tells the system headers: **“Make the POSIX.1-2008 functions and definitions available to my program.”**

// POSIX is a standard for operating-system interfaces, such as threads, processes, and files. Some of these interfaces are hidden by default in certain compilation modes; defining this macro lets the headers expose their declarations.

// Breaking it down:

// - `_POSIX_C_SOURCE` is a **feature-test macro** recognized by system headers.
// - `200809` selects the POSIX.1-2008 standard.
// - `L` makes the number a `long` integer literal.

// It must appear **before any `#include`**, because the headers check its value while being processed.

// **It does nothing at runtime.** It controls which interfaces your code can see during compilation; it doesn’t install or implement those functions.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAX_SUBJECTS 32
#define MAX_STUDENTS 128

typedef struct {
    int student_id;
    double average;
    int minimum;
    int maximum;
    int subject_marks[MAX_SUBJECTS];
} student_stats;

struct thread_params {
    int thread_id;
    int start_index;
    int segment_size;
};

static int num_subjects, num_students;
static int marks[MAX_SUBJECTS][MAX_STUDENTS];   /* marks[subject][student] */
static student_stats stats[MAX_STUDENTS];       /* phase 1 / segment-sorted */
static student_stats sorted_stats[MAX_STUDENTS];/* globally sorted (merge)  */
static int sort_key;                            /* read-only during phase 2 */

static double key_value(const student_stats *s) // pass by reference
{
    if (sort_key == -3) return s->maximum;
    if (sort_key == -2) return s->minimum;
    if (sort_key == -1) return s->average;
    return s->subject_marks[sort_key];
}

/* true if a must be placed before b (descending, tie -> lower id first) */
static int before(const student_stats *a, const student_stats *b)
{
    double va = key_value(a), vb = key_value(b);
    if (va > vb) return 1;
    if (va < vb) return 0;
    return a->student_id < b->student_id;
}

/* ---------- Phase 1 ---------- */
static void *aggregate_worker(void *arg)
{
    int i = *(int *)arg;
    student_stats *s = &stats[i];
    long sum = 0;
    s->student_id = i;
    s->minimum = marks[0][i];
    s->maximum = marks[0][i];
    for (int j = 0; j < num_subjects; j++) {
        int m = marks[j][i];
        s->subject_marks[j] = m;
        sum += m;
        if (m < s->minimum) s->minimum = m;
        if (m > s->maximum) s->maximum = m;
    }
    s->average = (double)sum / num_subjects;
    pthread_exit(NULL);
}

/* ---------- Phase 2 ---------- */
// Line by line:
// - 79: Begin with the second record; a one-record prefix is already sorted.
// - 80: Copy the entire current record into temporary variable t.
// - 81: Start comparing with the record immediately to its left.
// - 82: Continue while still inside the segment and t belongs before the current record.
// - 83: Move that record one position right.
// - 84: Move left to inspect the next record.
// - 86: Place the saved record into the gap.
// - 88: End the sorting worker.
// INSERTION SORT
static void *sort_worker(void *arg)
{
    struct thread_params *p = arg;
    int start = p->start_index, end = p->start_index + p->segment_size;
    for (int i = start + 1; i < end; i++) {          /* insertion sort */
        student_stats t = stats[i];
        int j = i - 1;
        while (j >= start && before(&t, &stats[j])) {
            stats[j + 1] = stats[j];
            j--;
        }
        stats[j + 1] = t;
    }
    pthread_exit(NULL);
}

static void kway_merge(int k, int seg) // seg is the segment size, example : k = 3 segments (0 , 1, 2); seg = 2 size each segment.
{
    int head[MAX_STUDENTS];
    // head[t] is the position of the next unused record in the segment t : these are positions in stats array, not student IDs
    for (int t = 0; t < k; t++) head[t] = t * seg; // (0, 2, 4)
    for (int out = 0; out < num_students; out++) {
        int best = -1;
        // best will store the index of the 'segment' containing the best available head. -1 means no candidate has been selected yet.
        for (int t = 0; t < k; t++) {
            if (head[t] >= (t + 1) * seg) continue; // segment exhausted, move to the next segment
            if (best < 0 || before(&stats[head[t]], &stats[head[best]]))
                best = t;
        }
        sorted_stats[out] = stats[head[best]++];
    }
}

static int read_input(const char *path)
// The function receives a filename and returns 0 for success or -1 for failure.
{
    FILE *f = fopen(path, "r"); 
    // FILE * refers to a C library file stream. "r" opens the file for reading.
    static int vals[2 + MAX_SUBJECTS * MAX_STUDENTS]; // temporarily hold all the parsed integers, extra two positions for the dimensions
    int n = 0; // n counts stored values.
    char line[1024]; // holds one chunk of text read from the file
    if (!f) { perror(path); return -1; }
    while (fgets(line, sizeof line, f)) {
        // fgets() reads up to the buffer limit, stopping earlier at a newline or end of file. It adds a terminating null character on a successful read.

        // now, this code reads from a string called line, ignoring everything after //. and stores them in the array vals.
        char *c = strstr(line, "//");           /* tolerate // comments */
        // strstr searches for the first occurence of a substring. If "//" is found, it returns a pointer to its first /, otherwise it returns NULL.
        if (c) *c = '\0';

        // pointers for parsing
        char *p = line, *e;
        for (;;) {
            long v = strtol(p, &e, 10); // string to long, skipping whitespaces 
            if (e == p) break;
            if (n >= (int)(sizeof vals / sizeof vals[0])) { fclose(f); return -1; }
            vals[n++] = (int)v;
            p = e;
        }
    }
    fclose(f);
    if (n < 2) return -1;
    num_subjects = vals[0];
    num_students = vals[1];
    if (num_subjects < 1 || num_subjects > MAX_SUBJECTS ||
        num_students < 1 || num_students > MAX_STUDENTS ||
        n < 2 + num_subjects * num_students) {
        fprintf(stderr, "input.txt: bad dimensions or too few marks\n"); // stderr = output stream conventionally used for errors.
        return -1;
    }
    for (int i = 0; i < num_subjects; i++)
        for (int j = 0; j < num_students; j++)
            marks[i][j] = vals[2 + i * num_students + j];
    return 0; // function returns 0 on success and -1 on failure.
}

// Average is displayed with two digits after the decimal point.
static void print_key(const student_stats *s)
{
    if (sort_key == -1) printf("%.2f", s->average);
    else printf("%d", (int)key_value(s));
}

int main(void)
{
    pthread_t tid[MAX_STUDENTS];
    int ids[MAX_STUDENTS]; // ids provides stable argument storage for aggregation workers.
    struct thread_params params[MAX_STUDENTS]; // params provides stable argument storage for sorting workers.
    int k;

    if (read_input("input.txt") != 0) return 1; // If reading fails, terminate with a nonzero process exit status, conventionally indicating failure.

    for (int i = 0; i < num_students; i++) {            /* Phase 1 */
        ids[i] = i;
        if (pthread_create(&tid[i], NULL, aggregate_worker, &ids[i]) != 0) {
            perror("pthread_create"); return 1;
        }
    }
    for (int i = 0; i < num_students; i++) pthread_join(tid[i], NULL);

    for (;;) {                                           /* Phase 2 prompts */
        printf("Enter sort key (-3=max, -2=min, -1=avg, 0..%d=subject index): ",
               num_subjects - 1);
        if (scanf("%d", &sort_key) != 1) return 1;
        if (sort_key >= -3 && sort_key < num_subjects) break;
        printf("Invalid sort key.\n");
    }
    for (;;) {
        printf("Enter number of sorting threads (k): ");
        if (scanf("%d", &k) != 1) return 1;
        if (k >= 1 && k <= num_students && num_students % k == 0) break;
        printf("k must satisfy 1 <= k <= %d and divide %d.\n",
               num_students, num_students);
    }

    int seg = num_students / k;
    for (int t = 0; t < k; t++) {
        params[t].thread_id = t;
        params[t].start_index = t * seg;
        params[t].segment_size = seg;
        if (pthread_create(&tid[t], NULL, sort_worker, &params[t]) != 0) {
            perror("pthread_create"); return 1;
        }
    }
    for (int t = 0; t < k; t++) pthread_join(tid[t], NULL);
    kway_merge(k, seg);

    printf("--- Report Card Menu ---\n");                /* Phase 3 */
    for (;;) {
        int cmd;
        printf("Enter command (1 <id> = view student, 2 = view all, 0 = exit): ");
        if (scanf("%d", &cmd) != 1) break;
        if (cmd == 0) break;
        if (cmd == 1) {
            int id;
            if (scanf("%d", &id) != 1) break;
            const student_stats *s = NULL;
            for (int i = 0; i < num_students; i++)
                if (sorted_stats[i].student_id == id) s = &sorted_stats[i];
            if (!s) { printf("No such student.\n"); continue; }
            printf("Student ID : %d\nAverage    : %.2f\nMinimum    : %d\nMaximum    : %d\n",
                   s->student_id, s->average, s->minimum, s->maximum);
            printf("Subject Marks: [");
            for (int j = 0; j < num_subjects; j++)
                printf("%s%d", j ? ", " : "", s->subject_marks[j]);
            printf("]\n");
        } else if (cmd == 2) {
            for (int i = 0; i < num_students; i++) {
                printf("s%-3d ", sorted_stats[i].student_id);
                print_key(&sorted_stats[i]);
                printf("\n");
            }
        } else {
            printf("Unknown command.\n");
        }
    }
    return 0;
}

// One small issue in this code: pthread_create returns an error number directly, so perror may print an unrelated message. The usual error-reporting pattern is:
// int err = pthread_create(&tid[i], NULL, aggregate_worker, &ids[i]);
// if (err != 0) { // 0 means success
//     fprintf(stderr, "pthread_create: %s\n", strerror(err));
//     return 1;
// }


// Operation	Cost :-
// Read the marks	O(MN)
// Aggregate one student	O(M)
// Aggregate all students, total work	O(MN)
// Insertion-sort one segment, worst case	O((N/k)²)
// All segment sorts, total worst-case work	O(N²/k)
// Your scan-based k-way merge	O(Nk)
// Look up one student	O(N) search, plus O(M) printing
// Print the ranking	O(N)