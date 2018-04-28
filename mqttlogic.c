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
#include <locale.h>

#include <unistd.h>
#include <getopt.h>
#include <syslog.h>
#include <mosquitto.h>

#include "lib/libt.h"
#include "rpnlogic.h"
#include "common.h"

#define NAME "mqttlogic"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* program options */
static const char help_msg[] =
	NAME ": an MQTT logic processor\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=STR	Give MQTT topic suffix for scripts (default '/logic')\n"
	" -S, --setsuffix=STR	Give MQTT topic suffix for scripts that write to /set (default '/setlogic')\n"
	" -c, --onchange=STR	Give MQTT topic suffix for onchange handler scripts (default '/onchange')\n"
	" -w, --write=STR	Give MQTT topic suffix for writing the topic on /logicw (default /set)\n"
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
	{ "Suffix", required_argument, NULL, 'S', },
	{ "onchange", required_argument, NULL, 'c', },
	{ "write", required_argument, NULL, 'w', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:s:S:c:w:";

/* logging */
static int loglevel = LOG_WARNING;

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_suffix = "/logic";
static const char *mqtt_setsuffix = "/setlogic";
static const char *mqtt_onchangesuffix = "/onchange";
static const char *mqtt_write_suffix = "/set";
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;
	struct item *prev;

	char *topic;
	char *writetopic;
	char *lastvalue;

	struct rpn *logic;
	struct rpn *onchange;
};

static struct item *items;

static struct stack rpnstack;

/* topic cache */
struct topic {
	char *topic;
	char *value;
	int ref;
	int isnew;
};
static struct topic *topics;
static int ntopics; /* used topics */
static int stopics;

#define myfree(x) ({ if (x) { free(x); (x) = NULL; }})

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

/* mqtt cache */
static int rpn_has_ref(struct rpn *rpn, const char *topic);

static int topiccmp(const void *a, const void *b)
{
	return strcmp(((const struct topic *)a)->topic ?: "", ((const struct topic *)b)->topic ?: "");
}

struct topic *get_topic(const char *name, int create)
{
	struct topic *topic;
	struct topic ref = { .topic = (char *)name, };
	struct item *it;

	topic = bsearch(&ref, topics, ntopics, sizeof(*topics), topiccmp);
	if (topic)
		return topic;
	if (!create)
		return NULL;
	/* make room */
	if (ntopics >= stopics) {
		stopics += 128;
		topics = realloc(topics, sizeof(*topics)*stopics);
	}
	topics[ntopics++] = (struct topic){ .topic = strdup(name), };
	qsort(topics, ntopics, sizeof(*topics), topiccmp);
	topic = get_topic(name, 0);

	/* set already referenced topics */
	for (it = items; it; it = it->next) {
		if (rpn_has_ref(it->logic, name))
			topic->ref += 1;
	}
	return topic;
}

static struct topic *lastrpntopic;
int rpn_env_isnew(void)
{
	return lastrpntopic->isnew;
}

const char *rpn_lookup_env(const char *name, struct rpn *rpn)
{
	struct topic *topic;

	topic = get_topic(name, 0);
	lastrpntopic = topic;
	if (!topic) {
		mylog(LOG_INFO, "topic %s not found", name);
		return NULL;
	}
	return topic->value;
}

int rpn_write_env(const char *value, const char *name, struct rpn *rpn)
{
	int ret;

	mylog(LOG_NOTICE, "mosquitto_publish %s%c%s", name, rpn->cookie ? '=' : '>', value);
	ret = mosquitto_publish(mosq, NULL, name, strlen(value), value, mqtt_qos, rpn->cookie);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", name, mosquitto_strerror(ret));
	return ret;
}

/* replace all relative topic references to absolute */
static char *resolve_relative_path(const char *path, const char *ref)
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

