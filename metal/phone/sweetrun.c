/*
 * sweetrun: runs the test commands in /var/local/run.txt once per boot (launchd job
 * org.sweet.run, RunAtLoad).
 *
 * run.txt: one command per line, words split on spaces, # comments. Builtins:
 *   sleep SECS         wait (e.g. for SpringBoard)
 *   env NAME=VALUE     set for the following commands
 * Each command's start, exit status and run time are logged as "SWEETRUN: ...". Everything (these
 * lines and the commands' stdout/stderr) also goes to /var/local/run.log, rewritten each boot:
 * console output from launchd jobs is not reliable (0095 boot: it stopped after two lines).
 * Files come from the boot image (tools/mkpayload.py --files, bsd/dev/sweet_bootfiles.c).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static FILE *logf;

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (logf) {
		va_start(ap, fmt);
		vfprintf(logf, fmt, ap);
		va_end(ap);
		fflush(logf);
		fsync(fileno(logf));
	}
}

int main(int argc, char **argv)
{
	const char *list = argc > 1 ? argv[1] : "/var/local/run.txt";
	setvbuf(stdout, NULL, _IOLBF, 0);
	logf = fopen("/var/local/run.log", "w");
	FILE *f = fopen(list, "r");
	if (!f) {
		say("SWEETRUN: no %s (%s)\n", list, strerror(errno));
		return 0;
	}
	say("SWEETRUN: start (%s)\n", list);
	char line[1024];
	int n = 0;
	while (fgets(line, sizeof(line), f)) {
		char *hash = strchr(line, '#');
		if (hash)
			*hash = 0;
		char *av[64];
		int ac = 0;
		for (char *t = strtok(line, " \t\r\n"); t && ac < 63; t = strtok(NULL, " \t\r\n"))
			av[ac++] = t;
		av[ac] = NULL;
		if (!ac)
			continue;
		if (!strcmp(av[0], "sleep") && ac == 2) {
			say("SWEETRUN: sleep %s\n", av[1]);
			sleep((unsigned)atoi(av[1]));
			continue;
		}
		if (!strcmp(av[0], "env") && ac == 2 && strchr(av[1], '=')) {
			putenv(strdup(av[1]));
			say("SWEETRUN: env %s\n", av[1]);
			continue;
		}
		n++;
		say("SWEETRUN: [%d] %s ...\n", n, av[0]);
		double t0 = now_s();
		pid_t pid;
		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init(&fa);
		if (logf) {
			posix_spawn_file_actions_adddup2(&fa, fileno(logf), 1);
			posix_spawn_file_actions_adddup2(&fa, fileno(logf), 2);
		}
		int err = posix_spawn(&pid, av[0], &fa, NULL, av, environ);
		posix_spawn_file_actions_destroy(&fa);
		if (err) {
			say("SWEETRUN: [%d] spawn failed: %s\n", n, strerror(err));
			continue;
		}
		int st = 0;
		while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
			;
		if (WIFSIGNALED(st))
			say("SWEETRUN: [%d] killed by signal %d after %.2f s\n", n, WTERMSIG(st), now_s() - t0);
		else
			say("SWEETRUN: [%d] exit %d after %.2f s\n", n, WEXITSTATUS(st), now_s() - t0);
	}
	fclose(f);
	sync();
	say("SWEETRUN: done (%d commands)\n", n);
	return 0;
}
