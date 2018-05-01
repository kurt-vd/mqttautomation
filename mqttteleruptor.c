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

#define NAME "mqttteleruptor"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

static inline void myfree(void *ptr)
{
	if (ptr)
		free(ptr);
}

/* program options */
static const char help_msg[] =
	NAME ": Control teleruptors using 2 independant mqtt topics\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=STR	Give MQTT topic suffix for configuration (default '/teleruptorcfg')\n"
	" -w, --write=STR	Give MQTT topic suffix for writing the topic (default /set)\n"
	" -S, --nosuffix	Write control topic without suffix\n"
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
	{ "nosuffix", no_argument, NULL, 'S', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:s:w:S";

/* logging */
static int loglevel = LOG_WARNING;

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_suffix = "/teleruptorcfg";
static const char *mqtt_write_suffix = "/set";
static int mqtt_suffixlen = 14;
static int no_mqtt_ctl_suffix;
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
	/* ctltopic: topic to control the teleruptor */
	char *ctltopic;
	char *ctlwrtopic;
	/* statetopic: topic that reads back the teleruptor */
	char *statetopic;
	/* requested value */
	int reqval;
	/* current value */
	int currval;
	/* the value of the ctr line, with potential feedback */
	int ctlval;
	int currctlval;
	/* # retries passed */
	int nretry;
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

static int test_suffix(const char *topic, const char *suffix)
{
	int len;

	len = strlen(topic ?: "") - strlen(suffix ?: "");
	if (len < 0)
		return 0;
	/* match suffix */
	return !strcmp(topic+len, suffix ?: "");
}

static struct item *get_item_by_ctl(const char *topic)
{
	struct item *it;

	for (it = items; it; it = it->next) {
		if (!strcmp(it->ctltopic, topic))
			return it;
	}
	return NULL;
}

static struct item *get_item_by_state(const char *topic)
{
	struct item *it;

	for (it = items; it; it = it->next) {
		if (!strcmp(it->statetopic, topic))
			return it;
	}
	return NULL;
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
	it->reqval = it->currval = it->currctlval = -1; /* mark invalid */
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

static void idle_teleruptor(void *dat);
static void set_teleruptor(void *dat);
static void reset_teleruptor(void *dat);

static void drop_item(struct item *it)
{
	int ret;

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
	free(it->ctltopic);
	myfree(it->ctlwrtopic);
	free(it->statetopic);
	free(it);
	/* free timers */
	libt_remove_timeout(set_teleruptor, it);
	libt_remove_timeout(reset_teleruptor, it);
	libt_remove_timeout(idle_teleruptor, it);
}

/* timer callbacks */
static void idle_teleruptor(void *dat)
{
	struct item *it = dat;

	/* return to idle */
	it->ctlval = 0;
	if (it->ctlwrtopic && it->currctlval == 1) {
		/* teleruptor control is not working */
		mylog(LOG_WARNING, "teleruptor control %s does not respond", it->topic);
		return;
	}
	if (it->reqval >= 0 && it->reqval != it->currval)
		/* retry */
		set_teleruptor(it);
}

static void reset_teleruptor(void *dat)
{
	struct item *it = dat;
	int ret;

	if (it->ctlwrtopic && it->currctlval == 0) {
		/* teleruptor control is not working */
		mylog(LOG_WARNING, "teleruptor control %s does not respond", it->topic);
		it->ctlval = 0;
		return;
	}
	ret = mosquitto_publish(mosq, NULL, it->ctlwrtopic ?: it->ctltopic, 1, "0", mqtt_qos, !it->ctlwrtopic);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->ctlwrtopic ?: it->ctltopic, mosquitto_strerror(ret));
	it->ctlval = 2;
	libt_add_timeout(0.5, idle_teleruptor, it);
}

static void set_teleruptor(void *dat)
{
	struct item *it = dat;
	int ret;

	mylog(LOG_INFO, "change teleruptor %s", it->topic);
	if (++it->nretry > 3) {
		mylog(LOG_WARNING, "teleruptor %s keeps failing", it->topic);
		return;
	}
	ret = mosquitto_publish(mosq, NULL, it->ctlwrtopic ?: it->ctltopic, 1, "1", mqtt_qos, !it->ctlwrtopic);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->ctlwrtopic ?: it->ctltopic, mosquitto_strerror(ret));
	it->ctlval = 1;
	libt_add_timeout(0.5, reset_teleruptor, it);
}

