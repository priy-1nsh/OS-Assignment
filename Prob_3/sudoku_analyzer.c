/* Problem 3: Sudoku Conflict Analyzer and Repair Advisor
 * Build: gcc -Wall -Wextra -O2 -pthread -o sudoku_analyzer sudoku_analyzer.c
 * Usage: ./sudoku_analyzer grid.txt
 *
 * Why every shared array is race-free (no locks used):
 *  grid[][]      : written once by the parent before any thread exists; read-only after.
 *  validity[27]  : validator thread u writes ONLY validity[u] (disjoint indices).
 *  dup_mask_g[27]: written by the parent after all validators were joined; read-only
 *                  for the analytics threads.
 *  heatmap[][]   : analytics thread b writes ONLY the 9 cells of its own 3x3 block
 *                  (disjoint); parent reads it after joining them.
 *  unit_result   : malloc'ed privately by its worker, handed over via pthread_exit/join.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>

#define N 9
#define NUNITS 27
#define VALID 1
#define INVALID 0

static int grid[N][N];
static int validity[NUNITS]; // 27 units are there
static int dup_mask_g[NUNITS]; // stores which digits are duplicated in unit u
static int heatmap[N][N]; // stores a conflict score (0-3) for a cell

struct unit_params { int unit_id; }; // which unit to inspect
struct unit_result { // validator's detailed result
    int unit_id;
    int valid;
    int missing_mask; // simply a compact set of missing digits
    int dup_mask; // compact set of duplicated digits
    int blank_count;
};
struct block_params { int block_id; }; // argument for the analytics thread which answers which 3X3 block to inspect.

/* k-th cell (0..8) of unit u -> (r,c) */
// find the cell belonging to that unit
// what are the row and col of the kth cell in unit u
static void unit_cell(int u, int k, int *r, int *c)
{
    if (u < 9)       { *r = u;      *c = k; } // rows
    else if (u < 18) { *r = k;      *c = u - 9; } // columns
    else {
        int s = u - 18;
        *r = (s / 3) * 3 + k / 3;
        *c = (s % 3) * 3 + k % 3;
    }
}

// validate all the 27 units and update the validity array and returns the unit_result pointers
static void *validate_worker(void *arg)
{
    const struct unit_params *p = arg; // containing unit_id
    int u = p->unit_id, cnt[10] = {0}, blanks = 0;
    // cnt[10] - number of occurences of digit 1 .. 9
    struct unit_result *res = malloc(sizeof *res); // allocating memory because the result must remain available after the worker finishes.

    for (int k = 0; k < N; k++) {
        int r, c;
        unit_cell(u, k, &r, &c);
        int v = grid[r][c];
        if (v == 0) blanks++; else cnt[v]++;
    }
    if (!res) pthread_exit(NULL); // allocation failed, thread returns no result
    res->unit_id = u;
    res->missing_mask = res->dup_mask = 0;
    for (int d = 1; d <= 9; d++) {
        if (cnt[d] == 0) res->missing_mask |= 1 << d;
        if (cnt[d] >= 2) res->dup_mask |= 1 << d;
    }
    res->blank_count = blanks;
    res->valid = (!res->missing_mask && !res->dup_mask && !blanks) ? VALID : INVALID; // unit is valid only when no digit is missing, no digit is duplicated, and no cell is blank
    validity[u] = res->valid;               /* own index only */
    pthread_exit(res); // returns the detailed result pointer to the thread that joins it
}

// assign conflict scores
static void *analytics_worker(void *arg)
{
    const struct block_params *p = arg; // receives the block id (0-8)
    int br = (p->block_id / 3) * 3, bc = (p->block_id % 3) * 3; // br, bc = top-left corner
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int r = br + i, c = bc + j, v = grid[r][c], score;
            if (v == 0) score = 3;
            else {
                // for a filled cell, the score counts how many of these contain a duplicate of its value : it's row, it's col, it's block
                int sg = (r / 3) * 3 + c / 3;
                score = ((dup_mask_g[r] >> v) & 1) +
                        ((dup_mask_g[9 + c] >> v) & 1) +
                        ((dup_mask_g[18 + sg] >> v) & 1);
            }
            heatmap[r][c] = score;          /* own block only */
        }
    pthread_exit(NULL);
}

static int load_grid(const char *path)
{
    FILE *f = fopen(path, "r");
    int n = 0, ch; // ch takes the integer character read, and n counts the number of characters read
    if (!f) { perror(path); return -1; }
    while ((ch = fgetc(f)) != EOF) {
        int v;
        if (isspace(ch)) continue; // ignores space, tab and newlines
        if (ch == '.' || ch == '0') v = 0;
        else if (ch >= '1' && ch <= '9') v = ch - '0';
        else {
            fprintf(stderr, "Error: invalid character '%c' in %s\n", ch, path);
            fclose(f); return -1;
        }
        if (n >= N * N) {
            fprintf(stderr, "Error: more than 81 cells in %s\n", path);
            fclose(f); return -1;
        }
        grid[n / N][n % N] = v;
        n++;
    }
    fclose(f);
    if (n != N * N) {
        fprintf(stderr, "Error: expected 81 cells, found %d in %s\n", n, path);
        return -1;
    }
    return 0;
}

