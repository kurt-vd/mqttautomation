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

#define NAME "mqttpoort"
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
	NAME ": Control poort by 1 button + 1 'closed' input\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=STR	Give MQTT topic suffix for configuration (default '/poortcfg')\n"
	" -S, --nosuffix	Write control topic without suffix\n"
	" -k, --homekit=SUFFIX[,WRSUFFIX]	Report/accept 'homekit' status to this suffix\n"
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
	{ "nosuffix", no_argument, NULL, 'S', },
	{ "homekit", required_argument, NULL, 'k', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:s:Sk:";

/* logging */
static int loglevel = LOG_WARNING;

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_suffix = "/poortcfg";
static const char *const mqtt_write_suffix = "/set";
static int no_mqtt_ctl_suffix;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;
static const char *mqtt_homekit_suffix;
static const char *mqtt_homekit_wrsuffix;

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;
	struct item *prev;

	char *topic;
	int topiclen;
	char *writetopic;
	char *dirtopic;
	/* ctltopic: topic to control the poort */
	char *ctltopic;
	char *ctlwrtopic;
	int ctltype;
#define PUSHBUTTON	0 /* push 1..0 to start/stop operation */
#define MOTOR		1 /* push -1, 0 or +1 to a (H-bridge) motor */
	/* statetopic: topic that reads back the poort */
	char *statetopic;
	/* homekittopic: topic to publish homekit state */
	char *homekittopic;
	char *homekitwrtopic;
	/* min time between pulses, default 0.5 */
	double idletime;
	/* scale, # seconds to fully open/close */
	double openmaxtime, closemaxtime;
	/* # seconds initial delay */
	double openstarttime, closestarttime;
	/* # seconds margin at eol */
	double eoltime;
	/* requested value
	 * When NAN, the logic will not try to reach the target position
	 */
	double reqval;
	/* current value */
	double currval;
	double currvaltime;
	/* the value of the ctr line, with potential feedback */
	int ctlval;
	int currctlval;
	int mustwait;
	/* value of the state line */
	int stateval;
	/* # retries passed */
	int nretry;
	int state;
		#define ST_CLOSED	0
		#define ST_OPEN		1
		#define ST_CSTOPPED	2
		#define ST_OSTOPPED	3
		#define ST_CSTART	4
		#define ST_OSTART	5
		#define ST_CLOSING	6
		#define ST_OPENING	7
		#define ST_CMARGIN	8
		#define ST_OMARGIN	9
	int flags;
		/* special behaviour: ctrl during close will open */
		#define FL_NO_CLOSE_STOPPED	0x01
	/* cached direction of movement */
	int currdir;
};
#define posctrl(it) (!isnan(it->reqval))

static const char *const states[] = {
	[ST_CLOSED] = "closed",
	[ST_OPEN] = "open",
	[ST_CSTOPPED] = "closing-stopped",
	[ST_OSTOPPED] = "open-stopped",
	[ST_CSTART] = "close-starting",
	[ST_OSTART] = "open-starting",
	[ST_CLOSING] = "closing",
	[ST_OPENING] = "opening",
	[ST_CMARGIN] = "closing-eol",
	[ST_OMARGIN] = "opening-eol",
};

static const int statedirs[] = {
	[ST_CSTART] = -1,
	[ST_OSTART] = 1,
	[ST_CLOSING] = -1,
	[ST_OPENING] = 1,
	[ST_CMARGIN] = -1,
	[ST_OMARGIN] = 1,
};

struct item *items;

