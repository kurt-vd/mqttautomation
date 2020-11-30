#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <unistd.h>
#include <getopt.h>
#include <syslog.h>
#include <sys/stat.h>
#include <mosquitto.h>

#include "lib/libt.h"
#include "common.h"

#define NAME "mqttled"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* program options */
static const char help_msg[] =
	NAME ": an MQTT to LED bridge\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=STR	Give MQTT topic suffix for timeouts (default '/ledhw')\n"
	" -w, --write=STR	Give MQTT topic suffix for writing the topic (default /set)\n"
	"\n"
	"Paramteres\n"
	" PATTERN	A pattern to subscribe for\n"
	;

#ifdef _GNU_SOURCE
static struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "mqtt", required_argument, NULL, 'm', },
	{ "suffix", required_argument, NULL, 's', },
	{ "write", required_argument, NULL, 'w', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:s:w:";

/* logging */
static int loglevel = LOG_WARNING;

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_suffix = "/ledhw";
static const char *mqtt_write_suffix = "/set";
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;
	struct item *prev;

	char *topic;
	int topiclen;
	char *writetopic;
	char *redirtopic;
	char *redirwrtopic;
	char *name;
	char *sysfsdir;
	int value;
	int maxvalue;
	int initialized;

	int flags;
		#define FL_SHELLY (1 << 0)
};

__attribute__((unused))
static int is_physical(const struct item *it)
{
	return !!it->sysfsdir;
}
__attribute__((unused))
static int is_virtual(const struct item *it)
{
	return !it->sysfsdir && !it->redirtopic;
}
__attribute__((unused))
static int is_redir(const struct item *it)
{
	return !it->sysfsdir && it->redirtopic;
}

struct item *items;

/* sysfs attrs */
static const char *vattr_reads(const char *fmt, va_list va)
{
	FILE *fp;
	char *file;
	static char *line;
	static size_t linesize;
	int ret;

	vasprintf(&file, fmt, va);

	fp = fopen(file, "r");
	if (!fp) {
		free(file);
		return NULL;
	}
	ret = getline(&line, &linesize, fp);
	fclose(fp);
	free(file);
	return (ret < 0) ? NULL : line;
}

const char *attr_reads(const char *fmt, ...)
{
	va_list va;
	const char *result;

	va_start(va, fmt);
	result = vattr_reads(fmt, va);
	va_end(va);
	return result;
}

/* */
int attr_read(int default_value, const char *fmt, ...)
{
	va_list va;
	const char *result;

	va_start(va, fmt);
	result = vattr_reads(fmt, va);
	va_end(va);

	if (!result) {
		mylog(LOG_WARNING, "fopen %s r: %s", fmt, ESTR(errno));
		return default_value;
	}
	return strtol(result, NULL, 0);
}

int attr_write(const char *value, const char *fmt, ...)
{
	FILE *fp;
	int ret;
	char *file;
	va_list va;

	va_start(va, fmt);
	vasprintf(&file, fmt, va);
	va_end(va);

	fp = fopen(file, "w");
	if (fp) {
		ret = fprintf(fp, "%s\n", value);
		fclose(fp);
	} else {
		mylog(LOG_WARNING, "fopen %s w: %s", file, ESTR(errno));
		ret = -1;
	}
	free(file);
	return ret;
}

/* MQTT iface */
static void my_mqtt_log(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
	static const int logpri_map[] = {
		MOSQ_LOG_ERR, LOG_ERR,
		MOSQ_LOG_WARNING, LOG_WARNING,
		MOSQ_LOG_NOTICE, LOG_NOTICE,
		MOSQ_LOG_INFO, LOG_INFO,
		//MOSQ_LOG_DEBUG, LOG_DEBUG,
		0,
	};
	int j;

	for (j = 0; logpri_map[j]; j += 2) {
		if (level & logpri_map[j]) {
			mylog(logpri_map[j+1], "[mosquitto] %s", str);
			return;
		}
	}
}

static int test_suffix(const char *topic, const char *suffix)
{
	int len;

	len = strlen(topic ?: "") - strlen(suffix ?: "");
	if (len < 0)
		return 0;
	/* match suffix */
	return !strcmp(topic+len, suffix ?: "");
}

static int test_nodename(const char *nodename)
{
	/* test node name */
	static char mynodename[128];

	if (!nodename)
		/* empty nodename matches always, for local hosts */
		return !strcmp(mqtt_host, "localhost") ||
			!strncmp(mqtt_host, "127.", 4) ||
			!strcmp(mqtt_host, "::1");

	gethostname(mynodename, sizeof(mynodename));
	return !strcmp(mynodename, nodename);
}