static void print_set(int mask)
{
    int first = 1;
    printf("{");
    for (int d = 1; d <= 9; d++)
        if (mask & (1 << d)) { printf("%s%d", first ? "" : ",", d); first = 0; }
    printf("}");
}

static void free_results(struct unit_result *res[])
{
    for (int i = 0; i < NUNITS; i++) free(res[i]);
}

int main(int argc, char **argv) // argc = argument count, argv = array containing the arguments
{
    pthread_t tid[NUNITS];
    struct unit_params up[NUNITS]; // 27 validators
    struct block_params bp[N];
    struct unit_result *res[NUNITS] = {0}; // result for 27 validators

    if (argc != 2) { fprintf(stderr, "Usage: %s <grid-file>\n", argv[0]); return 1; }
    if (load_grid(argv[1]) != 0) return 1;

    printf("Input Grid:\n");
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            if (grid[r][c]) printf("%d", grid[r][c]); else printf(".");
            printf(c < N - 1 ? " " : "\n");
        }
    }

    /* Phase 1 */
    // start the 27 validators
    for (int u = 0; u < NUNITS; u++) {
        up[u].unit_id = u;
        if (pthread_create(&tid[u], NULL, validate_worker, &up[u]) != 0) {
            perror("pthread_create"); return 1;
        }
    }
    int ok = 1;
    for (int u = 0; u < NUNITS; u++) {
        void *ret = NULL;
        pthread_join(tid[u], &ret); // parent receives the pointer given by the child thread.
        res[u] = ret;
        if (!ret) ok = 0; // if any returned pointer is NULL, allocation failure is reported
    }
    if (!ok) { fprintf(stderr, "worker allocation failure\n"); free_results(res); return 1; }

    int nvalid = 0;
    for (int u = 0; u < NUNITS; u++) nvalid += validity[u];
    if (nvalid == NUNITS) {
        printf("\nVALID\n");
        free_results(res);
        return 0;
    }
    printf("\nINVALID\n");

    /* Phase 2 (a) */
    printf("Validity Score: %.2f%%\n\n", 100.0 * nvalid / NUNITS);

    /* (b) heatmap */
    // start the 9 analytics threads
    for (int u = 0; u < NUNITS; u++) dup_mask_g[u] = res[u]->dup_mask; // first copy the duplicated masks into the shared array
    for (int b = 0; b < N; b++) {
        bp[b].block_id = b;
        // one thread per block
        if (pthread_create(&tid[b], NULL, analytics_worker, &bp[b]) != 0) {
            perror("pthread_create"); free_results(res); return 1;
        }
    }
    for (int b = 0; b < N; b++) pthread_join(tid[b], NULL);

    printf("Conflict Density Heatmap:\n");
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            printf("%d%c", heatmap[r][c], c < N - 1 ? ' ' : '\n'); // heatmap printed

    /* (c) hotspots */
    // every cell scoring 3 is a hotspot, including all blanks.
    int hot = 0;
    printf("\nHotspots (score 3):\n");
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (heatmap[r][c] == 3) {
                printf("  (%d,%d) value=%d\n", r, c, grid[r][c]);
                hot++;
            }
    printf("Total Hotspots: %d\n\n", hot);

    /* (d) missing digit report */
    printf("Missing Digit Report:\n");
    static const char *kind[3] = { "Row", "Column", "Sub-grid" };
    // kind[u/9] - unit type and u % 9 = number within that type
    for (int u = 0; u < NUNITS; u++) {
        const struct unit_result *x = res[u];
        if (x->valid) continue; // skip valid units
        printf("  %-8s %d : missing ", kind[u / 9], u % 9);
        print_set(x->missing_mask); // prints the missing-digit bitmask as a set
        printf("  blanks=%d", x->blank_count);

        // repair condition
        // builtin_popcount counts the set bits
        if (x->blank_count == 1 && __builtin_popcount((unsigned)x->missing_mask) == 1) {
            // there is exactly one blank and exactly one missing digit
            int d = __builtin_ctz((unsigned)x->missing_mask); // counts the trailing zeros. Since the mask contains excatly one set bit, this gives that bit's position - the missing digit itself. 
            for (int k = 0; k < N; k++) {
                int r, c;
                unit_cell(u, k, &r, &c);
                if (grid[r][c] == 0)
                    printf("  -> repair: put %d at (%d,%d)", d, r, c);
            }
        }
        printf("\n");
    }

    free_results(res);
    return 0;
}