/* MQTT iface */
static void my_mqtt_log(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
	static const int logpri_map[] = {
		MOSQ_LOG_ERR, LOG_ERR,
		MOSQ_LOG_WARNING, LOG_WARNING,
		MOSQ_LOG_NOTICE, LOG_NOTICE,
		MOSQ_LOG_INFO, LOG_DEBUG,
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
	it->reqval = it->currval = NAN;
	it->currctlval = -10; /* mark invalid */
	it->topic = strndup(topic, len);
	it->topiclen = len;
	it->closemaxtime = it->openmaxtime = it->openstarttime = it->closestarttime = it->eoltime = 0;
	it->idletime = 0.5;
	if (mqtt_write_suffix)
		asprintf(&it->writetopic, "%s%s", it->topic, mqtt_write_suffix);
	asprintf(&it->dirtopic, "%s/dir", it->topic);
	if (mqtt_homekit_suffix)
		asprintf(&it->homekittopic, "%s%s", it->topic, mqtt_homekit_suffix);
	if (mqtt_homekit_wrsuffix)
		asprintf(&it->homekitwrtopic, "%s%s", it->topic, mqtt_homekit_wrsuffix);

	/* subscribe */
	ret = mosquitto_subscribe(mosq, NULL, it->writetopic ?: it->topic, mqtt_qos);
	if (ret)
		mylog(LOG_ERR, "mosquitto_subscribe '%s': %s", it->writetopic ?: it->topic, mosquitto_strerror(ret));
	if (it->homekitwrtopic) {
		ret = mosquitto_subscribe(mosq, NULL, it->homekitwrtopic, mqtt_qos);
		if (ret)
			mylog(LOG_ERR, "mosquitto_subscribe '%s': %s", it->homekitwrtopic, mosquitto_strerror(ret));
	}

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

static void on_poort_moved(void *dat);
static void idle_ctl(void *dat);
static void set_ctl(struct item *it, int dir);
static void reset_ctl(void *dat);
static void on_ctl_set_timeout(void *dat);

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
	if (it->homekitwrtopic) {
		ret = mosquitto_unsubscribe(mosq, NULL, it->homekitwrtopic);
		if (ret)
			mylog(LOG_ERR, "mosquitto_unsubscribe '%s': %s", it->homekitwrtopic, mosquitto_strerror(ret));
	}

	/* free memory */
	free(it->topic);
	myfree(it->writetopic);
	myfree(it->dirtopic);
	free(it->ctltopic);
	myfree(it->ctlwrtopic);
	free(it->statetopic);
	myfree(it->homekittopic);
	myfree(it->homekitwrtopic);
	free(it);
	/* free timers */
	libt_remove_timeout(reset_ctl, it);
	libt_remove_timeout(idle_ctl, it);
	libt_remove_timeout(on_poort_moved, it);
	libt_remove_timeout(on_ctl_set_timeout, it);
}

static void poort_publish(struct item *it)
{
	const char *result;
	int ret;

	if (it->state == ST_OPEN || it->currval > 1)
		result = "1";
	else if (it->state == ST_CLOSED || it->currval < 0)
		result = "0";
	else
		result = mydtostr(it->currval);

	ret = mosquitto_publish(mosq, NULL, it->topic, strlen(result), result, mqtt_qos, 1);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
}

static void poort_publish_dir(struct item *it)
{
	int ret;
	char buf[16];

	static const char *const dirstrs[] = {
		"closing",
		"idle",
		"opening",
	};

	it->currdir = statedirs[it->state];
	mylog(LOG_INFO, "poort %s: %s, %s", it->topic, dirstrs[it->currdir+1], mydtostr(it->currval));
	sprintf(buf, "%i", statedirs[it->state]);
	ret = mosquitto_publish(mosq, NULL, it->dirtopic, strlen(buf), buf, mqtt_qos, 1);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->dirtopic, mosquitto_strerror(ret));
}

static void poort_publish_homekit(struct item *it)
{
	static const char *const strs[10] = {
		[ST_CLOSED] = "closed",
		[ST_CSTART] = "closing",
		[ST_CLOSING] = "closing",
		[ST_CMARGIN] = "closing",
		[ST_CSTOPPED] = "stopped",
		[ST_OPEN] = "open",
		[ST_OSTOPPED] = "stopped",
		[ST_OSTART] = "opening",
		[ST_OPENING] = "opening",
		[ST_OMARGIN] = "opening",
	};
	int ret;
	const char *str = strs[it->state];

	if (!it->homekittopic)
		return;
	mylog(LOG_INFO, "poort %s: homekit '%s'", it->topic, str);
	ret = mosquitto_publish(mosq, NULL, it->homekittopic, strlen(str), str, mqtt_qos, 1);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->homekittopic, mosquitto_strerror(ret));
}

/* returns the travel time needed to reach reqval */
static double travel_time_needed(struct item *it)
{
	if (it->reqval < it->currval)
		return (it->reqval - it->currval)*it->closemaxtime;
	else
		return (it->reqval - it->currval)*it->openmaxtime;
}

