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

#include <unistd.h>
#include <getopt.h>
#include <locale.h>
#include <syslog.h>
#include <mosquitto.h>

#include "common.h"

#define NAME "mqttimport"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* program options */
static const char help_msg[] =
	NAME ": an MQTT topic importer\n"
	"usage:	" NAME " [OPTIONS ...]\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -q, --qos=QoS		Set QoS to use (default 1)\n"
	" -t, --type=TYPE	Set import type:\n"
	"			force: unconditionally write all topics\n"
	"			normal: write topics only when different\n"
	"			missing: only write missing topics\n"
	" -f, --force		(legacy) -tforce\n"
	" -n, --dry-run		Don't actually send info, only report what would be done\n"
	;

#ifdef _GNU_SOURCE
static struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "mqtt", required_argument, NULL, 'm', },
	{ "qos", required_argument, NULL, 'q', },
	{ "type", required_argument, NULL, 't', },
	{ "force", no_argument, NULL, 'f', },
	{ "dry-run", no_argument, NULL, 'n', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:q:t:fn";

/* logging */
static int loglevel = LOG_WARNING;

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;
static int type = 0;
#define TYPE_FORCE	3
#define TYPE_NORMAL	2
#define TYPE_MISSING	1
static int dryrun;

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;
	struct item *prev;

	int imported;
	char *topic;
	char *value;
};

static struct item *items;

/* signalling */
static void onsigterm(int signr)
{
	sigterm = 1;
}

/* self-sync */
static char *myuuid;
static const char selfsynctopic[] = "tmp/selfsync";
static void send_self_sync(struct mosquitto *mosq)
{
	int ret;

	asprintf(&myuuid, "%i-%li-%i", getpid(), time(NULL), rand());

	ret = mosquitto_subscribe(mosq, NULL, selfsynctopic, mqtt_qos);
	if (ret)
		mylog(LOG_ERR, "mosquitto_subscribe %s: %s", selfsynctopic, mosquitto_strerror(ret));
	ret = mosquitto_publish(mosq, NULL, selfsynctopic, strlen(myuuid), myuuid, mqtt_qos, 0);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", selfsynctopic, mosquitto_strerror(ret));
}
static int is_self_sync(const struct mosquitto_message *msg)
{
	return !strcmp(msg->topic, selfsynctopic) &&
		!strcmp(myuuid ?: "", msg->payload ?: "");
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

static struct item *get_item(const char *topic)
{
	struct item *it;

	for (it = items; it; it = it->next)
		if (!strcmp(it->topic, topic)) {
			mylog(LOG_ERR, "duplicate topic '%s' specified", topic);
			return NULL;
		}
	/* not found, create one */
	it = malloc(sizeof(*it));
	memset(it, 0, sizeof(*it));
	it->topic = strdup(topic);

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
	/* remove from list */
	if (it->prev)
		it->prev->next = it->next;
	if (it->next)
		it->next->prev = it->prev;
	/* free memory */
	free(it->topic);
	if (it->value)
		free(it->value);
	free(it);
}

static void send_item(struct item *it, const char *msg)
{
	int ret;

	if (dryrun) {
		mylog(LOG_NOTICE, "%s %s=%s", msg, it->topic, it->value ?: "");
		drop_item(it);
		return;
	}
	mylog(LOG_NOTICE, "%s %s", msg, it->topic);
	ret = mosquitto_publish(mosq, NULL, it->topic, strlen(it->value ?: ""), it->value, mqtt_qos, 1);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
	it->imported = 1;
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	struct item *it, *next;

	if (is_self_sync(msg)) {
		for (it = items; it; it = next) {
			next = it->next;
			send_item(it, "import");
		}
	}

	for (it = items; it; it = it->next) {
		if (!strcmp(msg->topic, it->topic)) {
			if (it->imported)
				drop_item(it);
			else if (type == TYPE_NORMAL && strcmp(msg->payload ?: "", it->value ?: ""))
				send_item(it, "update");
			else {
				mylog(LOG_INFO, "leave %s", it->topic);
				drop_item(it);
			}
			break;
		}
	}
}

int main(int argc, char *argv[])
{
	int opt, ret;
	char *str, *tok;
	char mqtt_name[32];
	char *line;
	size_t linesize;
	struct item *it;

	setlocale(LC_ALL, "");
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
	case 'q':
		mqtt_qos = strtoul(optarg, NULL, 0);
		break;
	case 'f':
		type = TYPE_FORCE;
		break;
	case 't':
		if (!strcmp(optarg, "force"))
			type = TYPE_FORCE;
		else if (!strcmp(optarg, "normal"))
			type = TYPE_NORMAL;
		else if (!strcmp(optarg, "missing"))
			type = TYPE_MISSING;
		else
			goto printhelp;
		break;
	case 'n':
		dryrun = 1;
		break;

	default:
		fprintf(stderr, "unknown option '%c'", opt);
	case '?':
printhelp:
		fputs(help_msg, stderr);
		exit(1);
		break;
	}

	myopenlog(NAME, 0, LOG_LOCAL2);
	myloglevel(loglevel);
	signal(SIGINT, onsigterm);
	signal(SIGTERM, onsigterm);

	/* MQTT start */
	mosquitto_lib_init();
	sprintf(mqtt_name, "%s-%i", NAME, getpid());
	mosq = mosquitto_new(mqtt_name, true, 0);
	if (!mosq)
		mylog(LOG_ERR, "mosquitto_new failed: %s", ESTR(errno));

	mosquitto_log_callback_set(mosq, my_mqtt_log);
	mosquitto_message_callback_set(mosq, my_mqtt_msg);

	ret = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
	if (ret)
		mylog(LOG_ERR, "mosquitto_connect %s:%i: %s", mqtt_host, mqtt_port, mosquitto_strerror(ret));

	/* parse input file */
	line = NULL;
	linesize = 0;
	while (1) {
		ret = getline(&line, &linesize, stdin);
		if (ret < 0) {
			if (feof(stdin))
				break;
			mylog(LOG_ERR, "readline <stdin>: %s", ESTR(errno));
		}
		if (line[ret-1] == '\n')
			/* cut newline */
			line[ret-1] = 0;
		tok = strtok(line, " \t");
		if (line[0] == '#' || !tok || !strlen(tok))
			/* ignore such line */
			continue;
		it = get_item(tok);
		if (!it)
			continue;
		/* find remainder */
		tok = strtok(NULL, "");
		if (tok)
			it->value = strdup(tok);
		/* subscribe to topic */
		ret = mosquitto_subscribe(mosq, NULL, it->topic, mqtt_qos);
		if (ret)
			mylog(LOG_ERR, "mosquitto_subscribe %s: %s", it->topic, mosquitto_strerror(ret));
		if (type == TYPE_FORCE)
			send_item(it, "force");
	}
	if (line)
		free(line);
	send_self_sync(mosq);

	/* loop */
	while (!sigterm && items) {
		ret = mosquitto_loop(mosq, 1000, 1);
		if (ret)
			mylog(LOG_ERR, "mosquitto_loop: %s", mosquitto_strerror(ret));
	}
	return 0;
}
