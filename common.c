#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#define SYSLOG_NAMES
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

int mysetloglevelstr(char *str)
{
	int j;

	for (j = 0; prioritynames[j].c_name; ++j) {
		if (!strcmp(str ?: "", prioritynames[j].c_name)) {
			myloglevel(prioritynames[j].c_val);
			return prioritynames[j].c_val;
		}
	}
	return -1;
}

double mystrtod(const char *str, char **endp)
{
	char *localendp;
	double value;

	if (!endp)
		endp = &localendp;
	value = strtod(str ?: "nan", endp);
	if (**endp && strchr(":h'", **endp))
		value += strtod((*endp)+1, endp)/60;
	if (**endp && strchr(":m\"", **endp))
		value += strtod((*endp)+1, endp)/3600;
	return (*endp == str) ? NAN : value;
}
const char *mydtostr(double d)
{
	static char buf[64];
	char *str;
	int ptpresent = 0;

	sprintf(buf, "%lg", d);
	for (str = buf; *str; ++str) {
		if (*str == '.')
			ptpresent = 1;
		else if (*str == 'e')
			/* nothing to do anymore */
			break;
		else if (ptpresent && *str == '0') {
			int len = strspn(str, "0");
			if (!str[len]) {
				/* entire string (after .) is '0' */
				*str = 0;
				if (str > buf && *(str-1) == '.')
					/* remote '.' too */
					*(str-1) = 0;
				break;
			}
		}
	}
	return buf;
}