static int direction_needed(struct item *it)
{
	double time = travel_time_needed(it);

	if (time < -0.5)
		return -1;
	else if (time > 0.5)
		return 1;
	else
		return 0;
}

static void poort_moved(struct item *it)
{
	double now = libt_now();
	double delta;

	if (it->currdir < 0)
		delta = (now - it->currvaltime)/it->closemaxtime;
	else if (it->currdir > 0)
		delta = (now - it->currvaltime)/it->openmaxtime;
	else
		delta = 0;

	switch (it->state) {
	case ST_CSTART:
		if ((now - it->currvaltime) > (it->closestarttime-0.05)) {
			it->currvaltime += it->closestarttime;
			it->state = ST_CLOSING;
			poort_publish_homekit(it);
		}
		break;
	case ST_OSTART:
		if ((now - it->currvaltime) > (it->openstarttime-0.05)) {
			it->currvaltime += it->openstarttime;
			it->state = ST_OPENING;
			poort_publish_homekit(it);
		}
		break;
	case ST_CLOSING:
		it->currvaltime = now;
		it->currval -= delta;
		if (it->currval < 0) {
			it->currval = 0;
			it->state = ST_CMARGIN;
			poort_publish_homekit(it);
		}
		poort_publish(it);
		break;
	case ST_OPENING:
		it->currvaltime = now;
		it->currval += delta;
		if (it->currval > 1) {
			it->currval = 1;
			it->state = ST_OMARGIN;
			poort_publish_homekit(it);
		}
		poort_publish(it);
		break;
	}
}

/* timer callbacks */
static void on_poort_moved(void *dat)
{
	struct item *it = dat;
	double delay;

	poort_moved(it);
	/* find next probe */
	switch (it->state) {
	case ST_CMARGIN:
		delay = it->currvaltime + it->eoltime - libt_now();
		if (delay > 0.01)
			/* postpone */
			libt_add_timeout(delay, on_poort_moved, it);
		else switch (it->ctltype) {
		case PUSHBUTTON:
			/* return to open */
			it->state = ST_OPEN;
			it->currval = 1.1;
			poort_publish(it);
			poort_publish_homekit(it);

			if (posctrl(it)) {
				if (++it->nretry > 3)
					mylog(LOG_WARNING, "poort %s keeps failing", it->topic);
				else {
					mylog(LOG_WARNING, "poort %s: closed not seen, retry ...", it->topic);
					/* retry */
					set_ctl(it, direction_needed(it));
				}
			}
			break;
		case MOTOR:
			if (it->stateval) {
				/* end-of-course reached, stop now */
				it->state = ST_CLOSED;
			} else {
				mylog(LOG_WARNING, "poort %s: closed not seen", it->topic);
				it->state = ST_CSTOPPED;
				it->currval = 1.1;
				poort_publish(it);
			}
			set_ctl(it, 0);
			poort_publish_dir(it);
			poort_publish_homekit(it);
			break;
		}
		break;
	case ST_OMARGIN:
		delay = it->currvaltime + it->eoltime - libt_now();
		if (delay > 0.01)
			/* postpone */
			libt_add_timeout(delay, on_poort_moved, it);
		else switch (it->ctltype) {
		case PUSHBUTTON:
			/* end-of-course reached
			 * set state to opened if opening,
			 * closed state should be detected by a sensor
			 */
			it->state = ST_OPEN;
			poort_publish_dir(it);
			poort_publish_homekit(it);
			if (posctrl(it) && it->reqval < 0.9)
				set_ctl(it, direction_needed(it));
			break;
		case MOTOR:
			/* end-of-course reached, stop now */
			set_ctl(it, 0);
			it->state = ST_OPEN;
			poort_publish_dir(it);
			poort_publish_homekit(it);
			break;
		}
		break;
	case ST_CSTART:
		delay = it->currvaltime + it->closestarttime - libt_now();
		libt_add_timeout(delay, on_poort_moved, it);
		break;
	case ST_OSTART:
		delay = it->currvaltime + it->openstarttime - libt_now();
		libt_add_timeout(delay, on_poort_moved, it);
		break;
	case ST_CLOSING:
	case ST_OPENING:
		delay = 0.5;

		if ((it->ctltype == PUSHBUTTON && it->ctlval) || it->mustwait || !posctrl(it))
			; /* don't make a smaller delay */
		else if (it->state == ST_CLOSING)
			delay = -travel_time_needed(it);
		else if (it->state == ST_OPENING)
			delay = travel_time_needed(it);
		/* limit delay */
		if (!posctrl(it))
			delay = 0.5;
		else if (delay < 0.05) {
			if (it->reqval > 0.1 && it->reqval < 0.9) {
				/* stop the poort! */
				set_ctl(it, 0);
				break;
			}
			delay = 0.05;
		}
		else if (delay > 0.5)
			delay = 0.5;
		libt_add_timeout(delay, on_poort_moved, it);
		break;
	}
}

