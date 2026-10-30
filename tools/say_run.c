/*
 * say_run: run a program and put what it says in the kernel's log.
 *
 * The standard output and error of a launchd job here end at /dev/console, which is kept
 * nowhere: a job that prints a report leaves no trace. This runs another program with both on
 * a pipe and sends every line to kern.hakios.say (bsd/dev/hakios.mali.c), which prints it into
 * the kernel log -- the one the loader keeps in DRAM and pstore keeps after the reset.
 *
 *   say_run PROGRAM [ARG...]
 *
 * The exit status is the program's own (127 when it cannot be started at all).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define SAY_LINE        480             /* the kernel's line buffer is 512 */

static void
say(const char *text, size_t length)
{
	while (length > 0) {
		char line[SAY_LINE + 1];
		size_t part = length > SAY_LINE ? SAY_LINE : length;

		memcpy(line, text, part);
		line[part] = 0;
		(void)sysctlbyname("kern.hakios.say", NULL, NULL, line, part);
		text += part;
		length -= part;
	}
}

static void
say_text(const char *text)
{
	say(text, strlen(text));
}

int
main(int argc, char **argv)
{
	char line[SAY_LINE + 1], buf[4096];
	size_t length = 0;
	ssize_t n;
	int fds[2], status = 0;
	pid_t child;

	if (argc < 2) {
		say_text("say_run: usage: say_run PROGRAM [ARG...]");
		return 2;
	}
	if (pipe(fds) != 0) {
		say_text("say_run: pipe failed");
		return 1;
	}
	child = fork();
	if (child < 0) {
		say_text("say_run: fork failed");
		return 1;
	}
	if (child == 0) {
		char message[512];

		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
		close(fds[0]);
		close(fds[1]);
		execv(argv[1], &argv[1]);
		snprintf(message, sizeof(message), "say_run: cannot start %s", argv[1]);
		say_text(message);
		_exit(127);
	}
	close(fds[1]);
	while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\n' || buf[i] == '\r') {
				if (length != 0) {
					line[length] = 0;
					say(line, length);
					length = 0;
				}
			} else {
				line[length++] = buf[i];
				if (length == SAY_LINE) {
					line[length] = 0;
					say(line, length);
					length = 0;
				}
			}
		}
	}
	if (length != 0) {
		line[length] = 0;
		say(line, length);
	}
	close(fds[0]);
	(void)waitpid(child, &status, 0);
	{
		char message[256];

		if (WIFEXITED(status)) {
			snprintf(message, sizeof(message), "say_run: %s exited with status %d", argv[1],
			    WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			snprintf(message, sizeof(message), "say_run: %s died of signal %d", argv[1],
			    WTERMSIG(status));
		} else {
			snprintf(message, sizeof(message), "say_run: %s ended (status %#x)", argv[1], status);
		}
		say_text(message);
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
