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

#define NAME "mqttmotor"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* program options */
static const char help_msg[] =
	NAME ": an MQTT to motor driver (H-bridge)\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=STR	Give MQTT topic suffix for timeouts (default '/motorhw')\n"
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
static const char *mqtt_suffix = "/motorhw";
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
	int type;
#define MAX_OUT 3
	char *sysfsdir[MAX_OUT];
};

struct item *items;

/* utils */
static void myfree(void *ptr)
{
	if (ptr)
		free(ptr);
}

/* types */
static const char *const types[] = {
	"L293D",
#define L293D 0
	"SN754410",
#define SN754410 1
	NULL,
};

static int lookup_type(const char *type)
{
	const char *const *lp;

	for (lp = types; *lp; ++lp) {
		if (!strcasecmp(*lp, type))
			return lp - types;
	}
	return -1;
}

/* sysfs attrs */
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
		MOSQ_LOG_DEBUG, LOG_DEBUG,
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

	if (!nodename || !strcmp("*", nodename))
		/* empty nodename matches always, for local hosts */
		return !strcmp(mqtt_host, "localhost") ||
			!strncmp(mqtt_host, "127.", 4) ||
			!strcmp(mqtt_host, "::1");

	gethostname(mynodename, sizeof(mynodename));
	return !strcmp(mynodename, nodename);
}

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
	int ret, j;

	/* remove from list */
	if (it->prev)
		it->prev->next = it->next;
	if (it->next)
		it->next->prev = it->prev;

	ret = mosquitto_unsubscribe(mosq, NULL, it->writetopic ?: it->topic);
	if (ret)
		mylog(LOG_ERR, "mosquitto_unsubscribe '%s': %s", it->writetopic ?: it->topic, mosquitto_strerror(ret));
	/* free memory */
	free(it->topic);
	myfree(it->writetopic);
	for (j = 0; j < MAX_OUT; ++j)
		myfree(it->sysfsdir[j]);
	free(it);
}

static void find_led(char **dst, const char *name)
{
	/* find full path for led or brightness */
	static const char *const sysfsdir_fmts[] = {
		"/sys/class/leds/%s",
		"/tmp/led/%s",
		NULL,
	};
	char *path;
	struct stat st;
	int j;

	if (!strcmp("...", name)) {
		/* special case, fake led */
		*dst = strdup("...");
		return;
	}

	for (j = 0; sysfsdir_fmts[j]; ++j) {
		asprintf(&path, sysfsdir_fmts[j], name);
		if (!stat(path, &st)) {
			*dst = path;
			break;
		}
		free(path);
	}
}

static int set_led(const char *H, const char *sysfsdir, int on)
{
	int ret;

	if (!strcmp(sysfsdir, "..."))
		return 0;
	ret = attr_write(on ? "255" : "0", "%s/value", sysfsdir);
	if (ret < 0)
		mylog(LOG_WARNING, "failed to write %i to led %s for H %s",
				on ? 255 : 0, sysfsdir, H);
	return ret;
}

static void set_H(struct item *it, const char *newvalue, int republish)
{
	int ret, newval, idle;
	char *endp;
	int en, a, b;

	idle = !newvalue || !*newvalue;
	newval = strtol(newvalue ?: "", &endp, 0);
	if (!idle && endp == newvalue) {
		if (!strcasecmp(newvalue, "brake"))
			newval = 0;
		else if (!strcasecmp(newvalue, "stop"))
			newval = 0;
		else if (!strcasecmp(newvalue, "left"))
			newval = -1;
		else if (!strcasecmp(newvalue, "right"))
			newval = 1;
		else if (!strcasecmp(newvalue, "ccw"))
			newval = -1;
		else if (!strcasecmp(newvalue, "cw"))
			newval = 1;
		else if (!strcasecmp(newvalue, "idle"))
			idle = 1;
	}

	switch (it->type) {
	case L293D:
	case SN754410:
		en = !idle;
		a = !idle && newval >= 0;
		b = !idle && newval <= 0;
		if (set_led(it->topic, it->sysfsdir[0], a) < 0 ||
			set_led(it->topic, it->sysfsdir[1], b) < 0 ||
			set_led(it->topic, it->sysfsdir[2], en) < 0)
			goto failed;
		break;
	default:
		break;
	}

	if (republish && mqtt_write_suffix) {
		/* publish, retained when writing the topic, volatile (not retained) when writing to another topic */
		ret = mosquitto_publish(mosq, NULL, it->topic, strlen(newvalue ?: ""), newvalue, mqtt_qos, 1);
		if (ret < 0)
			mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
	}
	return;
failed:;
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int forme, j;
	char *str;
	struct item *it;

	if (!strcmp(msg->topic, "tools/loglevel")) {
		mysetloglevelstr(msg->payload);
	} else if (test_suffix(msg->topic, mqtt_suffix)) {
		/* grab boardname */
		forme = test_nodename(strtok(msg->payload ?: "", " \t"));
		it = get_item(msg->topic, mqtt_suffix, !!msg->payloadlen && forme);
		if (!it)
			return;

		/* this is a spec msg */
		if (!msg->payloadlen || !forme) {
			mylog(LOG_INFO, "removed H spec for %s", it->topic);
			drop_item(it);
			return;
		}

		/* free old spec */
		for (j = 0; j < MAX_OUT; ++j)
			myfree(it->sysfsdir[j]);
		memset(it->sysfsdir, 0, sizeof(it->sysfsdir));

		/* find type */
		str = strtok(NULL, " \t");
		it->type = lookup_type(str);
		if (it->type < 0) {
			mylog(LOG_WARNING, "bad type '%s' for H %s", str, it->topic);
			drop_item(it);
			return;
		}

		for (j = 0; j < MAX_OUT; ++j) {
			str = strtok(NULL, " \t");
			if (!str)
				break;
			find_led(&it->sysfsdir[j], str);
			if (!it->sysfsdir[j]) {
				mylog(LOG_WARNING, "bad led '%s' for H %s", str, it->topic);
				drop_item(it);
				return;
			}
		}

		/* finalize */
		mylog(LOG_INFO, "new spec for H %s", it->topic);

	} else if ((it = get_item(msg->topic, mqtt_write_suffix, 0)) != NULL) {
		/* this is the write topic */
		if (!msg->retain)
			set_H(it, msg->payload, 1);

	} else if ((!mqtt_write_suffix || msg->retain) &&
			(it = get_item(msg->topic, NULL, 0)) != NULL) {
		/* this is the main led topic */
		set_H(it, msg->payload, 0);
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
