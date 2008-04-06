/*
 * askpass.c - prompts a user for a passphrase using any suitable method
 *             and prints the result to stdout.
 *
 * Copyright (C) 2008   David Härdeman <david@hardeman.nu>
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this package; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */


#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <termios.h>
#include <sys/klog.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <signal.h>

#define DEBUG 0

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

static bool disable_method(const char *method);

/*****************************************************************************
 * Utility functions                                                         *
 *****************************************************************************/
static void
debug(const char *fmt, ...)
{
	va_list ap;
	static bool first = true;
	static FILE *dbgfile;

	if (!DEBUG)
		return;

	if (first) {
		first = false;
		dbgfile = fopen("/tmp/askpass.debug", "a");
	}

	if (!dbgfile)
		return;

	va_start(ap, fmt);
	vfprintf(dbgfile, fmt, ap);
	va_end(ap);
}

static void
usage(const char *arg0, const char *errmsg)
{
	if (errmsg)
		fprintf(stderr, "Error: %s\nUsage: %s PROMPT\n", errmsg, arg0);
	else
		fprintf(stderr, "Usage: %s PROMPT\n", arg0);
	exit(EXIT_FAILURE);
}

static void
fifo_common_finish(int fd, char **buf, size_t *used, size_t *size)
{
	if (fd >= 0)
		close(fd);

	if (!*buf)
		return;

	memset(*buf, '\0', *size);
	free(*buf);
	*buf = NULL;
	*used = 0;
	*size = 0;
}

static bool
fifo_common_read(int fd, char **buf, size_t *used, size_t *size)
{
	ssize_t result;

again:
	if ((*size - *used) == 0) {
		*size += 4096;
		*buf = realloc(*buf, *size);
		if (!*buf) {
			*size = 0;
			*used = 0;
			debug("Failed to allocate memory for passphrase\n");
			return false;
		}
	}

reread:
	result = read(fd, *buf + *used, *size - *used);

	if (result < 0) {
		if (errno == EAGAIN)
			return false;
		if (errno == EINTR)
			goto reread;
		debug("Error when reading from fifo\n");
		return false;
	}

	debug("Read %i bytes from fifo\n", (int)result);
	*used += result;

	if (result == 0)
		return true;

	goto again;
}


/*****************************************************************************
 * usplash functions                                                         *
 *****************************************************************************/
#define USPLASH_WRITE_FIFO "/dev/.initramfs/usplash_fifo"
#define USPLASH_READ_FIFO "/dev/.initramfs/usplash_outfifo"
#define USPLASH_CONSOLE "/dev/tty8"
static size_t usplashused = 0;
static size_t usplashsize = 0;
static char *usplashbuf = NULL;
static bool usplashwaiting = false;

static void
usplash_finish(int fd)
{
	int console;
	char buf[100];
	ssize_t ret;
	int tries = 0;
	bool somedata = false;

	if (usplashwaiting) {
		/* This is ugly, but we need to unwedge usplash if a different
		 * method has been used to provide the passphrase and usplash
		 * is still waiting for user input
		 */
		debug("Usplash cleanup fd %i\n", fd);
		console = open(USPLASH_CONSOLE, O_RDWR);
		if (console >= 0) {
			/* Send newline to usplash */
			ioctl(console, TIOCSTI, "\n");
			close(console);

			/* Flush (partial) passphrase from pipe */
			while (tries < 10) {
				ret = read(fd, buf, sizeof(buf));

				if (ret > 0) {
					somedata = true;
					continue;
				}

				if (ret < 0 && errno == EINTR)
					continue;

				if ((ret <  0 && errno == EAGAIN) ||
				    (ret == 0 && somedata == false)) {
					sleep(1);
					tries++;
					continue;
				}

				break;
			}
			memset(buf, '\0', sizeof(buf));
		}
		usplashwaiting = false;
	}

	fifo_common_finish(fd, &usplashbuf, &usplashused, &usplashsize);
}