static void led_initial_value(void *dat);
static struct item *get_item(const char *topic, const char *suffix, int create)
{
	struct item *it;
	int len, ret;

	len = strlen(topic ?: "") - strlen(suffix ?: "");
	if (len < 0)
		return NULL;
	/* match suffix */
	if (strcmp(topic+len, suffix ?: ""))
		return NULL;
	/* match base topic */
	for (it = items; it; it = it->next) {
		if ((it->topiclen == len) && !strncmp(it->topic ?: "", topic, len))
			return it;
	}
	if (!create)
		return NULL;
	/* not found, create one */
	it = malloc(sizeof(*it));
	memset(it, 0, sizeof(*it));
	it->topic = strndup(topic, len);
	it->topiclen = len;
	if (mqtt_write_suffix)
		asprintf(&it->writetopic, "%s%s", it->topic, mqtt_write_suffix);

	/* subscribe */
	ret = mosquitto_subscribe(mosq, NULL, it->writetopic ?: it->topic, mqtt_qos);
	if (ret)
		mylog(LOG_ERR, "mosquitto_subscribe '%s': %s", it->writetopic ?: it->topic, mosquitto_strerror(ret));

	/* insert in linked list */
	it->next = items;
	if (it->next) {
		it->prev = it->next->prev;
		it->next->prev = it;
	} else
		it->prev = (struct item *)(((char *)&items) - offsetof(struct item, next));
	it->prev->next = it;
	return it;
}

static void drop_item(struct item *it)
{
	int ret;

	libt_remove_timeout(led_initial_value, it);
	/* remove from list */
	if (it->prev)
		it->prev->next = it->next;
	if (it->next)
		it->next->prev = it->prev;

	if (it->redirtopic) {
		ret = mosquitto_unsubscribe(mosq, NULL, it->redirtopic);
		if (ret)
			mylog(LOG_ERR, "mosquitto_unsubscribe '%s': %s", it->redirtopic, mosquitto_strerror(ret));
		free(it->redirtopic);
	}
	if (it->redirwrtopic)
		free(it->redirwrtopic);
	ret = mosquitto_unsubscribe(mosq, NULL, it->writetopic ?: it->topic);
	if (ret)
		mylog(LOG_ERR, "mosquitto_unsubscribe '%s': %s", it->writetopic ?: it->topic, mosquitto_strerror(ret));
	/* free memory */
	free(it->topic);
	if (it->name)
		free(it->name);
	if (it->writetopic)
		free(it->writetopic);
	if (it->sysfsdir)
		free(it->sysfsdir);
	free(it);
}

static void init_led(struct item *it)
{
	/* find full path for led or brightness */
	static const char *const sysfsdir_fmts[] = {
		"/sys/class/leds/%s",
		"/sys/class/backlight/%s",
		"/tmp/%s",
		NULL,
	};
	char *path;
	struct stat st;
	int j, ret;

	if (it->sysfsdir)
		free(it->sysfsdir);
	it->sysfsdir = NULL;
	if (it->redirtopic) {
		ret = mosquitto_unsubscribe(mosq, NULL, it->redirtopic);
		if (ret)
			mylog(LOG_ERR, "mosquitto_unsubscribe '%s': %s", it->redirtopic, mosquitto_strerror(ret));
		free(it->redirtopic);
	}
	it->redirtopic = NULL;
	if (it->redirwrtopic)
		free(it->redirwrtopic);
	it->redirwrtopic = NULL;
	it->flags &= ~FL_SHELLY;
	it->maxvalue = 1;

	if (!strcmp(it->name, "...")) {
		return;
	} else if (!strncmp("shelly:", it->name, 7)) {
		it->redirtopic = strdup(it->name+7);
		asprintf(&it->redirwrtopic, "%s/command", it->redirtopic);
		it->flags |= FL_SHELLY;
		goto redirected;

	} else if (!strncmp("redir:", it->name, 6)) {
		it->redirtopic = strdup(it->name+6);
		asprintf(&it->redirwrtopic, "%s%s", it->redirtopic, mqtt_write_suffix ?: "");
redirected:
		/* subscribe */
		ret = mosquitto_subscribe(mosq, NULL, it->redirtopic, mqtt_qos);
		if (ret)
			mylog(LOG_ERR, "mosquitto_subscribe '%s': %s", it->redirtopic, mosquitto_strerror(ret));
		mylog(LOG_INFO, "%s: redirect to %s", it->topic, it->redirtopic);
		return;
	}

	for (j = 0; sysfsdir_fmts[j]; ++j) {
		asprintf(&path, sysfsdir_fmts[j], it->name);
		if (!stat(path, &st)) {
			it->sysfsdir = path;
			break;
		}
		free(path);
	}
	if (!it->sysfsdir)
		return;
	mylog(LOG_INFO, "%s: active on %s", it->topic, it->sysfsdir);
	it->maxvalue = attr_read(255, "%s/max_brightness", it->sysfsdir);
}

