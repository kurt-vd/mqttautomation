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
#include <sys/ioctl.h>
#include <asm/ioctl.h>
#include <sys/stat.h>
#include <linux/spi/spidev.h>
#include <mosquitto.h>
#include <arpa/inet.h>

#include "lib/libt.h"
#include "common.h"

#define NAME "mqttled"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* program options */
static const char help_msg[] =
	NAME ": an MQTT to APA102 LED array bridge\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=STR	Give MQTT topic suffix for timeouts (default '/apa102hw')\n"
	" -w, --write=STR	Give MQTT topic suffix for writing the topic (default empty)\n"
	" -d, --device=SPIDEV	GIve SPI device file (default /dev/spidev0.0)\n"
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

	{ "device", required_argument, NULL, 'd', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:s:w:d:";

/* logging */
static int loglevel = LOG_WARNING;

/* signal handler */
static volatile int sigterm;

static void onsigterm(int sig)
{
	sigterm = 1;
}

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_suffix = "/apa102hw";
static const char *mqtt_write_suffix;
static int mqtt_suffixlen = 9;
static int mqtt_write_suffixlen;
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
	int index;
	int rgb;
	int republish;
};

struct item *items;

/* apa102 spi transfer */
static const char *spidev = "/dev/spidev0.0";
static int spisk;
static int led_count; /* the numer of leds in the array to write */
static int deleted_led_count; /* the number of leds in the array to write,
				 that have been deleted */
static int spi_scheduled;
static uint32_t *pdat;
static int ndat;

static inline int rgb_to_bgr(int rgb)
{
	int r, g, b;

	r = rgb & 0xff0000;
	g = rgb & 0x00ff00;
	b = rgb & 0x0000ff;
	return (r >> 16) | g | (b << 16);
}

static void spi_write_apa102(void *dat)
{
	struct item *it;
	int j, ret, myledcnt;
	struct spi_ioc_transfer xf = {};

	spi_scheduled = 0;
	if (!led_count) {
		for (it = items; it; it = it->next) {
			if (it->index >= led_count)
				led_count = it->index+1;
		}
	}
	myledcnt = led_count;
	if (deleted_led_count > myledcnt)
		myledcnt = deleted_led_count;
	if (ndat < (led_count+3)) {
		ndat = led_count+3;
		pdat = realloc(pdat, ndat*sizeof(*pdat));
	}

	/* set fixed sync items */
	pdat[0] = 0;
	pdat[ndat-2] = 0;
	pdat[ndat-1] = ~0;
	/* initialize led array */
	for (j = 1; j < ndat-2; ++j)
		pdat[j] = htonl(0xe0000000);
	/* fill data */
	if (!sigterm)
		/* only put real data when not quitting */
	for (it = items; it; it = it->next)
		pdat[it->index+1] = htonl(0xff000000 | rgb_to_bgr(it->rgb));

	/* spi transfer */
	xf.tx_buf = (unsigned long)pdat;
	xf.len = ndat*sizeof(*pdat);
	ret = ioctl(spisk, SPI_IOC_MESSAGE(1), &xf);
	if (ret < 0) {
		mylog(LOG_WARNING, "spi transfer failed: %s", ESTR(errno));
		return;
	}
	if (sigterm)
		return;
	/* all leds written, forget possibly deleted leds */
	deleted_led_count = 0;

	for (it = items; it; it = it->next) {
		if (!it->republish)
			continue;
		char buf[16];
		sprintf(buf, "#%06x", it->rgb & 0xffffff);
		/* publish, retained when writing the topic, volatile (not retained) when writing to another topic */
		ret = mosquitto_publish(mosq, NULL, it->topic, strlen(buf), buf, mqtt_qos, 1);
		if (ret < 0)
			mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
		it->republish = 0;
	}
}

/* colors */
static int rgb_to_rrggbb(int value)
{
	int r, g, b;

	r = (value >> 8) & 0xf;
	g = (value >> 4) & 0xf;
	b = (value >> 0) & 0xf;
	return (((r << 4) + r) << 16) + (((g << 4) + g) << 8) + (((b << 4) + b) << 0);
}

