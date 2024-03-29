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
#include <sys/signalfd.h>
#include <mosquitto.h>

#include "lib/libt.h"
#include "lib/libe.h"
#include "lib/libtimechange.h"
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
	" -n, --dry-run		don't actually set anything\n"
	"\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=STR	Give MQTT topic suffix for scripts (default '/logic')\n"
	" -S, --setsuffix=STR	Give MQTT topic suffix for scripts that write to /set (default '/setlogic')\n"
	" -c, --onchange=STR	Give MQTT topic suffix for onchange handler scripts (default '/onchange')\n"
	" -b, --button=STR	Give MQTT topic suffix for button handler scripts (default '/button')\n"
	" -B, --longbutton=STR	Give MQTT topic suffix for longbutton handler scripts (default '/longbutton')\n"
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
	{ "dry-run", no_argument, NULL, 'n', },

	{ "mqtt", required_argument, NULL, 'm', },
	{ "suffix", required_argument, NULL, 's', },
	{ "Suffix", required_argument, NULL, 'S', },
	{ "onchange", required_argument, NULL, 'c', },
	{ "write", required_argument, NULL, 'w', },
	{ "button", required_argument, NULL, 'b', },
	{ "longbutton", required_argument, NULL, 'B', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?nm:s:S:c:w:b:B:";

/* logging */
static int loglevel = LOG_WARNING;
static int dryrun;

/* signal handler */
static int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_suffix = "/logic";
static const char *mqtt_setsuffix = "/setlogic";
static const char *mqtt_onchangesuffix = "/onchange";
static const char *mqtt_btns_suffix = "/button";
static const char *mqtt_btnl_suffix = "/longbutton";
static const char *mqtt_write_suffix = "/set";
static const char *mqtt_flags_suffix = "/logicflags";
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;
static double long_btn_delay = 1.0;

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;
	struct item *prev;

	char *topic;
	char *writetopic;
	char *lastvalue;
	/* keep track of /set items of which the
	 * remote handler is not yet ready
	 */
	int recvd;

	struct rpn *logic;
	int logicflags;
	int rpnflags;
		#define RPNFL_VERBOSE	(1 << 0)
		#define RPNFL_SILENT	(1 << 1)
	struct rpn *onchange;
	char *logic_payload;
	char *onchange_payload;
	int btnvalue;
	struct rpn *btns;
	struct rpn *btnl;
	char *btns_payload;
	char *btnl_payload;

	/* cache topic misses during startup */
	char *missingtopic;
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

static struct item *curritem;
static struct topic *currtopic;

static int mqtt_ready;

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

/* log to mqtt */
static const char *mqtt_log_levels[] = {
	[LOG_EMERG] = "log/" NAME "/emerg",
	[LOG_ALERT] = "log/" NAME "/alert",
	[LOG_CRIT] = "log/" NAME "/crit",
	[LOG_ERR] = "log/" NAME "/err",
	[LOG_WARNING] = "log/" NAME "/warn",
	[LOG_NOTICE] = "log/" NAME "/notice",
	[LOG_INFO] = "log/" NAME "/info",
	[LOG_DEBUG] = "log/" NAME "/debug",
};
static void mqttloghook(int level, const char *payload)
{
	if (!(level & LOG_MQTT))
		return;

	int ret;
	int purelevel = level & LOG_PRIMASK;

	ret = mosquitto_publish(mosq, NULL, mqtt_log_levels[purelevel], strlen(payload), payload, mqtt_qos, 0);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", mqtt_log_levels[purelevel], mosquitto_strerror(ret));
}

/* mqtt cache */
static int rpn_has_ref(struct rpn *rpn, const char *topic);
static void on_btn_long(void *dat);

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
	return lastrpntopic && lastrpntopic->isnew;
}

const char *rpn_lookup_env(const char *name, struct rpn *rpn)
{
	struct topic *topic;

	topic = get_topic(name, 0);
	lastrpntopic = topic;
	if (!topic) {
		if (strcmp(curritem->missingtopic ?: "", name)) {
			/* new missing topic found */
			if (mqtt_ready)
				mylog(LOG_INFO | LOG_MQTT, "%s: %s not found", curritem->topic, name);
			if (curritem->missingtopic)
				free(curritem->missingtopic);
			curritem->missingtopic = strdup(name);
		}
		return NULL;

	}
	if (!strcmp(curritem->missingtopic ?: "", name)) {
		free(curritem->missingtopic);
		curritem->missingtopic = NULL;
	}
	return topic->value;
}

