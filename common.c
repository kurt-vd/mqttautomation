#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <syslog.h>

#include "common.h"

/* error logging */
static int logtostderr = -1;
static int maxloglevel = LOG_WARNING;

void myopenlog(const char *name, int options, int facility)
{
	char *tty;

	tty = ttyname(STDERR_FILENO);
	logtostderr = tty && !!strcmp(tty, "/dev/console");
	if (!logtostderr && name) {
		openlog(name, options, facility);
		setlogmask(LOG_UPTO(maxloglevel));
	}
}

void myloglevel(int level)
{
	maxloglevel = level;
	if (logtostderr == 0)
		setlogmask(LOG_UPTO(maxloglevel));
}

void mylog(int loglevel, const char *fmt, ...)
{
	va_list va;

	if (logtostderr < 0)
		myopenlog(NULL, 0, LOG_LOCAL1);

	if (logtostderr && loglevel > maxloglevel)
		goto done;
	va_start(va, fmt);
	if (logtostderr) {
		vfprintf(stderr, fmt, va);
		fputc('\n', stderr);
		fflush(stderr);
	} else
		vsyslog(loglevel, fmt, va);
	va_end(va);
done:
	if (loglevel <= LOG_ERR)
		exit(1);
}