static const struct {
	int rgb;
	const char *name;
} colormap[] = {
	{ 0x000000, "black", },
	{ 0xffffff, "white", },
	{ 0x0000ff, "blue", },
	{ 0xa52a2a, "brown", },
	{ 0x00ffff, "cyan", },
	{ 0xffd700, "gold", },
	{ 0x00ff00, "green", },
	{ 0xffff00, "yellow", },
	{ 0xffa500, "orange", },
	{ 0xff0000, "red", },
	{ 0xff1493, "pink", },
	{ 0xff00ff, "fuchsia", },
	{ 0x800080, "purple", },
};
#define NCOLORS	(sizeof(colormap)/sizeof(colormap[0]))

static void setled(struct item *it, const char *newvalue, int republish)
{
	int ret, newval, j;
	char *endp;

	newval = it->rgb;
	if (!newvalue || !*newvalue)
		newval = 0;
	else if (*newvalue == '#') {
		/* RRGGBB format */
		mylog(LOG_WARNING, "%s: RRGGBB %s", it->topic, newvalue+1);
		newval = strtoul(newvalue+1, &endp, 16);
		if (endp-newvalue == 3+1)
			newval = rgb_to_rrggbb(newval);
	} else if (*newvalue >= '0' && *newvalue <= '9') {
		ret = strtoul(newvalue, NULL, 10);
		if (ret < NCOLORS)
			newval = colormap[ret].rgb;
		else
			newval = colormap[ret ? 1 : 0].rgb;
	} else {
		for (j = 0; j < NCOLORS; ++j) {
			if (!strcasecmp(colormap[j].name, newvalue)) {
				newval = colormap[j].rgb;
				break;
			}
		}
		if (j >= NCOLORS)
			/* turn black */
			newval = 0;
	}
	if (newval != it->rgb) {
		it->rgb = newval;
		it->republish = republish && mqtt_write_suffix;
		if (!spi_scheduled)
			libt_add_timeout(0.001, spi_write_apa102, NULL);
	}
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

	/* set republish initially, as we can't hold writing it sometime */
	it->republish = 1;

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

	/* remove from list */
	if (it->prev)
		it->prev->next = it->next;
	if (it->next)
		it->next->prev = it->prev;

	/* clear cached ledcount */
	led_count = 0;
	/* mark led index as deleted */
	if (deleted_led_count <= it->index)
		deleted_led_count = it->index+1;

	ret = mosquitto_unsubscribe(mosq, NULL, it->writetopic ?: it->topic);
	if (ret)
		mylog(LOG_ERR, "mosquitto_unsubscribe '%s': %s", it->writetopic ?: it->topic, mosquitto_strerror(ret));
	/* free memory */
	free(it->topic);
	if (it->writetopic)
		free(it->writetopic);
	free(it);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int forme;
	char *ledspec;
	struct item *it;

	if (test_suffix(msg->topic, mqtt_suffix)) {
		/* grab boardname */
		ledspec = strtok(msg->payload ?: "", " \t");
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

		/* process new led spec */
		it->index = strtoul(ledspec, NULL, 0);
		mylog(LOG_INFO, "new apa102 led spec for %s: %i", it->topic, it->index);
		/* clear cached ledcount */
		led_count = 0;

	} else if ((it = get_item(msg->topic, mqtt_write_suffix, 0)) != NULL) {
		/* this is the write topic */
		setled(it, msg->payload, 1);

	} else if ((!mqtt_write_suffix || msg->retain) &&
			(it = get_item(msg->topic, NULL, 0)) != NULL) {
		/* this is the main led topic */
		setled(it, msg->payload, 0);
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
		mqtt_write_suffix = optarg;
		mqtt_write_suffixlen = strlen(mqtt_write_suffix);
		break;

	case 'd':
		spidev = optarg;
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

	/* SPI device */
	ret = spisk = open(spidev, O_RDWR);
	if (ret < 0)
		mylog(LOG_ERR, "open %s failed: %s", spidev, ESTR(errno));
	uint8_t bits = 8;
	if (ioctl(spisk, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
		mylog(LOG_ERR, "%s set 8bits: %s", spidev, ESTR(errno));

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

	sigaction(SIGTERM, &(struct sigaction){ .sa_handler = onsigterm, }, NULL);
	sigaction(SIGINT, &(struct sigaction){ .sa_handler = onsigterm, }, NULL);

	while (!sigterm) {
		libt_flush();
		waittime = libt_get_waittime();
		if (waittime > 1000)
			waittime = 1000;
		ret = mosquitto_loop(mosq, waittime, 1);
		if (ret)
			mylog(LOG_ERR, "mosquitto_loop: %s", mosquitto_strerror(ret));
	}
	/* blank all leds (sigterm set) */
	spi_write_apa102(NULL);
	return 0;
}