static void setvalue(struct item *it, int newvalue)
{
	it->reqval = newvalue;
	if (it->reqval == it->currval)
		return;
	it->nretry = 0;
	if (!it->ctlval)
		/* start modifying teleruptor state */
		set_teleruptor(it);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int ret;
	struct item *it;

	if (!strcmp(msg->topic, "tools/loglevel")) {
		mysetloglevelstr(msg->payload);
	} else if (test_suffix(msg->topic, mqtt_suffix)) {
		char *ctl, *state;

		ctl = strtok(msg->payload ?: "", " \t");
		state = strtok(NULL, " \t");
		/* grab boardname */
		it = get_item(msg->topic, mqtt_suffix, ctl && state);
		if (!it)
			return;

		/* this is a spec msg */
		if (!msg->payloadlen) {
			mylog(LOG_INFO, "removed teleruptor spec for %s", it->topic);
			drop_item(it);
			return;
		}
		/* reconfigure in */
		if (!it->statetopic || strcmp(it->statetopic, state)) {
			if (it->statetopic) {
				ret = mosquitto_unsubscribe(mosq, NULL, it->statetopic);
				if (ret)
					mylog(LOG_ERR, "mosquitto_unsubscribe '%s': %s", it->statetopic, mosquitto_strerror(ret));
				free(it->statetopic);
			}
			it->statetopic = resolve_relative_path(state, it->topic) ?: strdup(state);
			ret = mosquitto_subscribe(mosq, NULL, it->statetopic, mqtt_qos);
			if (ret)
				mylog(LOG_ERR, "mosquitto_subscribe '%s': %s", it->statetopic, mosquitto_strerror(ret));
			it->currval = -1;
		}
		/* reconfigure out */
		if (!it->ctltopic || strcmp(it->ctltopic, ctl)) {
			if (it->ctltopic && mqtt_write_suffix && !no_mqtt_ctl_suffix) {
				ret = mosquitto_unsubscribe(mosq, NULL, it->ctltopic);
				if (ret)
					mylog(LOG_ERR, "mosquitto_unsubscribe '%s': %s", it->ctltopic, mosquitto_strerror(ret));
			}
			myfree(it->ctltopic);
			it->ctltopic = resolve_relative_path(ctl, it->topic) ?: strdup(ctl);
			if (mqtt_write_suffix && !no_mqtt_ctl_suffix) {
				free(it->ctlwrtopic);
				asprintf(&it->ctlwrtopic, "%s%s", it->ctltopic, mqtt_write_suffix);
				ret = mosquitto_subscribe(mosq, NULL, it->ctltopic, mqtt_qos);
				if (ret)
					mylog(LOG_ERR, "mosquitto_subscribe '%s': %s", it->ctltopic, mosquitto_strerror(ret));
			}
			it->ctlval = 0;
			it->nretry = 0;
			libt_remove_timeout(set_teleruptor, it);
			libt_remove_timeout(reset_teleruptor, it);
			libt_remove_timeout(idle_teleruptor, it);
		}
		setvalue(it, it->reqval);
		/* finalize */
		mylog(LOG_INFO, "new teleruptor spec for %s", it->topic);

	} else if ((it = get_item(msg->topic, mqtt_write_suffix, 0)) != NULL) {
		/* this is the write topic */
		if (!msg->retain)
			setvalue(it, strtoul(msg->payload ?: "0", NULL, 0));

	} else if ((!mqtt_write_suffix || msg->retain) &&
			(it = get_item(msg->topic, NULL, 0)) != NULL) {
		/* this is the main led topic */
		setvalue(it, strtoul(msg->payload ?: "0", NULL, 0));

	} else if ((it = get_item_by_state(msg->topic)) != NULL) {
		it->currval = strtol(msg->payload ?: "-1", NULL, 0);

	} else if ((it = get_item_by_ctl(msg->topic)) != NULL) {
		it->currctlval = strtol(msg->payload ?: "-1", NULL, 0);
		if (!it->ctlwrtopic && msg->retain)
			it->ctlval = it->currctlval;

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
		mqtt_suffixlen = strlen(mqtt_suffix);
		break;
	case 'w':
		mqtt_write_suffix = *optarg ? optarg : NULL;
		break;
	case 'S':
		no_mqtt_ctl_suffix = 1;
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
