#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <unistd.h>
#define SYSLOG_NAMES
#include <syslog.h>
#include <sys/uio.h>

#include <mosquitto.h>
#include "common.h"

/* error logging */
static int logtostderr = -1;
static int maxloglevel = LOG_WARNING;
static const char *label;
static void (*loghook)(int loglevel, const char *str);

/* safeguard our LOG_EXIT extension */
#if (LOG_MQTT & (LOG_FACMASK | LOG_PRIMASK))
#error LOG_MQTT conflict
#endif

void mylogsethook(void (*hook)(int loglevel, const char *str))
{
	loghook = hook;
}

void myopenlog(const char *name, int options, int facility)
{
	logtostderr = isatty(STDERR_FILENO);
	if (!logtostderr && name) {
		openlog(name, options, facility);
		setlogmask(LOG_UPTO(maxloglevel));
	} else
		label = name;
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
	char *msg = NULL;

	if (logtostderr < 0)
		myopenlog(NULL, 0, LOG_LOCAL1);

	if (logtostderr && ((loglevel & LOG_PRIMASK) > maxloglevel))
		goto done;
	if (logtostderr) {
		struct timespec tv;
		char timbuf[64];

		clock_gettime(CLOCK_REALTIME, &tv);
		strftime(timbuf, sizeof(timbuf), "%b %d %H:%M:%S", localtime(&tv.tv_sec));
		sprintf(timbuf+strlen(timbuf), ".%03u ", (int)(tv.tv_nsec/1000000));

		va_start(va, fmt);
		vasprintf(&msg, fmt, va);
		va_end(va);

		struct iovec vec[] = {
			{ .iov_base = timbuf, .iov_len = strlen(timbuf), },
			{ .iov_base = (char *)label, .iov_len = strlen(label), },
			{ .iov_base = ": ", .iov_len = 2, },
			{ .iov_base = msg, .iov_len = strlen(msg), },
			{ .iov_base = "\n", .iov_len = 1, },
		};
		writev(STDERR_FILENO, vec, sizeof(vec)/sizeof(vec[0]));
	} else {
		va_start(va, fmt);
		vsyslog(loglevel & LOG_PRIMASK, fmt, va);
		va_end(va);
	}
done:
	if (loghook) {
		if (!msg) {
			va_start(va, fmt);
			vasprintf(&msg, fmt, va);
			va_end(va);
		}
		loghook(loglevel, msg);
	}
	if (msg)
		free(msg);
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
	const char *strpos;
	double value, part;
	unsigned long fact, fact2;

	if (!endp)
		endp = &localendp;
	if (!str)
		return NAN;

	for (value = 0, fact2 = 1, strpos = str; *strpos;) {
		part = strtod(strpos, endp);
		if (*endp <= strpos)
			goto done;
		switch (**endp) {
		case 'w':
			fact = 60*60*24*7;
			fact2 = 60*60*24;
			break;
		case 'd':
			fact = 60*60*24;
			fact2 = 60*60;
			break;
		case 'h':
			fact = 60*60;
			fact2 = 60;
			break;
		case 'm':
			fact = 60;
			fact2 = 1;
			break;
		case 's':
			fact = 1;
			fact2 = 0;
			break;
		case 0:
			value += part*fact2;
			goto done;
			break;
		default:
			goto done;
		}
		value += part*fact;
		/* consume suffix */
		*endp += 1;
		strpos = *endp;
	}
done:
	return (*endp == str) ? NAN : value;
}
const char *mydtostr(double d)
{
	static char buf[64];
	char *str;
	int ptpresent = 0;

	if (isnan(d))
		return "";
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

char *resolve_relative_path(const char *path, const char *ref)
{
	char *abspath, *str, *up;

	if (!path || !ref)
		return NULL;

	if (!strncmp(path, "./", 2)) {
		asprintf(&abspath, "%s/%s", ref, path +2);
		return abspath;

	} else if (!strcmp(path, ".")) {
		return strdup(ref);

	} else if (!strncmp(path, "..", 2)) {
		asprintf(&abspath, "%s/%s", ref, path);
		for (str = strstr(abspath, "/.."); str; str = strstr(abspath, "/..")) {
			*str = 0;
			up = strrchr(abspath, '/');
			if (!up) {
				*str = '/';
				break;
			}
			memmove(up, str+3, strlen(str+3)+1);
		}
		return abspath;
	}
	return NULL;
}

/* self-sync util */
static char myuuid[128];
static const char selfsynctopic[] = "tmp/selfsync";
void send_self_sync(struct mosquitto *mosq, int qos)
{
	int ret;

	sprintf(myuuid, "%i-%li-%i", getpid(), time(NULL), rand());

	ret = mosquitto_subscribe(mosq, NULL, selfsynctopic, qos);
	if (ret)
		mylog(LOG_ERR, "mosquitto_subscribe %s: %s", selfsynctopic, mosquitto_strerror(ret));
	ret = mosquitto_publish(mosq, NULL, selfsynctopic, strlen(myuuid), myuuid, qos, 0);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", selfsynctopic, mosquitto_strerror(ret));
}

int is_self_sync(const struct mosquitto_message *msg)
{
	return !strcmp(msg->topic, selfsynctopic) &&
		!strcmp(myuuid, msg->payload ?: "");
}

/* time function */
double walltime(void)
{
	struct timeval t;
	if (0 != gettimeofday(&t, 0))
		exit(1);
	return t.tv_sec + ((t.tv_usec % 1000000) / 1e6);
}