static void idle_ctl(void *dat)
{
	struct item *it = dat;

	/* return to idle */
	it->mustwait = 0;
	mylog(LOG_INFO, "poort %s: idle bttn", it->topic);
	/* TODO: decide new action */
	switch (it->state) {
	case ST_CSTOPPED:
	case ST_OSTOPPED:
		/* measure difference in traveltime */
		if (posctrl(it) && fabs(travel_time_needed(it)) > (0.5+it->idletime))
			/* start moving if we have enough time to stop again */
			set_ctl(it, direction_needed(it));
		break;
	case ST_OSTART:
	case ST_OPENING:
	case ST_OMARGIN:
	case ST_OPEN:
		if (it->stateval) {
			it->state = ST_CLOSED;
			poort_publish_homekit(it);
			if (posctrl(it))
				/* retry opening */
				set_ctl(it, 1);
		}
		/* TODO: when is this triggered? */
		if (posctrl(it) && travel_time_needed(it) < -0.5)
			/* trigger again */
			set_ctl(it, -1);
		break;

	case ST_CSTART:
	case ST_CLOSING:
	case ST_CMARGIN:
	case ST_CLOSED:
		if (posctrl(it) && travel_time_needed(it) > 0.5)
			/* trigger again */
			set_ctl(it, 1);
		break;
	}

}

static void reset_ctl(void *dat)
{
	struct item *it = dat;
	int ret;

	ret = mosquitto_publish(mosq, NULL, it->ctlwrtopic ?: it->ctltopic, 1, "0", mqtt_qos, !it->ctlwrtopic);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->ctlwrtopic ?: it->ctltopic, mosquitto_strerror(ret));
	it->ctlval = 0;
	it->mustwait = 1;
	mylog(LOG_INFO, "poort %s: pushed bttn", it->topic);

	if (it->ctlwrtopic)
		libt_add_timeout(0.25, on_ctl_set_timeout, it);
	else
		libt_add_timeout(it->idletime, idle_ctl, it);
}

static void on_ctl_set(struct item *it);
static void set_ctl(struct item *it, int dir)
{
	int ret;
	char msg[8];

	switch (it->ctltype) {
	case PUSHBUTTON:
		if (it->ctlval || it->mustwait)
			return;
		mylog(LOG_INFO, "poort %s: push bttn", it->topic);
		ret = mosquitto_publish(mosq, NULL, it->ctlwrtopic ?: it->ctltopic, 1, "1", mqtt_qos, !it->ctlwrtopic);
		if (ret < 0)
			mylog(LOG_ERR, "mosquitto_publish %s: %s", it->ctlwrtopic ?: it->ctltopic, mosquitto_strerror(ret));
		it->ctlval = 1;
		break;
	case MOTOR:
		sprintf(msg, "%i", dir);
		mylog(LOG_INFO, "poort %s: set motor '%s'", it->topic, msg);
		ret = mosquitto_publish(mosq, NULL, it->ctlwrtopic ?: it->ctltopic, strlen(msg), msg, mqtt_qos, !it->ctlwrtopic);
		if (ret < 0)
			mylog(LOG_ERR, "mosquitto_publish %s: %s", it->ctlwrtopic ?: it->ctltopic, mosquitto_strerror(ret));
		it->ctlval = dir;
		break;
	default:
		return;
	}
	if (!it->ctlwrtopic)
		/* don't wait feedback */
		on_ctl_set(it);
	else
		libt_add_timeout(0.25, on_ctl_set_timeout, it);
}

