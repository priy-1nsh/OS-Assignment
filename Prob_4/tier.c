/*
 * tier - userspace front-end for fork_guard
 *
 *   tier set <user> <gold|silver|bronze>   (root only)
 *   tier reset                             (root only)
 *   tier show                              tier assignments (/proc/sched_tier)
 *   tier show all                          assignments + fork stats/tunables
 *   tier stats                             fork stats/tunables (/proc/fork_guard)
 *
 * Build: gcc -Wall -Wextra -O2 -o tier tier.c
 */
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CTL	"/proc/fork_guard_ctl"
#define TIERS	"/proc/sched_tier"
#define STATS	"/proc/fork_guard"

static int write_ctl(const char *cmd)
{
	int fd = open(CTL, O_WRONLY);
	ssize_t n;

	if (fd < 0) {
		if (errno == EACCES || errno == EPERM)
			fprintf(stderr, "tier: must be root\n");
		else
			fprintf(stderr, "tier: cannot open %s: %s (is the fork_guard kernel running?)\n",
				CTL, strerror(errno));
		return -1;
	}
	n = write(fd, cmd, strlen(cmd));
	if (n < 0) {
		fprintf(stderr, "tier: write failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int show_tiers(void)
{
	char line[256];
	int rows = 0, first = 1;
	FILE *f = fopen(TIERS, "r");

	if (!f) {
		fprintf(stderr, "tier: cannot open %s: %s\n", TIERS, strerror(errno));
		return 1;
	}
	while (fgets(line, sizeof(line), f)) {
		if (first) {
			first = 0;
			continue;	/* header; print only if rows follow */
		}
		if (rows == 0)
			printf("UID    TIER    NICE\n");
		fputs(line, stdout);
		rows++;
	}
	fclose(f);
	if (!rows)
		printf("(no user is tiered; everyone is SILVER)\n");
	return 0;
}

static int show_stats(void)
{
	char line[512];
	FILE *f = fopen(STATS, "r");

	if (!f) {
		fprintf(stderr, "tier: cannot open %s: %s\n", STATS, strerror(errno));
		return 1;
	}
	while (fgets(line, sizeof(line), f))
		fputs(line, stdout);
	fclose(f);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: tier set <user> <gold|silver|bronze>\n"
		"       tier reset\n"
		"       tier show [all]\n"
		"       tier stats\n");
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "set")) {
		struct passwd *pw;
		char cmd[64];

		if (argc != 4) {
			usage();
			return 2;
		}
		if (strcmp(argv[3], "gold") && strcmp(argv[3], "silver") &&
		    strcmp(argv[3], "bronze")) {
			fprintf(stderr, "tier: tier must be gold, silver or bronze\n");
			return 2;
		}
		pw = getpwnam(argv[2]);
		if (!pw) {
			fprintf(stderr, "tier: no such user '%s'\n", argv[2]);
			return 1;
		}
		snprintf(cmd, sizeof(cmd), "set %u %s\n", (unsigned int)pw->pw_uid, argv[3]);
		if (write_ctl(cmd))
			return 1;
		printf("user %s (uid %u) -> %s\n", argv[2], (unsigned int)pw->pw_uid, argv[3]);
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "reset")) {
		if (write_ctl("reset\n"))
			return 1;
		printf("all users reset to SILVER\n");
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "show")) {
		int rc = show_tiers();

		if (argc == 3 && !strcmp(argv[2], "all")) {
			printf("\n");
			rc |= show_stats();
		}
		return rc;
	}
	if (argc == 2 && !strcmp(argv[1], "stats"))
		return show_stats();

	usage();
	return 2;
}