static bool
usplash_read(int fd, char **buf, size_t *size)
{
	debug("In usplash_read\n");
	if (fifo_common_read(fd, &usplashbuf, &usplashused, &usplashsize)) {
		while (usplashused > 0 && 
		       (usplashbuf[usplashused - 1] == '\n') ||
		       (usplashbuf[usplashused - 1] == '\0')) {
			usplashused--;
			usplashbuf[usplashused] = '\0';
			debug("Correcting usplash read length\n");
		}
		*buf = usplashbuf;
		*size = usplashused;
		usplashwaiting = false;
		return true;
	}

	return false;
}

static int
usplash_prepare(const char *prompt)
{
	int wrfd = -1;
	int rdfd = -1;
	ssize_t len;
	char command[strlen(prompt) + strlen("INPUTQUIET") + 2];

	sprintf(command, "INPUTQUIET %s", prompt);

	wrfd = open(USPLASH_WRITE_FIFO, O_WRONLY | O_NONBLOCK);
	if (wrfd < 0)
		goto out;

	len = write(wrfd, command, strlen(command) + 1);
	if (len < 0)
		goto out;

	rdfd = open(USPLASH_READ_FIFO, O_RDONLY | O_NONBLOCK);
	/* If usplash is enabled, disable console */
	if (rdfd >= 0) {
		disable_method("console");
		usplashwaiting = true;
	}

out:
	if (wrfd >= 0)
		close(wrfd);
	return rdfd;
}

/*****************************************************************************
 * fifo functions                                                            *
 *****************************************************************************/
#define FIFO_PATH "/lib/cryptsetup/passfifo"
static size_t fifoused = 0;
static size_t fifosize = 0;
static char *fifobuf = NULL;

static void
fifo_finish(int fd)
{
	fifo_common_finish(fd, &fifobuf, &fifoused, &fifosize);
}

static bool
fifo_read(int fd, char **buf, size_t *size)
{
	debug("In fifo_read\n");
	if (fifo_common_read(fd, &fifobuf, &fifoused, &fifosize)) {
		*buf = fifobuf;
		*size = fifoused;
		return true;
	}

	return false;
}

static int
fifo_prepare(const char *prompt)
{
	int ret;

	ret = mkfifo(FIFO_PATH, 0600);
	if (ret && errno != EEXIST)
		return -1;

	return open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
}

/*****************************************************************************
 * console functions                                                         *
 *****************************************************************************/
#define CONSOLE_PATH "/dev/console"
static struct termios term_old;
static bool term_set = false;
static char *consolebuf = NULL;
static size_t consolebuflen = 0;

static void
console_finish(int fd)
{
	if (consolebuf) {
		memset(consolebuf, '\0', consolebuflen);
		free(consolebuf);
		consolebuf = NULL;
		consolebuflen = 0;
	}

	if (!term_set || fd < 0)
		return;

	term_set = false;
	tcsetattr(fd, TCSAFLUSH, &term_old);
	fprintf(stderr, "\n");
	klogctl(7, NULL, 0);
}

bool
console_read(int fd, char **buf, size_t *size)
{
	ssize_t nread;

	/* Console is in ICANON mode so we'll get entire lines */
	nread = getline(&consolebuf, &consolebuflen, stdin);

	if (nread < 0)
		return NULL;

	/* Strip trailing newline, if any */
	if (nread > 0 && consolebuf[nread - 1] == '\n') {
		nread--;
		consolebuf[nread] = '\0';
	}

	*size = nread;
	*buf = consolebuf;

	return true;
}