static void on_ctl_set_timeout(void *dat)
{
	struct item *it = dat;

	mylog(LOG_WARNING, "poort control %s does not respond", it->topic);
	it->ctlval = it->currctlval;
}

static void on_ctl_set(struct item *it)
{
	int newstate;

	libt_remove_timeout(on_ctl_set_timeout, it);
	switch (it->ctltype) {
	case PUSHBUTTON:
		if (!it->ctlval) {
			libt_add_timeout(it->idletime, idle_ctl, it);
			return;
		}
		/* only change state on set value */
		libt_add_timeout(0.5, reset_ctl, it);

		static const int newstates[] = {
			[ST_OPEN] = ST_CSTART,
			[ST_OSTOPPED] = ST_CSTART,
			[ST_OPENING] = ST_OSTOPPED,
			[ST_OSTART] = ST_OSTOPPED,
			[ST_CLOSED] = ST_OSTART,
			[ST_CSTOPPED] = ST_OSTART,
			[ST_CLOSING] = ST_CSTOPPED, /* may be ST_OSTART */
			[ST_CSTART] = ST_CSTOPPED,
			/* it's unclear what to do in the margin */
			[ST_CMARGIN] = ST_CSTOPPED,
			[ST_OMARGIN] = ST_OSTOPPED,
		};
		newstate = newstates[it->state];

		if (it->flags & FL_NO_CLOSE_STOPPED) {
			/* special handling: btn pushed during close opens the port */
			if (newstate == ST_CSTOPPED)
				newstate = ST_OSTART;
		}
		break;
	case MOTOR:
		if (it->currdir == it->ctlval)
			/* nothing new */
			return;
		if (it->ctlval < 0)
			newstate = ST_CSTART;
		else if (it->ctlval > 0)
			newstate = ST_OSTART;
		else switch (it->state) {
		case ST_OSTART:
		case ST_OPENING:
			newstate = ST_OSTOPPED;
			break;
		case ST_CSTART:
		case ST_CLOSING:
			newstate = ST_CSTOPPED;
			break;
		case ST_OMARGIN:
			newstate = ST_OPEN;
			break;
		case ST_CMARGIN:
			newstate = ST_CLOSED;
			break;
		default:
			/* no change */
			newstate = it->state;
			break;
		}
		break;
	}

	/* stop counting movement */
	poort_moved(it);
	libt_remove_timeout(on_poort_moved, it);
	/* update now, after poort_moved (et al.?) have finished
	 * using the previous state
	 */
	it->state = newstate;
	poort_publish_dir(it);
	poort_publish_homekit(it);
	/* start counting movement */
	it->currvaltime = libt_now();
	on_poort_moved(it);
}

static void stop(struct item *it)
{
	if (!statedirs[it->state])
		return;
	it->nretry = 0;
	/* disable position control */
	it->reqval = NAN;
	set_ctl(it, 0);
}

