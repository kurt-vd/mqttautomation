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
#include <fcntl.h>
#include <getopt.h>
#include <syslog.h>
#include <wordexp.h>
#include <sys/stat.h>
#include <mosquitto.h>

#include "lib/libt.h"
#include "common.h"

#define NAME "mqttsysfsrd"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* program options */
static const char help_msg[] =
	NAME ": publish sysfs attributes into MQTT\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"usage:	" NAME " [OPTIONS ...] PATH=TOPIC[,mul=MULTIPLIER][,samplerate=REPEAT] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=STR	Give MQTT topic suffix for spec (default '/sysfsrd')\n"
	"\n"
	"Paramteres\n"
	" PATTERN	A pattern to subscribe for\n"
	"\n"
	".../sysfsrd format\n"
	" 'HOST|* PATH [mul=MULTIPLIER] [rate=SAMPLINGPERIOD] [enum STR=VAL ...]'\n"
	;

#ifdef _GNU_SOURCE
static struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "mqtt", required_argument, NULL, 'm', },
	{ "suffix", required_argument, NULL, 's', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:s:";

/* logging */
static int loglevel = LOG_WARNING;

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_suffix = "/sysfsrd";
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;

/* state */
static struct mosquitto *mosq;

struct map {
	double v;
	char *s;
};
struct item {
	struct item *next;
	struct item *prev;

	char *topic;
	int topiclen;
	char *sysfs;
	long lastvalue;
	double mul;
	double samplerate;

	int lasterr;

	struct map *map;
	int nmap, mapsize;
};

struct item *items;

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

static struct item *get_item(const char *topic, const char *suffix, int create)
{
	struct item *it;
	int len;

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

static void drop_map(struct item *it)
{
	int j;

	for (j = 0; j < it->nmap; ++j)
		free(it->map[j].s);
	free(it->map);
	/* clear values */
	it->map = NULL;
	it->nmap = it->mapsize = 0;
}

static void add_map(struct item *it, double v, const char *s)
{
	if (it->nmap+1 > it->mapsize) {
		it->mapsize += 16;
		it->map = realloc(it->map, it->mapsize*sizeof(*it->map));
		if (!it->map)
			mylog(LOG_ERR, "realloc map failed for %s: %s", it->topic, ESTR(errno));
	}
	it->map[it->nmap].v = v;
	it->map[it->nmap].s = strdup(s ?: "");
	++it->nmap;
}

static void pub_it(void *dat);
static void drop_item(struct item *it)
{
	/* remove from list */
	if (it->prev)
		it->prev->next = it->next;
	if (it->next)
		it->next->prev = it->prev;
	/* clean mqtt topic */
	mosquitto_publish(mosq, NULL, it->topic, 0, NULL, 0, 1);
	libt_remove_timeout(pub_it, it);
	/* free memory */
	drop_map(it);
	free(it->topic);
	if (it->sysfs)
		free(it->sysfs);
	free(it);
}

static void parse_map(struct item *it)
{
	char *key, *value;

	for (;;) {
		key = strtok(NULL, " \t");
		if (!key)
			break;
		value = strchr(key, '=');
		if (!value)
			break;
		/* insert null seperator */
		*value++ = 0;
		add_map(it, strtod(value, NULL), key);
	}
}

static const char *const we_err[] = {
	[WRDE_BADCHAR] = "Illegal occurrence of newline or one of |, &, ;, <, >, (, ), {, }",
	[WRDE_BADVAL] = "An undefined shell variable was referenced, with WRDE_UNDEF",
	[WRDE_CMDSUB] = "Command substitution occurred with WRDE_NOCMD",
	[WRDE_NOSPACE] = "Out of memory",
	[WRDE_SYNTAX] = "Shell syntax error, such as unbalanced parentheses or unmatched quotes",
};

static int find_path(struct item *it, const char *pattern)
{
	int ret;
	char *path;
	wordexp_t we = {};

	ret = wordexp(pattern, &we, WRDE_NOCMD | WRDE_REUSE);
	if (ret) {
		mylog(LOG_WARNING, "'%s': %s", pattern, we_err[ret] ?: "uknonwn error");
		return -1;
	}
	if (we.we_wordc < 1) {
		mylog(LOG_WARNING, "'%s': no result", pattern);
		return -1;
	}
	path = we.we_wordv[0];
	if (stat(path, &(struct stat){}) != 0) {
		mylog(LOG_WARNING, "'%s' failed: %s", path, ESTR(errno));
		return -1;
	}
	if (it->sysfs)
		free(it->sysfs);
	it->sysfs = strdup(path);
	return 0;
}

/* read hw */
static inline int err_is_new(struct item *it, int errnum)
{
	if (it->lasterr == errnum)
		return 0;
	it->lasterr = errnum;
	return 1;
}
static void pub_it(void *dat)
{
	struct item *it = dat;
	static char strvalue[128];
	int fd, ret, j;
	long value;

	fd = open(it->sysfs, O_RDONLY);
	if (fd < 0) {
		if (err_is_new(it, errno))
			mylog(LOG_WARNING, "open %s failed: %s", it->sysfs, ESTR(errno));
		goto fail_open;
	}
	ret = read(fd, strvalue, sizeof(strvalue)-1);
	if (ret < 0) {
		if (err_is_new(it, errno))
			mylog(LOG_WARNING, "read %s failed: %s", it->sysfs, ESTR(errno));
		goto fail_read;
	}
	close(fd);
	if (err_is_new(it, 0))
		mylog(LOG_WARNING, "%s back to normal", it->sysfs);
	/* null-terminate value */
	strvalue[ret] = 0;

	if (it->map) {
		if (ret > 0 && strvalue[ret-1] == '\n')
			strvalue[--ret] = 0;
		/* value is not parsed as int, but the index of the enum map */
		for (j = 0; j < it->nmap; ++j) {
			if (!strcasecmp(it->map[j].s, strvalue))
				break;
		}
		value = j;
		mylog(LOG_DEBUG, "%s: got %s, %i", it->topic, strvalue, j);
	} else
		value = strtol(strvalue, NULL, 0);

	if (value != it->lastvalue) {
		const char *str = it->map ? ((value >= it->nmap) ? "" : mydtostr(it->map[value].v)): mydtostr(value * it->mul);

		/* publish, retained when writing the topic, volatile (not retained) when writing to another topic */
		ret = mosquitto_publish(mosq, NULL, it->topic, strlen(str), str, mqtt_qos, 1);
		if (ret < 0)
			mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
		else
			it->lastvalue = value;
	}
	libt_repeat_timeout(it->samplerate, pub_it, dat);
	return;
fail_read:
	close(fd);
fail_open:
	libt_repeat_timeout(60, pub_it, dat);
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

	if (!nodename || !strcmp(nodename, ".") || !strcmp(nodename, "*"))
		/* empty nodename matches always, for local hosts */
		return !strcmp(mqtt_host, "localhost") ||
			!strncmp(mqtt_host, "127.", 4) ||
			!strcmp(mqtt_host, "::1");

	gethostname(mynodename, sizeof(mynodename));
	return !strcmp(mynodename, nodename);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int forme;
	char *key, *value;
	struct item *it;

	if (!strcmp(msg->topic, "tools/loglevel")) {
		mysetloglevelstr(msg->payload);

	} else if (test_suffix(msg->topic, mqtt_suffix)) {
		/* this is a config msg */
		forme = test_nodename(strtok(msg->payload, " \t"));
		it = get_item(msg->topic, mqtt_suffix, !!msg->payloadlen && forme);
		if (!it)
			return;
		/* remove on null config */
		if (!msg->payloadlen || !forme) {
			mylog(LOG_INFO, "removed sysfsrdcfg spec for %s", it->topic);
			drop_item(it);
			return;
		}
		/* re-initialize defaults */
		it->mul = 1e-3;
		it->samplerate = 1;
		drop_map(it);
		/* invalidate cache */
		it->lastvalue = -1;
		/* parse path */
		value = strtok(NULL, " \t");
		if (!value)
			mylog(LOG_INFO, "no sysfs path defined for %s", it->topic);
		if (find_path(it, value ?: "") < 0) {
			mylog(LOG_NOTICE, "%s: no path '%s'", it->topic, value ?: "");
			drop_item(it);
			return;
		}

		for (;;) {
			key = strtok(NULL, " \t");
			if (!key)
				break;
			value = strchr(key, '=');
			if (value)
				/* null terminate */
				*value++ = 0;
			if (!strcmp(key, "mul"))
				it->mul = strtod(value, NULL);
			else if (!strcmp(key, "samplerate"))
				it->samplerate = strtod(value, NULL);
			else if (!strcmp(key, "enum"))
				parse_map(it);
			else
				mylog(LOG_WARNING, "unknown attribute '%s' for %s", key, it->topic);
		}
		mylog(LOG_INFO, "new mqttfromsysfs spec for %s: %s", it->topic, it->sysfs);
		if (it->map) {
			int j;
			for (j = 0; j < it->nmap; ++j)
				mylog(LOG_DEBUG, "	%s=%s", it->map[j].s, mydtostr(it->map[j].v));
		}
		pub_it(it);
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