static void setled(struct item *it, const char *newvalue, int republish, int forcelocal)
{
	int ret, newval;
	char buf[16], *endp;

	if (!it->initialized)
		libt_remove_timeout(led_initial_value, it);

	if (!strcmp(newvalue, "toggle"))
		newval = !it->value;

	else if (forcelocal && (it->flags & FL_SHELLY))
		newval = !strcmp(newvalue, "on");

	else
		newval = strtod(newvalue ?: "", &endp)*it->maxvalue;

	if (is_virtual(it)) {
		/* do nothing, without backend */
	} else if (is_redir(it)) {
		/* a redirecting led */
		if (!forcelocal) {
			if (it->flags & FL_SHELLY)
				newvalue = newval ? "on" : "off";

			/* redirect to remote led */
			mylog(LOG_DEBUG, "%s > %s", it->redirwrtopic, newvalue ?: "''");
			ret = mosquitto_publish(mosq, NULL, it->redirwrtopic, strlen(newvalue ?: ""), newvalue, mqtt_qos, 0);
			if (ret < 0)
				mylog(LOG_ERR, "mosquitto_publish %s: %s", it->redirwrtopic, mosquitto_strerror(ret));
			return;
		} else {
			if (it->flags & FL_SHELLY)
				newvalue = newval ? "1" : "0";
		}

	} else if (!it->sysfsdir) {
		/* don't ack the led */
		return;
	} else if (endp > newvalue) {
		if (!strstr(it->sysfsdir, "/backlight/")) {
			if (attr_write("none", "%s/trigger", it->sysfsdir) < 0)
				return;
		}
		sprintf(buf, "%i", newval);
		if (attr_write(buf, "%s/brightness", it->sysfsdir) < 0)
			return;
	} else {
		char *dupstr = strdup(newvalue);
		char *tok = strtok(dupstr, " \t");

		if (attr_write(tok, "%s/trigger", it->sysfsdir) < 0) {
			free(dupstr);
			return;
		}
		if (!strcmp(tok, "timer")) {
			tok = strtok(NULL, " \t");
			if (tok) {
				newval = strtod(tok, NULL)*1000;
				sprintf(buf, "%i", newval);
				attr_write(buf, "%s/delay_on", it->sysfsdir);
			}
			endp = tok;
			tok = strtok(NULL, " \t");
			if (tok || endp) {
				newval = strtod(tok ?: endp, NULL)*1000;
				sprintf(buf, "%i", newval);
				attr_write(buf, "%s/delay_off", it->sysfsdir);
			}
		}
		free(dupstr);
	}

	it->value = newval;
	if (!it->initialized || (republish && mqtt_write_suffix)) {
		newvalue = mydtostr(newval * 1.0 / it->maxvalue);
		it->initialized = 1;
		/* publish, retained when writing the topic, volatile (not retained) when writing to another topic */
		mylog(LOG_DEBUG, "%s > %s", it->topic, newvalue ?: "''");
		ret = mosquitto_publish(mosq, NULL, it->topic, strlen(newvalue ?: ""), newvalue, mqtt_qos, 1);
		if (ret < 0)
			mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
	}
}