static void setvalue(struct item *it, double newvalue)
{
	/* stick to the end-of-course */
	if (newvalue < 0.1)
		newvalue = 0;
	else if (newvalue > 0.9)
		newvalue = 1;

	if (isnan(it->currval)) {
		/* escape from init state, fake an initial position
		 * opposite to the requested one.
		 */
		if (newvalue > 0.9) {
			it->currval = 0.01;
			it->state = ST_CSTOPPED;
			poort_publish(it);
		} else if (newvalue < 0.1) {
			it->currval = 1;
			it->state = ST_OPEN;
			poort_publish(it);
		}
	}
	it->reqval = newvalue;
	if (fabs(it->reqval - it->currval) < 0.01)
		return;
	mylog(LOG_INFO, "poort %s: set %s", it->topic, mydtostr(newvalue));
	it->nretry = 0;

	if (it->state == ST_CMARGIN || it->state == ST_OMARGIN ||
			(it->currval < 0.05 && it->state == ST_CLOSING) ||
			(it->currval > 0.95 && it->state == ST_OPENING))
		/* don't disturb near the end of course */
		/* todo: disturb when closing */
		return;
	if (it->reqval < it->currval && statedirs[it->state] < 0)
		return;
	else if (it->reqval > it->currval && statedirs[it->state] > 0)
		return;
	/* start modifying poort state */
	set_ctl(it, direction_needed(it));
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
			mylog(LOG_INFO, "removed poort spec for %s", it->topic);
			drop_item(it);
			return;
		}
		/* reconfigure in */
		state = resolve_relative_path(state, it->topic) ?: strdup(state);
		if (!it->statetopic || strcmp(it->statetopic, state)) {
			if (it->statetopic) {
				ret = mosquitto_unsubscribe(mosq, NULL, it->statetopic);
				if (ret)
					mylog(LOG_ERR, "mosquitto_unsubscribe '%s': %s", it->statetopic, mosquitto_strerror(ret));
				free(it->statetopic);
			}
			it->statetopic = state;
			ret = mosquitto_subscribe(mosq, NULL, it->statetopic, mqtt_qos);
			if (ret)
				mylog(LOG_ERR, "mosquitto_subscribe '%s': %s", it->statetopic, mosquitto_strerror(ret));
			it->currval = NAN;
		} else
			free(state);
		/* reconfigure out */
		ctl = resolve_relative_path(ctl, it->topic) ?: strdup(ctl);
		if (!it->ctltopic || strcmp(it->ctltopic, ctl)) {
			if (it->ctltopic && mqtt_write_suffix && !no_mqtt_ctl_suffix) {
				ret = mosquitto_unsubscribe(mosq, NULL, it->ctltopic);
				if (ret)
					mylog(LOG_ERR, "mosquitto_unsubscribe '%s': %s", it->ctltopic, mosquitto_strerror(ret));
			}
			myfree(it->ctltopic);
			it->ctltopic = ctl;
			if (mqtt_write_suffix && !no_mqtt_ctl_suffix) {
				free(it->ctlwrtopic);
				asprintf(&it->ctlwrtopic, "%s%s", it->ctltopic, mqtt_write_suffix);
				ret = mosquitto_subscribe(mosq, NULL, it->ctltopic, mqtt_qos);
				if (ret)
					mylog(LOG_ERR, "mosquitto_subscribe '%s': %s", it->ctltopic, mosquitto_strerror(ret));
			}
			it->ctlval = 0;
			it->mustwait = 0;
			it->nretry = 0;
			libt_remove_timeout(reset_ctl, it);
			libt_remove_timeout(idle_ctl, it);
			libt_remove_timeout(on_poort_moved, it);
			libt_remove_timeout(on_ctl_set_timeout, it);
		} else
			free(ctl);
		/* preset other fields */
		it->ctltype = 0;
		it->openmaxtime = 0;
		it->closemaxtime = 0;
		it->openstarttime = 0;
		it->closestarttime = 0;
		it->eoltime = 0;
		it->idletime = 0;
		it->flags &= ~FL_NO_CLOSE_STOPPED;
		/* parse remaining tokens */
		it->flags = 0;
		char *tok, *value;
		for (tok = strtok(NULL, " \t"); tok; tok = strtok(NULL, " \t")) {
			value = strchr(tok, '=');
			if (value)
				/* null terminate */
				*value++ = 0;
			if (!strcmp(tok, "opentime"))
				it->openmaxtime = strtod(value, NULL);
			else if (!strcmp(tok, "closetime"))
				it->closemaxtime = strtod(value, NULL);
			else if (!strcmp(tok, "openstarttime"))
				it->openstarttime = strtod(value, NULL);
			else if (!strcmp(tok, "closestarttime"))
				it->closestarttime = strtod(value, NULL);
			else if (!strcmp(tok, "eoltime"))
				it->eoltime = strtod(value, NULL);
			else if (!strcmp(tok, "idletime"))
				it->idletime = strtod(value, NULL);
			else if (!strcmp(tok, "noclosestop"))
				it->flags |= FL_NO_CLOSE_STOPPED;
			else if (!strcmp(tok, "type")) {
				char *endp;

				it->ctltype = strtoul(value, &endp, 0);
				if (endp > value)
					;
				else if (!strcmp(value, "pushbutton"))
					it->ctltype = PUSHBUTTON;
				else if (!strcmp(value, "motor"))
					it->ctltype = MOTOR;
				else {
					mylog(LOG_INFO, "unknow type '%s' for poort %s", value, it->topic);
					drop_item(it);
					return;
				}
			}

		}
		if (!isnan(it->reqval))
			/* repeat last value */
			setvalue(it, it->reqval);
		/* finalize */
		mylog(LOG_INFO, "new poort spec for %s", it->topic);

	} else if (!msg->retain && (it = get_item(msg->topic, mqtt_write_suffix, 0)) != NULL) {
		/* this is the write topic */
		if (!msg->payloadlen)
			stop(it);
		else if (!msg->retain)
			setvalue(it, strtod(msg->payload ?: "0", NULL));

	} else if (!msg->retain && (it = get_item(msg->topic, mqtt_homekit_wrsuffix, 0)) != NULL) {
		/* this is the write topic via homekit */
		if (!msg->payloadlen)
			stop(it);
		else if (!strcmp(msg->payload, "open"))
			setvalue(it, 1);
		else if (!strcmp(msg->payload, "closed"))
			setvalue(it, 0);

	} else if ((!mqtt_write_suffix || msg->retain) &&
			(it = get_item(msg->topic, NULL, 0)) != NULL) {
		/* this is the main led topic */
		setvalue(it, strtod(msg->payload ?: "0", NULL));

	} else if ((it = get_item_by_state(msg->topic)) != NULL) {
		it->stateval = strtoul(msg->payload ?: "0", NULL, 0);

		if (it->stateval) {
			if (it->state == ST_CLOSING) {
				poort_moved(it);
				mylog(LOG_INFO, "poort %s: closing %.2lf, closed detected", it->topic, it->currval);
			} else if (it->state == ST_CMARGIN) {
				mylog(LOG_INFO, "poort %s: closing margin %.1lfs, closed detected", it->topic, libt_now() - it->currvaltime);
			} else {
				/* disable position control */
				it->reqval = NAN;
				mylog(LOG_INFO, "poort %s: closed detected", it->topic);
			}
			switch (it->ctltype) {
			case PUSHBUTTON:
				it->currval = 0;
				it->state = ST_CLOSED;
				libt_remove_timeout(on_poort_moved, it);
				poort_publish(it);
				poort_publish_dir(it);
				poort_publish_homekit(it);
				if (posctrl(it) && it->reqval > 0.1)
					/* open the poort now */
					set_ctl(it, direction_needed(it));
				break;
			case MOTOR:
				if (msg->retain) {
					it->currval = 0;
					it->state = ST_CLOSED;
					libt_remove_timeout(on_poort_moved, it);
				} else {
					it->currval = 0;
					it->state = ST_CMARGIN;
					it->currvaltime = libt_now();
					libt_add_timeout(it->eoltime, on_poort_moved, it);
				}
				poort_publish(it);
				poort_publish_dir(it);
				poort_publish_homekit(it);
				break;
			}

		} else if (it->state == ST_CLOSED) {
			if (msg->retain) {
				mylog(LOG_WARNING, "poort %s is not closed", it->topic);
				it->state = ST_OSTOPPED;
				poort_publish_homekit(it);
			} else if (it->ctltype == PUSHBUTTON) {
				mylog(LOG_WARNING, "poort %s opened unexpectedly", it->topic);
				/* change to direction ctl */
				it->reqval = NAN;
				it->state = ST_OSTART;
				poort_publish_dir(it);
				poort_publish_homekit(it);
				it->currvaltime = libt_now();
				libt_add_timeout(0.5, on_poort_moved, it);
			} else {
				mylog(LOG_WARNING, "poort %s opened unexpectedly", it->topic);
			}
		}

	} else if ((it = get_item_by_ctl(msg->topic)) != NULL) {
		it->currctlval = strtol(msg->payload ?: "0", NULL, 0);
		if (it->currctlval != it->ctlval) {
			/* unexpected edge */
			if (!msg->retain)
				mylog(LOG_INFO, "poort %s: direct controlled", it->topic);
			if (it->currctlval || it->ctltype != PUSHBUTTON)
				/* disable position control */
				it->reqval = NAN;
			it->ctlval = it->currctlval;
		}
		/* call on_ctl_set, also on expected change */
		on_ctl_set(it);
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
		no_mqtt_ctl_suffix = 1;
		break;
	case 'k':
		mqtt_homekit_suffix = optarg;
		str = strchr(mqtt_homekit_suffix, ',');
		if (str) {
			*str++ = 0;
			mqtt_homekit_wrsuffix = str;
		}
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