static int
console_prepare(const char *prompt)
{
	struct termios term_new;

	if (!isatty(STDIN_FILENO)) {
		if (access(CONSOLE_PATH, R_OK | W_OK)) {
			debug("No access to console device " CONSOLE_PATH "\n");
			return -1;
		}

		if (!freopen(CONSOLE_PATH, "r", stdin)  ||
		    !freopen(CONSOLE_PATH, "a", stdout) ||
		    !freopen(CONSOLE_PATH, "a", stderr) ||
		    !isatty(STDIN_FILENO)) {
			debug("Failed to open console\n");
			return -1;
		}
	}

	if (tcgetattr(STDIN_FILENO, &term_old)) {
		debug("Failed to get terminal settings\n");
		return -1;
	}

	term_new = term_old;
	term_new.c_lflag &= ~ECHO;
	term_new.c_lflag |= ICANON;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_new)) {
		debug("Failed to disable echoing\n");
		return -1;
	}

	if (fprintf(stderr, prompt) < 0) {
		debug("Failed to print prompt\n");
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_old);
		return -1;
	}

	/* Disable printk to console */
	klogctl(6, NULL, 0);
	term_set = true;
	return STDIN_FILENO;
}

/*****************************************************************************
 * main functions                                                            *
 *****************************************************************************/

struct method {
	const char *name;
	int (*prepare)(const char *prompt);
	bool (*read)(int fd, char **buf, size_t *size);
	void (*finish)(int fd);
	bool enabled;
	int fd;
};

static struct method methods[] = {
	{ "usplash", usplash_prepare, usplash_read, usplash_finish, true, -1 },
	{ "fifo", fifo_prepare, fifo_read, fifo_finish, true, -1 },
	{ "console", console_prepare, console_read, console_finish, true, -1 }
};

static bool
disable_method(const char *method)
{
	int i;
	bool result = false;

	for (i = 0; i < ARRAY_SIZE(methods); i++) {
		if (method && strcmp(methods[i].name, method))
			continue;
		if (!methods[i].enabled)
			continue;

		methods[i].finish(methods[i].fd);
		methods[i].fd = -1;
		methods[i].enabled = false;
		result = true;
	}

	return result;
}

int
main(int argc, char **argv, char **envp)
{
	char *pass = NULL;
	size_t passlen = 0;
	int i;
	int nfds;
	fd_set fds;
	int ret;
	bool done = false;
	sigset_t sigset;

	if (argc != 2)
		usage(argv[0], "incorrect number of arguments");

	sigfillset(&sigset);
	sigprocmask(SIG_BLOCK, &sigset, NULL);

	for (i = 0; i < ARRAY_SIZE(methods); i++) {
		if (!methods[i].enabled)
			continue;
		debug("Enabling method %s\n", methods[i].name);
		methods[i].fd = methods[i].prepare(argv[1]);
		if (methods[i].fd < 0)
			methods[i].enabled = false;
	}

	while (!done) {
		nfds = 0;
		FD_ZERO(&fds);
		for (i = 0; i < ARRAY_SIZE(methods); i++) {
			if (!methods[i].enabled || methods[i].fd < 0)
				continue;
			debug("method %i has fd %i and name %s\n", i, methods[i].fd, methods[i].name);
			FD_SET(methods[i].fd, &fds);
			if (methods[i].fd + 1 > nfds)
				nfds = methods[i].fd + 1;
		}

		if (nfds == 0) {
			debug("All methods disabled\n");
			exit(EXIT_FAILURE);
		}

		debug("Starting select with nfds %i\n", nfds);
		ret = select(nfds, &fds, NULL, NULL, NULL);

		if (ret <= 0) {
			if (ret == 0 || errno == EINTR)
				continue;
			debug("Select failed\n");
			disable_method(NULL);
			exit(EXIT_FAILURE);
		}

		for (i = 0; i < ARRAY_SIZE(methods); i++) {
			if (!methods[i].enabled || methods[i].fd < 0)
				continue;
			if (!FD_ISSET(methods[i].fd, &fds))
				continue;
			if (methods[i].read(methods[i].fd, &pass, &passlen) && pass) {
				done = true;
				break;
			}
		}
	}

	debug("Writing %i bytes to stdout\n", (int)passlen);
	write(STDOUT_FILENO, pass, passlen);
	disable_method(NULL);
	exit(EXIT_SUCCESS);
}