static void rpn_resolve_relative(struct rpn *rpn, const char *topic)
{
	char *abstopic;

	for (; rpn; rpn = rpn->next) {
		if (!rpn->topic)
			continue;
		abstopic = resolve_relative_path(rpn->topic, topic);
		if (abstopic) {
			free(rpn->topic);
			rpn->topic = abstopic;
		}
	}
}

/* logic items */
static void rpn_add_ref(struct rpn *rpn, int add)
{
	struct topic *topic;

	for (; rpn; rpn = rpn->next) {
		if (!rpn->topic)
			continue;
		topic = get_topic(rpn->topic, 0);
		if (!topic)
			continue;
		topic->ref += add;
	}
}
#define rpn_ref(rpn)	rpn_add_ref((rpn), +1)
#define rpn_unref(rpn)	rpn_add_ref((rpn), -1)
static int rpn_has_ref(struct rpn *rpn, const char *topic)
{
	for (; rpn; rpn = rpn->next) {
		if (rpn->topic && !strcmp(topic, rpn->topic))
			return 1;
	}
	return 0;
}

static int rpn_referred(struct rpn *rpn, void *dat)
{
	for (; rpn; rpn = rpn->next) {
		if (rpn == dat)
			return 1;
	}
	return 0;
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

static struct item *get_item(const char *topic, const char *suffix, int create)
{
	struct item *it;
	int matchlen;

	matchlen = strlen(topic ?: "") - strlen(suffix ?: "");
	/* match suffix */
	if (strcmp(topic+matchlen, suffix ?: ""))
		return NULL;

	for (it = items; it; it = it->next)
		if (!strncmp(it->topic, topic, matchlen) && !it->topic[matchlen])
			return it;
	if (!create)
		return NULL;
	/* not found, create one */
	it = malloc(sizeof(*it));
	memset(it, 0, sizeof(*it));
	/* set topic */
	it->topic = strdup(topic);
	it->topic[matchlen] = 0;
	/* set write topic */
	if (suffix == mqtt_setsuffix)
		asprintf(&it->writetopic, "%s%s", it->topic, mqtt_write_suffix);

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

static void drop_item(struct item *it, struct rpn **prpn)
{
	if (*prpn) {
		rpn_unref(*prpn);
		rpn_free_chain(*prpn);
		*prpn = NULL;
	}
	if (it->logic || it->onchange)
		return;
	/* remove from list */
	if (it->prev)
		it->prev->next = it->next;
	if (it->next)
		it->next->prev = it->prev;
	/* free memory */
	free(it->topic);
	myfree(it->writetopic);
	myfree(it->lastvalue);
	free(it);
}

static void do_logic(struct item *it, struct topic *trigger)
{
	int ret;
	const char *result;

	lastrpntopic = NULL;
	rpn_stack_reset(&rpnstack);
	if (trigger)
		trigger->isnew = 1;
	ret = rpn_run(&rpnstack, it->logic);
	if (trigger)
		trigger->isnew = 0;
	if (ret < 0 || !rpnstack.n)
		/* TODO: alert */
		return;
	result = rpnstack.strvalue ?: mydtostr(rpnstack.v[rpnstack.n-1]);
	/* test if we found something new */
	if (!strcmp(it->lastvalue ?: "", result))
		return;
	else if (trigger && !strcmp(trigger->topic, it->topic)) {
		/* This new calculation is triggered by the topic itself: beware loops */
		if (!strcmp(result, trigger->value ?: ""))
			/* our result changed to the current value: ok
			 * no need to republish
			 */
			goto save_cache;
		mylog(LOG_WARNING, "logic for '%s': avoid endless loop (was %s, new %s)", it->topic, it->lastvalue, result);
		return;
	}

	mylog(LOG_NOTICE, "mosquitto_publish %s%c%s", it->writetopic ?: it->topic, it->writetopic ? '>' : '=', result);
	ret = mosquitto_publish(mosq, NULL, it->writetopic ?: it->topic, strlen(result), result, mqtt_qos, !it->writetopic);
	if (ret < 0) {
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->writetopic ?: it->topic, mosquitto_strerror(ret));
		return;
	}
	/* save cache */
save_cache:
	if (it->lastvalue)
		free(it->lastvalue);
	it->lastvalue = strdup(result);
}

static void do_onchanged(struct item *it)
{
	lastrpntopic = NULL;
	rpn_stack_reset(&rpnstack);
	rpn_run(&rpnstack, it->onchange);
}

void rpn_run_again(void *dat)
{
	struct item *it = ((struct rpn *)dat)->dat;

	if (rpn_referred(it->logic, dat))
		do_logic(it, NULL);
	else if (rpn_referred(it->onchange, dat))
		do_onchanged(it);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	struct item *it;
	struct topic *topic;

	if (!strcmp(msg->topic, "tools/loglevel")) {
		mysetloglevelstr(msg->payload);
	} else if (test_suffix(msg->topic, mqtt_suffix)) {
		/* this is a logic set msg */
		it = get_item(msg->topic, mqtt_suffix, msg->payloadlen);
		if (!it || !msg->payloadlen) {
			if (it)
				drop_item(it, &it->logic);
			return;
		}
		if (it->writetopic) {
			free(it->writetopic);
			it->writetopic = NULL;
		}
		/* remove old logic */
		rpn_unref(it->logic);
		rpn_free_chain(it->logic);
		/* prepare new info */
		it->logic = rpn_parse(msg->payload, it);
		rpn_resolve_relative(it->logic, it->topic);
		rpn_ref(it->logic);
		mylog(LOG_INFO, "new logic for %s", it->topic);
		/* ready, first run */
		do_logic(it, NULL);
		return;
	} else if (test_suffix(msg->topic, mqtt_setsuffix)) {
		/* this is a logic set msg */
		it = get_item(msg->topic, mqtt_setsuffix, msg->payloadlen);
		if (!it || !msg->payloadlen) {
			if (it)
				drop_item(it, &it->logic);
			return;
		}
		if (!it->writetopic)
			asprintf(&it->writetopic, "%s%s", it->topic, mqtt_write_suffix);
		/* remove old logic */
		rpn_unref(it->logic);
		rpn_free_chain(it->logic);
		/* prepare new info */
		it->logic = rpn_parse(msg->payload, it);
		rpn_resolve_relative(it->logic, it->topic);
		rpn_ref(it->logic);
		mylog(LOG_INFO, "new setlogic for %s", it->topic);
		/* ready, first run */
		do_logic(it, NULL);
		return;
	} else if (test_suffix(msg->topic, mqtt_onchangesuffix)) {
		it = get_item(msg->topic, mqtt_onchangesuffix, msg->payloadlen);
		if (!it || !msg->payloadlen) {
			if (it)
				drop_item(it, &it->onchange);
			return;
		}
		/* remove old logic */
		rpn_unref(it->onchange);
		rpn_free_chain(it->onchange);
		/* prepare new info */
		it->onchange = rpn_parse(msg->payload, it);
		rpn_resolve_relative(it->onchange, it->topic);
		rpn_ref(it->onchange);
		mylog(LOG_INFO, "new onchange for %s", it->topic);
		return;
	}
	/* find topic */
	topic = get_topic(msg->topic, msg->payloadlen);
	if (topic) {
		free(topic->value);
		topic->value = strndup(msg->payload ?: "", msg->payloadlen);
		if (topic->ref) {
			for (it = items; it; it = it->next) {
				if (rpn_has_ref(it->logic, msg->topic))
					do_logic(it, topic);
			}
		}
	}
	/* run onchange logic */
	it = get_item(msg->topic, "", 0);
	if (it) {
		if (it->onchange)
			do_onchanged(it);

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
	case 'S':
		mqtt_setsuffix = optarg;
		break;
	case 'c':
		mqtt_onchangesuffix = optarg;
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

	myopenlog(NAME, 0, LOG_LOCAL2);
	myloglevel(loglevel);
	setlocale(LC_TIME, "");

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