static void led_initial_value(void *dat)
{
	struct item *it = dat;
	int ret;

	/* emit empty value on MQTT
	 * this does not hurt the real output, but may trigger
	 * other logic to re-send
	 */
	mylog(LOG_DEBUG, "%s > %s", it->topic, "''");
	ret = mosquitto_publish(mosq, NULL, it->topic, 0, NULL, mqtt_qos, 1);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int forme;
	char *ledname;
	struct item *it;

	if (!strcmp(msg->topic, "tools/loglevel")) {
		mysetloglevelstr(msg->payload);
	} else if (test_suffix(msg->topic, mqtt_suffix)) {
		/* grab boardname */
		ledname = strtok(msg->payload ?: "", " \t");
		forme = test_nodename(strtok(NULL, " \t"));
		it = get_item(msg->topic, mqtt_suffix, !!msg->payloadlen && forme);
		if (!it)
			return;

		/* this is a spec msg */
		if (!msg->payloadlen || !forme) {
			mylog(LOG_INFO, "removed led spec for %s", it->topic);
			drop_item(it);
			return;
		}
		/* free old led spec */
		if (it->sysfsdir)
			free(it->sysfsdir);
		it->sysfsdir = NULL;

		free(it->name);
		it->name = strdup(ledname);

		/* finalize */
		mylog(LOG_INFO, "new led spec for %s: '%s'", it->topic, ledname);
		init_led(it);
		if (!it->initialized && !is_redir(it))
			libt_add_timeout(0.5, led_initial_value, it);

	} else if ((it = get_item(msg->topic, mqtt_write_suffix, 0)) != NULL) {
		/* this is the write topic */
		if (!msg->retain)
			setled(it, msg->payload, 1, 0);

	} else {
		if ((!mqtt_write_suffix || msg->retain) &&
			(it = get_item(msg->topic, NULL, 0)) != NULL) {
			if (is_physical(it))
				/* this is the main led topic */
				setled(it, msg->payload, 0, 0);
		}

		/* test for redirected leds */
		for (it = items; it; it = it->next) {
			if (it->redirtopic && !strcmp(msg->topic, it->redirtopic))
				/* modified setled() implementation */
				setled(it, msg->payload, 1, 1);
		}
	}
}

int main(int argc, char *argv[])
{
	int opt, ret, waittime;
	char *str;
	char mqtt_name[32];

	/* argument parsing */
	while ((opt = getopt_long(argc, argv, optstring, long_opts, NULL)) >= 0)
	switch (opt) {
	case 'V':
		fprintf(stderr, "%s %s\nCompiled on %s %s\n",
				NAME, VERSION, __DATE__, __TIME__);
		exit(0);
	case 'v':
		++loglevel;
		break;
	case 'm':
		mqtt_host = optarg;
		str = strrchr(optarg, ':');
		if (str > mqtt_host && *(str-1) != ']') {
			/* TCP port provided */
			*str = 0;
			mqtt_port = strtoul(str+1, NULL, 10);
		}
		break;
	case 's':
		mqtt_suffix = optarg;
		break;
	case 'w':
		mqtt_write_suffix = *optarg ? optarg : NULL;
		break;

	default:
		fprintf(stderr, "unknown option '%c'\n", opt);
	case '?':
		fputs(help_msg, stderr);
		exit(1);
		break;
	}

	myopenlog(NAME, 0, LOG_LOCAL2);
	myloglevel(loglevel);

	/* MQTT start */
	mosquitto_lib_init();
	sprintf(mqtt_name, "%s-%i", NAME, getpid());
	mosq = mosquitto_new(mqtt_name, true, 0);
	if (!mosq)
		mylog(LOG_ERR, "mosquitto_new failed: %s", ESTR(errno));
	/* mosquitto_will_set(mosq, "TOPIC", 0, NULL, mqtt_qos, 1); */

	mosquitto_log_callback_set(mosq, my_mqtt_log);
	mosquitto_message_callback_set(mosq, my_mqtt_msg);

	ret = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
	if (ret)
		mylog(LOG_ERR, "mosquitto_connect %s:%i: %s", mqtt_host, mqtt_port, mosquitto_strerror(ret));

	if (optind >= argc) {
		ret = mosquitto_subscribe(mosq, NULL, "#", mqtt_qos);
		if (ret)
			mylog(LOG_ERR, "mosquitto_subscribe '#': %s", mosquitto_strerror(ret));
	} else for (; optind < argc; ++optind) {
		ret = mosquitto_subscribe(mosq, NULL, argv[optind], mqtt_qos);
		if (ret)
			mylog(LOG_ERR, "mosquitto_subscribe %s: %s", argv[optind], mosquitto_strerror(ret));
	}

	while (1) {
		libt_flush();
		waittime = libt_get_waittime();
		if (waittime > 1000)
			waittime = 1000;
		ret = mosquitto_loop(mosq, waittime, 1);
		if (ret)
			mylog(LOG_ERR, "mosquitto_loop: %s", mosquitto_strerror(ret));
	}
	return 0;
}