int rpn_write_env(const char *value, const char *name, struct rpn *rpn)
{
	int ret;

	mylog(LOG_NOTICE, "mosquitto_publish %s%c%s (in %s)", name, rpn->cookie ? '=' : '>', value,
			(curritem && curritem->topic) ? curritem->topic : "?");
	if (dryrun)
		return 0;
	ret = mosquitto_publish(mosq, NULL, name, strlen(value), value, mqtt_qos, rpn->cookie);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", name, mosquitto_strerror(ret));
	return ret;
}

/* replace all relative topic references to absolute */
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
	libt_remove_timeout(on_btn_long, it);
	if (it->logic || it->onchange || it->btns || it->btnl)
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
	myfree(it->missingtopic);
	free(it);
}

static void do_logic(struct item *it, struct topic *trigger)
{
	int ret;
	const char *result;
	int loglevel = LOG_NOTICE;

	if (it->rpnflags & RPNFL_VERBOSE)
		loglevel = LOG_NOTICE;
	else if (it->rpnflags & RPNFL_SILENT)
		loglevel = LOG_DEBUG;
	else if ((!trigger && (it->logicflags & RPNFN_PERIODIC))
		|| !(it->logicflags & RPNFN_LOGIC))
		loglevel = LOG_INFO;

	curritem = it;
	lastrpntopic = NULL;
	rpn_stack_reset(&rpnstack);
	if (trigger)
		trigger->isnew = 1;
	ret = rpn_run(&rpnstack, it->logic);
	if (trigger)
		trigger->isnew = 0;
	curritem = NULL;
	if (ret < 0)
		/* TODO: alert */
		return;
	if (!rpnstack.n) {
		/* no value, so clear our state */
		if (it->lastvalue) {
			free(it->lastvalue);
			mylog(loglevel, "%s: no value from logic", it->topic);
		}
		it->lastvalue = NULL;
		return;
	}

	struct rpn_el *el = rpnstack.v+rpnstack.n-1;
	result = el->a ?: mydtostr(el->d);
	/* test if we found something new */
	if (it->lastvalue && !strcmp(it->lastvalue, result))
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

	mylog(loglevel, "mosquitto_publish %s%c%s", it->writetopic ?: it->topic, it->writetopic ? '>' : '=', result);
	if (dryrun)
		goto save_cache;
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

static void do_event_rpn(struct item *it, struct rpn *rpn)
{
	if (!rpn)
		return;
	curritem = it;
	lastrpntopic = NULL;
	rpn_stack_reset(&rpnstack);
	rpn_run(&rpnstack, rpn);
	curritem = NULL;
}

void rpn_run_again(void *dat)
{
	struct item *it = ((struct rpn *)dat)->dat;

	if (rpn_referred(it->logic, dat))
		do_logic(it, NULL);
	else if (rpn_referred(it->onchange, dat))
		do_event_rpn(it, it->onchange);
	else if (rpn_referred(it->btns, dat))
		do_event_rpn(it, it->btns);
	else if (rpn_referred(it->btnl, dat))
		do_event_rpn(it, it->btnl);
}

static void on_btn_long(void *dat)
{
	struct item *it = dat;

	mylog(LOG_INFO, "%s/button: long", it->topic);
	do_event_rpn(it, it->btnl);
	/* fake reverted value */
	it->btnvalue = 0;
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	struct item *it;
	struct topic *topic;
	int ret;

	if (is_self_sync(msg)) {
		mqtt_ready = 1;
		for (it = items; it; it = it->next) {
			if (it->missingtopic)
				mylog(LOG_INFO | LOG_MQTT, "%s: %s not found", it->topic, it->missingtopic);
		}
	}

	if (!strcmp(msg->topic, "tools/loglevel")) {
		mysetloglevelstr(msg->payload);
	} else if (test_suffix(msg->topic, mqtt_suffix)) {
		/* this is a logic set msg */
		it = get_item(msg->topic, mqtt_suffix, msg->payloadlen);
		if (!it || !msg->payloadlen) {
			if (it) {
				myfree(it->logic_payload);
				drop_item(it, &it->logic);
			}
			return;
		}
		if (it->writetopic) {
			free(it->writetopic);
			it->writetopic = NULL;
		}
		if (!strcmp(it->logic_payload ?: "", msg->payload ?: "")) {
			mylog(LOG_DEBUG, "identical logic for %s", it->topic);
			return;
		}
		/* remove old logic */
		rpn_unref(it->logic);
		rpn_free_chain(it->logic);
		/* prepare new info */
		it->logic = rpn_parse(msg->payload, it);
		it->logicflags = rpn_collect_flags(it->logic);
		rpn_resolve_relative(it->logic, it->topic);
		rpn_ref(it->logic);
		myfree(it->logic_payload);
		it->logic_payload = strdup(msg->payload);
		mylog(LOG_INFO, "new logic for %s", it->topic);
		/* ready, first run */
		do_logic(it, NULL);
		return;
	} else if (test_suffix(msg->topic, mqtt_setsuffix)) {
		/* this is a logic set msg */
		it = get_item(msg->topic, mqtt_setsuffix, msg->payloadlen);
		if (!it || !msg->payloadlen) {
			if (it) {
				myfree(it->logic_payload);
				drop_item(it, &it->logic);
			}
			return;
		}
		if (!it->writetopic)
			asprintf(&it->writetopic, "%s%s", it->topic, mqtt_write_suffix);
		if (!strcmp(it->logic_payload ?: "", msg->payload ?: "")) {
			mylog(LOG_DEBUG, "identical logic for %s", it->topic);
			return;
		}
		/* remove old logic */
		rpn_unref(it->logic);
		rpn_free_chain(it->logic);
		/* prepare new info */
		it->logic = rpn_parse(msg->payload, it);
		it->logicflags = rpn_collect_flags(it->logic);
		rpn_resolve_relative(it->logic, it->topic);
		rpn_ref(it->logic);
		myfree(it->logic_payload);
		it->logic_payload = strdup(msg->payload);
		mylog(LOG_INFO, "new setlogic for %s", it->topic);
		/* ready, first run */
		do_logic(it, NULL);
		return;
	} else if (test_suffix(msg->topic, mqtt_onchangesuffix)) {
		it = get_item(msg->topic, mqtt_onchangesuffix, msg->payloadlen);
		if (!it || !msg->payloadlen) {
			if (it) {
				myfree(it->onchange_payload);
				drop_item(it, &it->onchange);
			}
			return;
		}
		if (!strcmp(it->onchange_payload ?: "", msg->payload ?: "")) {
			mylog(LOG_DEBUG, "identical onchange for %s", it->topic);
			return;
		}
		/* remove old logic */
		rpn_unref(it->onchange);
		rpn_free_chain(it->onchange);
		/* prepare new info */
		it->onchange = rpn_parse(msg->payload, it);
		rpn_resolve_relative(it->onchange, it->topic);
		rpn_ref(it->onchange);
		myfree(it->onchange_payload);
		it->onchange_payload = strdup(msg->payload);
		mylog(LOG_INFO, "new onchange for %s", it->topic);
		return;
	} else if (test_suffix(msg->topic, mqtt_btns_suffix)) {
		it = get_item(msg->topic, mqtt_btns_suffix, msg->payloadlen);
		if (!it || !msg->payloadlen) {
			if (it) {
				myfree(it->btns_payload);
				drop_item(it, &it->btns);
			}
			return;
		}
		if (!strcmp(it->btns_payload ?: "", msg->payload ?: "")) {
			mylog(LOG_DEBUG, "identical %s for %s", mqtt_btns_suffix, it->topic);
			return;
		}
		/* remove old logic */
		rpn_unref(it->btns);
		rpn_free_chain(it->btns);
		/* prepare new info */
		it->btns = rpn_parse(msg->payload, it);
		rpn_resolve_relative(it->btns, it->topic);
		rpn_ref(it->btns);
		myfree(it->btns_payload);
		it->btns_payload = strdup(msg->payload);
		mylog(LOG_INFO, "new %s for %s", mqtt_btns_suffix, it->topic);
		return;
	} else if (test_suffix(msg->topic, mqtt_btnl_suffix)) {
		it = get_item(msg->topic, mqtt_btnl_suffix, msg->payloadlen);
		if (!it || !msg->payloadlen) {
			if (it) {
				myfree(it->btnl_payload);
				drop_item(it, &it->btnl);
			}
			return;
		}
		if (!strcmp(it->btnl_payload ?: "", msg->payload ?: "")) {
			mylog(LOG_DEBUG, "identical %s for %s", mqtt_btnl_suffix, it->topic);
			return;
		}
		/* remove old logic */
		rpn_unref(it->btnl);
		rpn_free_chain(it->btnl);
		/* prepare new info */
		it->btnl = rpn_parse(msg->payload, it);
		rpn_resolve_relative(it->btnl, it->topic);
		rpn_ref(it->btnl);
		myfree(it->btnl_payload);
		it->btnl_payload = strdup(msg->payload);
		mylog(LOG_INFO, "new %s for %s", mqtt_btnl_suffix, it->topic);
		return;
	} else if (test_suffix(msg->topic, mqtt_flags_suffix)) {
		it = get_item(msg->topic, mqtt_flags_suffix, 0);
		if (!it)
			;
		else if (strchr(msg->payload, 'l'))
			it->rpnflags |= RPNFL_VERBOSE;
		else if (strchr(msg->payload, 'L'))
			it->rpnflags |= RPNFL_SILENT;
		return;
	}
	/* find topic */
	topic = get_topic(msg->topic, msg->payloadlen);
	if (topic) {
		free(topic->value);
		topic->value = strndup(msg->payload ?: "", msg->payloadlen);
		currtopic = topic;
		if (topic->ref) {
			for (it = items; it; it = it->next) {
				if (rpn_has_ref(it->logic, msg->topic))
					do_logic(it, topic);
			}
		}
		currtopic = NULL;
	}
	/* run onchange logic */
	it = get_item(msg->topic, "", 0);
	if (it) {
		if (!msg->retain && it->onchange)
			do_event_rpn(it, it->onchange);
		if (it->btns || it->btnl) {
			int curr = strtoul((char *)msg->payload ?: "", NULL, 0);

			if (curr && !it->btnvalue) {
				/* rising edge */
				if (it->btns && !it->btnl) {
					/* immediate delivery */
					mylog(LOG_INFO, "%s/button: immediate delivery", it->topic);
					do_event_rpn(it, it->btns);
				} else if (it->btnl) {
					mylog(LOG_INFO, "%s/button: measure ...", it->topic);
					/* wait for long-btn timeout */
					libt_add_timeout(long_btn_delay, on_btn_long, it);
				}
			} else if (!curr && it->btnvalue) {
				if (it->btnl) {
					/* falling edge, fire short button, only if we were waiting for long button */
					mylog(LOG_INFO, "%s/button: short", it->topic);
					libt_remove_timeout(on_btn_long, it);
					do_event_rpn(it, it->btns);
				}
			}
			it->btnvalue = curr;
		}
		if (it->writetopic && it->lastvalue && !it->recvd) {
			/* This is the first time we recv the main topic
			 * of which we wrote /set already
			 * The program handling this topic most probable has missed
			 * our /set request, so we repeat it here.
			 */
			mylog(LOG_NOTICE, "repeat %s>%s", it->writetopic, it->lastvalue);
			if (!dryrun) {
				ret = mosquitto_publish(mosq, NULL, it->writetopic, strlen(it->lastvalue), it->lastvalue, mqtt_qos, 0);
				if (ret < 0) {
					mylog(LOG_ERR, "mosquitto_publish %s: %s", it->writetopic, mosquitto_strerror(ret));
					return;
				}
			}
		}
		if (!msg->retain)
			/* remote end is present */
			it->recvd = 1;
	}
}

static void mqtt_maintenance(void *dat)
{
	int ret;
	struct mosquitto *mosq = dat;

	ret = mosquitto_loop_misc(mosq);
	if (ret)
		mylog(LOG_ERR, "mosquitto_loop_misc: %s", mosquitto_strerror(ret));
	libt_add_timeout(2.3, mqtt_maintenance, dat);
}

static void recvd_mosq(int fd, void *dat)
{
	struct mosquitto *mosq = dat;
	int evs = libe_fd_evs(fd);
	int ret;

	if (evs & LIBE_RD) {
		/* mqtt read ... */
		ret = mosquitto_loop_read(mosq, 1);
		if (ret)
			mylog(LOG_ERR, "mosquitto_loop_read: %s", mosquitto_strerror(ret));
	}
	if (evs & LIBE_WR) {
		/* flush mqtt write queue _after_ the timers have run */
		ret = mosquitto_loop_write(mosq, 1);
		if (ret)
			mylog(LOG_ERR, "mosquitto_loop_write: %s", mosquitto_strerror(ret));
	}
}

void mosq_update_flags(void)
{
	if (mosq)
		libe_mod_fd(mosquitto_socket(mosq), LIBE_RD | (mosquitto_want_write(mosq) ? LIBE_WR : 0));
}

static void timechanged(int fd, void *dat)
{
	struct item *it;

	if (libtimechange_iterate(fd) < 0) {
		if (errno == ECANCELED) {
			mylog(LOG_NOTICE, "wall-time changed");
			for (it = items; it; it = it->next) {
				if (it->logicflags & RPNFN_WALLTIME)
					do_logic(it, NULL);
			}
		}
	}
	if (libtimechange_arm(fd) < 0)
		mylog(LOG_ERR, "timerfd rearm: %s", ESTR(errno));
}

__attribute__((unused))
static void signalrecvd(int fd, void *dat)
{
	int ret;
	struct signalfd_siginfo sfdi;

	for (;;) {
		ret = read(fd, &sfdi, sizeof(sfdi));
		if (ret < 0 && errno == EAGAIN)
			break;
		if (ret < 0)
			mylog(LOG_ERR, "read signalfd: %s", ESTR(errno));
		switch (sfdi.ssi_signo) {
		case SIGTERM:
		case SIGINT:
			sigterm = 1;
			break;
		}
	}
}

int main(int argc, char *argv[])
{
	int opt, ret;
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
	case 'n':
		dryrun = 1;
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
	case 'b':
		mqtt_btns_suffix = optarg;
		break;
	case 'B':
		mqtt_btnl_suffix = optarg;
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

	libt_add_timeout(0, mqtt_maintenance, mosq);
	libe_add_fd(mosquitto_socket(mosq), recvd_mosq, mosq);

	/* prepare signalfd (turn off for debugging) */
#if 1
	sigset_t sigmask;
	int sigfd;

	sigfillset(&sigmask);

	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0)
		mylog(LOG_ERR, "sigprocmask: %s", ESTR(errno));
	sigfd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sigfd < 0)
		mylog(LOG_ERR, "signalfd failed: %s", ESTR(errno));
	libe_add_fd(sigfd, signalrecvd, NULL);
#endif

	/* listen to wall-time changes */
	int tcfd = libtimechange_makefd();
	if (tcfd < 0)
		mylog(LOG_ERR, "timerfd: %s", ESTR(errno));
	if (libtimechange_arm(tcfd) < 0)
		mylog(LOG_ERR, "timerfd rearm: %s", ESTR(errno));
	libe_add_fd(tcfd, timechanged, NULL);

	/* initiate a loopback to know if we got all retained topics */
	send_self_sync(mosq, mqtt_qos);

	/* catch LOG_MQTT-marked logs via MQTT */
	mylogsethook(mqttloghook);

	if (dryrun)
		mylog(LOG_NOTICE, "dry run, not touching anything");

	/* core loop */
	for (; !sigterm; ) {
		libt_flush();
		mosq_update_flags();
		ret = libe_wait(libt_get_waittime());
		if (ret >= 0)
			libe_flush();
	}
	/* cleanup */
	mosquitto_disconnect(mosq);
	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();
	return 0;
}
