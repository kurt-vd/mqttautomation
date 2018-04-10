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

#define NAME "testteleruptor"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* generic error logging */
#define mylog(loglevel, fmt, ...) \
	({\
		syslog(loglevel, fmt, ##__VA_ARGS__); \
		if (loglevel <= LOG_ERR)\
			exit(1);\
	})
#define ESTR(num)	strerror(num)

static inline void myfree(void *ptr)
{
	if (ptr)
		free(ptr);
}

/* program options */
static const char help_msg[] =
	NAME ": provide a fake teleruptor with in+out topics\n"
	"usage:	" NAME " [OPTIONS ...] ctltopic statetopic\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
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
	{ "write", required_argument, NULL, 'w', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:w:";

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_write_suffix = "/set";
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;

/* state */
static struct mosquitto *mosq;
static char *topic_ctl, *topic_ctl_set = NULL, *topic_state;
static double mindelay = 0.25;
static double ctldelay = 0.1;
static int istate;
static int state;
static int ctl;

/* code */
static void publish(const char *topic, int value)
{
	int ret;

	ret = mosquitto_publish(mosq, NULL, topic, 1, value ? "1" : "0", mqtt_qos, 1);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", topic, mosquitto_strerror(ret));
}

static void republish(void *dat)
{
	publish(topic_ctl, ctl);
}

static void pulsehi(void *dat)
{
	istate = 2;
	state = !state;
	publish(topic_state, state);
}
static void pulselo(void *dat)
{
	istate = 0;
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	if (!strcmp(msg->topic, topic_ctl_set ?: topic_ctl)) {
		int newvalue;

		newvalue = strtol(msg->payload ?: "0", NULL, 0);

		if (newvalue != ctl && topic_ctl_set)
			libt_add_timeout(ctldelay, republish, NULL);
		ctl = newvalue;

		switch (istate) {
		case 0:
			if (newvalue) {
				/* start rising edge */
				++istate;
				libt_add_timeout(mindelay, pulsehi, NULL);
			}
			break;
		case 1:
			if (!newvalue) {
				/* abort rising edge */
				libt_remove_timeout(pulsehi, NULL);
				--istate;
			}
			break;
		case 2:
			if (!newvalue) {
				/* start falling edge */
				++istate;
				libt_add_timeout(mindelay, pulselo, NULL);
			}
			break;
		case 3:
			if (newvalue) {
				/* abort falling edge */
				--istate;
				libt_remove_timeout(pulselo, NULL);
			}
			break;
		}
	}
}

int main(int argc, char *argv[])
{
	int opt, ret, waittime;
	char *str;
	char mqtt_name[32];
	int logmask = LOG_UPTO(LOG_NOTICE);

	/* argument parsing */
	while ((opt = getopt_long(argc, argv, optstring, long_opts, NULL)) >= 0)
	switch (opt) {
	case 'V':
		fprintf(stderr, "%s %s\nCompiled on %s %s\n",
				NAME, VERSION, __DATE__, __TIME__);
		exit(0);
	case 'v':
		switch (logmask) {
		case LOG_UPTO(LOG_NOTICE):
			logmask = LOG_UPTO(LOG_INFO);
			break;
		case LOG_UPTO(LOG_INFO):
			logmask = LOG_UPTO(LOG_DEBUG);
			break;
		}
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
	case 'w':
		mqtt_write_suffix = optarg;
		break;

	default:
		fprintf(stderr, "unknown option '%c'\n", opt);
	case '?':
		fputs(help_msg, stderr);
		exit(1);
		break;
	}

	if (optind + 2 != argc) {
		fprintf(stderr, "no ctl & state topics found\n");
		fputs(help_msg, stderr);
		exit(1);
	}
	topic_ctl = argv[optind++];
	topic_state = argv[optind++];
	if (mqtt_write_suffix && *mqtt_write_suffix)
		asprintf(&topic_ctl_set, "%s%s", topic_ctl, mqtt_write_suffix);

	openlog(NAME, LOG_PERROR, LOG_LOCAL2);
	setlogmask(logmask);

	/* MQTT start */
	mosquitto_lib_init();
	sprintf(mqtt_name, "%s-%i", NAME, getpid());
	mosq = mosquitto_new(mqtt_name, true, 0);
	if (!mosq)
		mylog(LOG_ERR, "mosquitto_new failed: %s", ESTR(errno));

	mosquitto_message_callback_set(mosq, my_mqtt_msg);

	ret = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
	if (ret)
		mylog(LOG_ERR, "mosquitto_connect %s:%i: %s", mqtt_host, mqtt_port, mosquitto_strerror(ret));

	ret = mosquitto_subscribe(mosq, NULL, topic_ctl_set ?: topic_ctl, mqtt_qos);
	if (ret)
		mylog(LOG_ERR, "mosquitto_subscribe '%s': %s", topic_ctl_set ?: topic_ctl, mosquitto_strerror(ret));

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
