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
#include <poll.h>
#include <syslog.h>
#include <sys/stat.h>
#include <mosquitto.h>
#include <linux/input.h>

#include "common.h"

/* overrule the toolchain provided struct input_event.
 * gcc messed a bit on arm eabi
 */
struct local_input_event {
	unsigned long tv_sec;
	unsigned long tv_usec;
	uint16_t type;
	uint16_t code;
	int32_t value;
};
#define NAME "mqttinputevent"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* program options */
static const char help_msg[] =
	NAME ": publish input events into MQTT\n"
	"usage:	" NAME " -d DEVICE [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=STR	Give MQTT topic suffix for spec (default '/inputhw')\n"
	" -d, --device=DEVICE	Process input events from DEVICE\n"
	" -p, --prefix=PREFIX	MQTT path to publish events\n"
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
	{ "device", required_argument, NULL, 'd', },
	{ "prefix", required_argument, NULL, 'p', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:s:d:p:";

/* logging */
static int loglevel = LOG_WARNING;

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_suffix = "/inputhw";
static int mqtt_suffixlen = 8;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;
static char *mqtt_prefix = "io/input";
static const char mqtt_unknown_topic[] = "unhandled/inputevent";

static char *inputdev;
static int infd;

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;
	struct item *prev;

	char *topic;
	int topiclen;
	int evtype;
	int evcode;
	int asbutton;
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
	for (it = items; it; it = it->next)
		if ((it->topiclen == len) && !strncmp(it->topic ?: "", topic, len))
			return it;
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

static void drop_item(struct item *it, int pubnull)
{
	/* remove from list */
	if (it->prev)
		it->prev->next = it->next;
	if (it->next)
		it->next->prev = it->prev;
	/* clean mqtt topic */
	if (pubnull)
		mosquitto_publish(mosq, NULL, it->topic, 0, NULL, 0, 1);
	/* free memory */
	free(it->topic);
	free(it);
}

static void pubitem(struct item *it, const char *payload)
{
	int ret;

	if (it->asbutton && strcmp(payload, "1"))
		return;
	/* publish, volatile for buttons, retained for the rest */
	ret = mosquitto_publish(mosq, NULL, it->topic, strlen(payload), payload, mqtt_qos, !it->asbutton);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
}

#define getbit(x, vec)	((vec[(x)/8] >> ((x) % 8)) & 1)
static void pubinitial(struct item *it)
{
	static uint8_t state[KEY_CNT/8+1];
	static char buf[32];

	/* load supported events */
	if (ioctl(infd, EVIOCGBIT(it->evtype, sizeof(state)), state) < 0)
		mylog(LOG_ERR, "ioctl %s EVIOCGBIT(KEY): %s", inputdev, ESTR(errno));
	if (!getbit(it->evcode, state)) {
		/* remove item from this input device */
		mylog(LOG_WARNING, "device %s has no input %u:%u, removing", inputdev, it->evtype, it->evcode);
		drop_item(it, 0);
		return;
	}

	/* table with ioctl */
	static const int ioctls[EV_CNT] = {
		[EV_KEY] = EVIOCGKEY(sizeof(state)),
		[EV_SW] = EVIOCGSW(sizeof(state)),

	};
	switch (it->evtype) {
	case EV_KEY:
	case EV_SW:
		/* load current state */
		if (ioctl(infd, ioctls[it->evtype], state) < 0)
			mylog(LOG_ERR, "ioctl %s EVIOCG{KEY,SW}: %s", inputdev, ESTR(errno));
		sprintf(buf, "%i", getbit(it->evcode, state));
		break;
	default:
		/* return without publishing */
		return;
	}
	pubitem(it, buf);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int forme;
	char *event;
	struct item *it;

	if (!strcmp(msg->topic, "tools/loglevel")) {
		mysetloglevelstr(msg->payload);
	} else if (test_suffix(msg->topic, mqtt_suffix)) {
		event = strtok(msg->payload ?: "", " \t");
		forme = test_nodename(strtok(NULL, " \t"));

		it = get_item(msg->topic, mqtt_suffix, msg->payloadlen && forme);

		if (!it)
			return;
		/* remove on null config */
		if (!msg->payloadlen || !forme) {
			mylog(LOG_INFO, "removed inputevent for %s", it->topic);
			drop_item(it, 1);
			return;
		}

		mylog(LOG_INFO, "new inputevent for %s", it->topic);
		/* process new inputevent */
		it->asbutton = 0;
		if (!strncmp(event, "button:", 7)) {
			it->evtype = EV_KEY;
			it->evcode = strtoul(event+7, NULL, 0);
			it->asbutton = 1;
		} else if (!strncmp(event, "key:", 4)) {
			it->evtype = EV_KEY;
			it->evcode = strtoul(event+4, NULL, 0);
		} else if (strchr(event, ':')) {
			it->evtype = strtoul(strtok(event, ":"), NULL, 0);
			it->evcode = strtoul(strtok(NULL, ":"), NULL, 0);
		} else if (*event == '#') {
			it->evtype = EV_KEY;
			it->evcode = strtoul(event+1, NULL, 0);
		} else
			mylog(LOG_WARNING, "unparsed inputevent for %s '%s'", it->topic, event);
		if (!it->evtype)
			mylog(LOG_WARNING, "inputevent for %s is invalid!, %u:%u", it->topic, it->evtype, it->evcode);
		pubinitial(it);
	}
}

int main(int argc, char *argv[])
{
	int opt, ret, j, cnt, nevs;
	struct item *it;
	char *str;
	char mqtt_name[32];
	struct pollfd pf[2];
	struct local_input_event evs[16];
	char valuestr[32];

	/* argument parsing */
	while ((opt = getopt_long(argc, argv, optstring, long_opts, NULL)) >= 0)
	switch (opt) {
	case 'V':
		fprintf(stderr, "%s %s\nCompiled on %s %s\n",
				NAME, VERSION, __DATE__, __TIME__);
		fprintf(stderr, "struct input_event size: %u\n", (int)sizeof(*evs));
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

	case 'd':
		inputdev = optarg;
		break;

	case 'p':
		mqtt_prefix = *optarg ? optarg : NULL;
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

	if (!inputdev)
		mylog(LOG_ERR, "no input device specified");

	infd = open(inputdev, O_RDONLY);
	if (infd < 0)
		mylog(LOG_ERR, "open %s: %s", inputdev, ESTR(errno));

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

	/* prepare poll */
	pf[0].fd = infd;
	pf[0].events = POLL_IN;
	pf[1].fd = mosquitto_socket(mosq);
	pf[1].events = POLL_IN;

	while (1) {
		ret = poll(pf, 2, 1000);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0)
			mylog(LOG_ERR, "poll ...");
		if (pf[0].revents) {
			/* read input events */
			ret = read(infd, evs, sizeof(evs));
			if (ret < 0)
				mylog(LOG_ERR, "read %s: %s", inputdev, ESTR(errno));
			nevs = ret/sizeof(*evs);
			for (j = 0; j < nevs; ++j) {
				if (evs[j].type == EV_SYN || evs[j].type == EV_MSC)
					/* ignore SYN events here */
					continue;
				cnt = 0;
				for (it = items; it; it = it->next) {
					if (it->evtype != evs[j].type || it->evcode != evs[j].code)
						continue;
					sprintf(valuestr, "%i", evs[j].value);
					pubitem(it, valuestr);
					++cnt;
				}
				if (mqtt_prefix && evs[j].type == EV_KEY) {
					char *topic;

					asprintf(&topic, "%s/key/%u", mqtt_prefix, evs[j].code);
					sprintf(valuestr, "%i", evs[j].value);
					ret = mosquitto_publish(mosq, NULL, topic, strlen(valuestr), valuestr, mqtt_qos, 0);
					if (ret < 0)
						mylog(LOG_ERR, "mosquitto_publish %s: %s", topic, mosquitto_strerror(ret));
					free(topic);
					++cnt;
				}
				if (!cnt) {
					sprintf(valuestr, "%u:%u %i", evs[j].type, evs[j].code, evs[j].value);
					ret = mosquitto_publish(mosq, NULL, mqtt_unknown_topic, strlen(valuestr), valuestr, mqtt_qos, 0);
					if (ret < 0)
						mylog(LOG_ERR, "mosquitto_publish %s: %s", mqtt_unknown_topic, mosquitto_strerror(ret));
				}
			}
		}
		if (pf[1].revents) {
			/* mqtt read ... */
			ret = mosquitto_loop_read(mosq, 1);
			if (ret)
				mylog(LOG_ERR, "mosquitto_loop_read: %s", mosquitto_strerror(ret));
		}
		/* mosquitto things to do each iteration */
		ret = mosquitto_loop_misc(mosq);
		if (ret)
			mylog(LOG_ERR, "mosquitto_loop_misc: %s", mosquitto_strerror(ret));
		if (mosquitto_want_write(mosq)) {
			ret = mosquitto_loop_write(mosq, 1);
			if (ret)
				mylog(LOG_ERR, "mosquitto_loop_write: %s", mosquitto_strerror(ret));
		}
	}
	return 0;
}
