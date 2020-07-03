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
#include <endian.h>
#include <fcntl.h>
#include <glob.h>
#include <getopt.h>
#include <syslog.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <mosquitto.h>

#include "common.h"
#include "lib/libt.h"
#include "lib/libe.h"

#define NAME "mqttiio"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* program options */
static const char help_msg[] =
	NAME ": bridge IIO into MQTT\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=STR	Give MQTT topic suffix for spec (default '/iiohw')\n"
	" -N, --nomqtt		Only emit incoming event without propagating to MQTT\n"
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
	{ "nomqtt", no_argument, NULL, 'N', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:s:N";

/* logging */
static int loglevel = LOG_WARNING;

/* signal handler */
static volatile int sigterm;
static volatile int ready;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_suffix = "/iiohw";
static int mqtt_suffixlen = 6;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;
static const char mqtt_unknown_topic[] = "unhandled/iio";
static int nomqtt;

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;
	struct item *prev;

	char *topic;
	int topiclen;
	char *device;
	char *element;
	const struct iioel *iio; /* abstract pointer, for quick compare */

	double hyst;
	double oldvalue;
};

struct item *items;

/* MQTT iface */
__attribute__((unused))
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

	/* set defaults */
	it->hyst = NAN;
	it->oldvalue = NAN;

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

	/* publish, volatile for buttons, retained for the rest */
	ret = mosquitto_publish(mosq, NULL, it->topic, strlen(payload), payload, mqtt_qos, 1);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
}

static void link_item(struct item *it);
static void add_device(const char *devname);
static void remove_device(const char *devname);
static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int forme;
	char *dev, *el;
	struct item *it;

	if (is_self_sync(msg)) {
		ready = 1;
	} else if (!strcmp(msg->topic, "tools/loglevel")) {
		mysetloglevelstr(msg->payload);
	} else if (!strncmp(msg->topic, "config/", 7)) {
		int namelen = strlen(NAME);
		if (!strncmp(msg->topic+7, NAME "/", namelen+1)) {
			if (!strcmp(msg->topic+7+namelen+1, "loglevel"))
				mysetloglevelstr(msg->payload);
			else if (!strcmp(msg->topic+7+namelen+1, "add"))
				add_device(msg->payload);
			else if (!strcmp(msg->topic+7+namelen+1, "remove"))
				remove_device(msg->payload);
		}
	} else if (test_suffix(msg->topic, mqtt_suffix)) {
		dev = strtok(msg->payload ?: "", " \t");
		el = strtok(NULL, " \t");

		forme = test_nodename(strtok(NULL, " \t"));

		it = get_item(msg->topic, mqtt_suffix, msg->payloadlen && forme);

		if (!it)
			return;
		/* remove on null config */
		if (!msg->payloadlen || !forme) {
			mylog(LOG_INFO, "removed iio element for %s", it->topic);
			drop_item(it, 1);
			return;
		}

		mylog(LOG_INFO, "new iio element for %s", it->topic);
		/* process new iio element */
		if (it->device)
			free(it->device);
		it->device = strdup(dev);
		if (it->element)
			free(it->element);
		it->element = strdup(el);
		link_item(it);
	}
}

struct iiodev {
	struct iiodev *next;
	struct iiodev *prev;

	struct iioel *els;
	int nels, sels;
	int fd;
	char *name;
	char *hname; /* /name property */
	uint8_t *dat;
	uint8_t *olddat;
	int datsize;
	int olddatvalid;
};
struct iioel {
	char *name;
	int index;
	int location;
	int enabled;
	int le;
	int sign;
	int bitsused;
	int bytesused;
	int shift;
	double scale;
	double offset;
	double si_mult;
	/* standard hysteresis for this element
	 * this allows the program to set hysteresis
	 * based on type of element,
	 * so to limit output with '-N' program option
	 */
	double hyst;
	double oldvalue;
};

static struct iiodev *iiodevs;

static void remove_iio(struct iiodev *dev);
/* round/align to @align */
const char *mydtostr_align(double d, double align)
{
	if (isnan(d))
		return "";
	if (align > 0)
		d -= fmod(d, align);
	return mydtostr(d);
}
const char *mydtostr_align2(double d, double align)
{
	if (isnan(d))
		return mydtostr(d);
	return mydtostr_align(d, align);
}

static void iiodev_data(int fd, void *dat)
{
	struct iiodev *dev = dat;
	struct iioel *el;
	int ret, nitems, newdatvalid, requiredsize;
	double valf;
	uint16_t val16;
	int32_t val32;
	int64_t val64;
	struct item *it;
	const char *payload;

	newdatvalid = ret = read(fd, dev->dat, dev->datsize);
	if (ret < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (ret < 0)
		mylog(LOG_ERR, "read %u from /dev/%s: %s", dev->datsize, dev->name, ESTR(errno));
	if (!ret) {
		mylog(LOG_WARNING, "/dev/%s %s eof", dev->name, dev->hname);
		remove_iio(dev);
		return;
	}
	if (newdatvalid != dev->olddatvalid)
		mylog(LOG_INFO, "read %u/%u from /dev/%s", ret, dev->datsize, dev->name);

	for (el = dev->els; el < &dev->els[dev->nels]; ++el) {
		if (!el->enabled)
			continue;
		requiredsize = el->location + el->bytesused;
		if (dev->olddatvalid >= requiredsize && newdatvalid >= requiredsize &&
				!memcmp(dev->dat+el->location, dev->olddat+el->location, el->bytesused))
			/* no change */
			continue;
		payload = NULL;
		if (newdatvalid < requiredsize) {
			if (dev->olddatvalid < requiredsize)
				/* no change, both old & new data
				 * do not contain this element
				 */
				continue;
			payload = "";
		} else
		switch (el->bytesused) {
		case 1:
			val32 = dev->dat[el->location];
			val32 >>= el->shift;
			val32 &= (1 << el->bitsused)-1;
			if (el->sign && val32 & (1 << (el->bitsused-1)))
				/* stuff highest bits */
				val32 |= ~((1 << el->bitsused)-1);
			valf = (val32-el->offset)*el->scale;
			break;
		case 2:
			memcpy(&val16, dev->dat+el->location, 2);
			val32 = el->le ? le16toh(val16) : be16toh(val16);

			val32 >>= el->shift;
			val32 &= (1 << el->bitsused)-1;
			if (el->sign && val32 & (1 << (el->bitsused-1)))
				/* stuff highest bits */
				val32 |= ~((1 << el->bitsused)-1);
			valf = (val32-el->offset)*el->scale;
			break;
		case 4:
			memcpy(&val32, dev->dat+el->location, 4);
			val32 = el->le ? le32toh(val32) : be32toh(val32);

			val32 >>= el->shift;
			if (el->bitsused < 32) {
				val32 &= (1 << el->bitsused)-1;
				if (el->sign && val32 & (1 << (el->bitsused-1)))
					/* stuff highest bits */
					val32 |= ~((1 << el->bitsused)-1);
			}
			if (!el->sign)
				valf = ((uint32_t)val32+el->offset)*el->scale;
			else
				valf = (val32-el->offset)*el->scale;
			break;
		case 8:
			memcpy(&val64, dev->dat+el->location, 8);
			val64 = el->le ? le64toh(val64) : be64toh(val64);

			val64 >>= el->shift;
			if (el->bitsused < 64) {
				val64 &= ((uint64_t)1 << el->bitsused)-1;
				if (el->sign && val64 & ((uint64_t)1 << (el->bitsused-1)))
					/* stuff highest bits */
					val64 |= ~(((uint64_t)1 << el->bitsused)-1);
			}
			if (!strcmp(el->name, "timestamp") && el->offset == 0.0 && el->scale == 1.0) {
				static char longbuf[128];

				sprintf(longbuf, "%llu.%06llu", (unsigned long long)val64 / 1000000000ULL,
						((unsigned long long)val64 % 1000000000ULL)/1000);
				payload = longbuf;
			} else
			if (!el->sign)
				valf = ((uint64_t)val64+el->offset)*el->scale;
			else
				valf = (val64-el->offset)*el->scale;
			break;
		default:
			valf = NAN;
			break;
		}

		valf *= el->si_mult;
		nitems = 0;
		if (nomqtt) {
			if ((isnan(el->oldvalue) && isnan(valf)) || fabs(el->oldvalue - valf) < el->hyst)
				;
			else {
				printf("%s %s: %s\n", dev->hname, el->name, payload ?: mydtostr_align2(valf, el->hyst));
				el->oldvalue = valf;
			}
			continue;
		}

		for (it = items; it; it = it->next) {
			if (it->iio != el)
				continue;
			++nitems;
			/* test against hysteresis */
			if (fabs(it->oldvalue - valf) < it->hyst)
				continue;
			pubitem(it, payload ?: mydtostr_align(valf, it->hyst));
			it->oldvalue = valf;
		}
		if (!nitems && strcmp(el->name, "timestamp")) {
			int ret;

			/* publish to unknow topic */
			payload = payload ?: mydtostr_align(valf, el->hyst);
			ret = mosquitto_publish(mosq, NULL, mqtt_unknown_topic, strlen(payload), payload, mqtt_qos, 0);
			if (ret < 0)
				mylog(LOG_ERR, "mosquitto_publish %s: %s", mqtt_unknown_topic, mosquitto_strerror(ret));
		}
		el->oldvalue = valf;
	}
	memcpy(dev->olddat, dev->dat, dev->datsize);
	dev->olddatvalid = newdatvalid;
	if (!nomqtt)
		fflush(stdout);
}

static int elementcmp(const void *va, const void *vb)
{
	const struct iioel *a = va, *b = vb;

	return a->index - b->index;
}

__attribute__((format(printf,2,3)))
static const char *prop_read2(int acceptable_errno, const char *fmt, ...)
{
	static char value[1024];
	va_list va;
	char *filename;
	int fd, ret;

	va_start(va, fmt);
	vasprintf(&filename, fmt, va);
	va_end(va);

	fd = open(filename, O_RDONLY);
	if (fd < 0 && errno != acceptable_errno)
		mylog(LOG_ERR, "open %s: %s", filename, ESTR(errno));
	if (fd < 0) {
		free(filename);
		return NULL;
	}
	ret = read(fd, value, sizeof(value)-1);
	if (ret < 0)
		mylog(LOG_ERR, "read %s: %s", filename, ESTR(errno));
	close(fd);
	/* null terminate */
	value[ret] = 0;
	free(filename);
	if (ret && value[ret-1] == '\n')
		value[--ret] = 0;
	return value;
}

#define prop_read(fmt, ...) prop_read2(0, fmt, ##__VA_ARGS__)

static void load_element(const struct iiodev *dev, struct iioel *el)
{
	static char filename[2048];
	const char *prop;

	/* read index */
	prop = prop_read("/sys/bus/iio/devices/%s/scan_elements/in_%s_index", dev->name, el->name);
	el->index = strtoul(prop, NULL, 0);

	/* read enable */
	prop = prop_read("/sys/bus/iio/devices/%s/scan_elements/in_%s_en", dev->name, el->name);
	el->enabled = strtoul(prop, NULL, 0);

	/* decode type */
	prop = prop_read("/sys/bus/iio/devices/%s/scan_elements/in_%s_type", dev->name, el->name);
	char le, sign;
	int bitsfill;
	if (sscanf(prop, "%ce:%c%u/%u>>%u", &le, &sign,
			&el->bitsused, &bitsfill, &el->shift) != 5)
		mylog(LOG_ERR, "wrong format for type '%s' of %s", prop, filename);
	el->le = le == 'l';
	el->sign = sign == 's';
	el->bytesused = bitsfill / 8;

	/* decode offset */
	prop = prop_read2(ENOENT, "/sys/bus/iio/devices/%s/in_%s_offset", dev->name, el->name);
	el->offset = strtod(prop ?: "0", NULL);

	/* decode scale */
	prop = prop_read2(ENOENT, "/sys/bus/iio/devices/%s/in_%s_scale", dev->name, el->name);
	el->scale = strtod(prop ?: "1", NULL);

	/* find si multiplier */
	if (!strncmp("temp", el->name, 4)) {
		el->si_mult = 1e-3;
		el->hyst = 0.5;
	} else if (!strncmp("humidity", el->name, 8)) {
		el->si_mult = 1e-2;
		el->hyst = 1e-2;
	} else {
		el->si_mult = 1;
		el->hyst = 0;
	}
	/* invalidate the cache */
	el->oldvalue = NAN;
}

/* link iioel to item */
static void link_element(const struct iiodev *dev, const struct iioel *el, struct item *it)
{
	mylog(LOG_INFO, "link %s,%s to %s", dev->hname, el->name, it->topic);
	/* link */
	it->iio = el;
	/* inherit hysteris if not set */
	if (isnan(it->hyst))
		it->hyst = el->hyst;
	if (!isnan(it->oldvalue) || !isnan(el->oldvalue)) {
		it->oldvalue = el->oldvalue;
		pubitem(it, mydtostr_align(it->oldvalue, it->hyst));
	}
}

/* link existing items to new iioel */
static void link_elements(struct iiodev *dev, struct iioel *el)
{
	struct item *it;

	if (!el->enabled)
		/* do not link anything */
		return;
	for (it = items; it; it = it->next) {
		/* match device name */
		if (strcmp(it->device, dev->name) && strcmp(it->device, dev->hname))
			continue;
		/* match element */
		if (strcmp(it->element, el->name))
			continue;
		link_element(dev, el, it);
	}
}

/* link existing iioelements to new item */
static void link_item(struct item *it)
{
	struct iiodev *dev;
	int j;

	for (dev = iiodevs; dev; dev = dev->next) {
		/* match device name */
		if (strcmp(it->device, dev->name) && strcmp(it->device, dev->hname))
			continue;
		for (j = 0; j < dev->nels; ++j) {
			if (strcmp(it->element, dev->els[j].name))
				continue;
			link_element(dev, &dev->els[j], it);
			return;
		}
	}
	/* nothing found */
	it->iio = NULL;
	if (!isnan(it->oldvalue)) {
		it->oldvalue = NAN;
		pubitem(it, NULL);
	}
}

static void remove_iio(struct iiodev *dev)
{
	struct iioel *el;
	struct item *it;

	mylog(LOG_INFO, "remove %s", dev->name);
	/* unlink from linked list */
	if (dev->next)
		dev->next->prev = dev->prev;
	if (dev->prev)
		dev->prev->next = dev->next;
	/* cleanup elements */
	for (el = dev->els; el < &dev->els[dev->nels]; ++el) {
		free(el->name);
		free(el);
		/* unlink items */
		for (it = items; it; it = it->next) {
			if (it->iio == el) {
				it->iio = NULL;
				if (!isnan(it->oldvalue)) {
					it->oldvalue = NAN;
					pubitem(it, NULL);
				}
			}
		}
	}
	/* cleanup */
	libe_remove_fd(dev->fd);
	close(dev->fd);
	free(dev->olddat);
	free(dev->dat);
	free(dev->hname);
	free(dev->name);
	free(dev);
}

static void remove_device(const char *devname)
{
	struct iiodev *dev;

	/* strip leading /dev/ */
	if (!strncmp("/dev/", devname, 5))
		devname += 5;

	/* find device */
	for (dev = iiodevs; dev; dev = dev->next) {
		if (!strcmp(devname, dev->name))
			break;
	}
	if (dev)
		remove_iio(dev);
}

static void add_device(const char *devname)
{
	glob_t els = {};
	int j, ret;
	struct iiodev *dev;
	struct iioel *el;
	static char filename[2048];
	const char *humanname;

	/* strip leading /dev/ */
	if (!strncmp("/dev/", devname, 5))
		devname += 5;
	/* find device */
	for (dev = iiodevs; dev; dev = dev->next) {
		if (!strcmp(devname, dev->name))
			break;
	}

	if (!dev) {
		mylog(LOG_INFO, "add %s", devname);
		humanname = prop_read2(ENOENT, "/sys/bus/iio/devices/%s/name", devname);

		/* verify if iio device is buffered
		 * I only handle buffered iio devices ...
		 * non-buffered devices can be handled like hwmon devices
		 */
		sprintf(filename, "/sys/bus/iio/devices/%s/buffer/enable", devname);
		if (access(filename, F_OK) < 0) {
			if (errno != ENOENT && errno != ENOTDIR)
				mylog(LOG_ERR, "access %s failed: %s", filename, ESTR(errno));
			mylog(LOG_INFO, "%s (%s) is not buffered, skipping", devname, humanname ?: "");
			return;
		}

		/* create new device */
		dev = malloc(sizeof(*dev));
		if (!dev)
			mylog(LOG_ERR, "malloc iiodev: %s", ESTR(errno));
		memset(dev, 0, sizeof(*dev));
		dev->name = strdup(devname);
		dev->hname = strdup(humanname ?: devname);

		/* open file */
		char filename[128];
		sprintf(filename, "/dev/%s", devname);
		dev->fd = open(filename, O_RDONLY | O_NONBLOCK);
		if (dev->fd < 0)
			mylog(LOG_ERR, "open %s: %s", devname, ESTR(errno));
		libe_add_fd(dev->fd, iiodev_data, dev);
		/* insert in linked list */
		dev->next = iiodevs;
		if (dev->next)
			dev->next->prev = dev;
		dev->prev = (struct iiodev *)&iiodevs; /* trickery */
		iiodevs = dev;
		mylog(LOG_INFO, "probed %s (%s)", devname, humanname);
	}

	/* grab scan elements for new devs */
	mylog(LOG_INFO, "scan %s", devname);
	char *elname;
	struct item *it;

	char pattern[128];
	sprintf(pattern, "/sys/bus/iio/devices/%s/scan_elements/in_*_en", dev->name);
	ret = glob(pattern, 0, NULL, &els);
	if (ret == GLOB_NOMATCH) {
		remove_iio(dev);
		return;
	}
	if (ret)
		mylog(LOG_ERR, "glob scan_elements: %s", ESTR(errno));

	/* mark old elements inactive */
	for (el = dev->els; el < dev->els+dev->nels; ++el) {
		/* publish lost items */
		for (it = items; it; it = it->next) {
			if (it->iio == el && !isnan(it->oldvalue)) {
				it->oldvalue = NAN;
				pubitem(it, NULL);
				it->iio = NULL;
			}
		}
		free(el->name);
	}
	dev->nels = 0;

	/* find scan elements for this device */
	for (j = 0; j < els.gl_pathc; ++j) {
		elname = strrchr(els.gl_pathv[j], '/');
		if (!elname)
			continue;
		/* pre-alloc room */
		if (dev->nels+1 > dev->sels) {
			dev->sels += 16;
			dev->els = realloc(dev->els, dev->sels*sizeof(*dev->els));
			if (!dev->els)
				mylog(LOG_ERR, "realloc %i elements: %s", dev->sels, ESTR(errno));
		}
		el = dev->els+dev->nels++;
		memset(el, 0, sizeof(*el));
		el->oldvalue = NAN;
		/* fill element */
		el->name = strndup(elname+4, strlen(elname)-7); /* strip in_*_en from name */
		load_element(dev, el);
		mylog(LOG_INFO, "new channel (%s) %s, %s", dev->name, dev->hname, el->name);
	}
	/* sort by index */
	qsort(dev->els, dev->nels, sizeof(*dev->els), elementcmp);
	/* determine locaion (offsets in the byte stream) */
	for (el = dev->els; el < dev->els+dev->nels; ++el) {
		int mod;

		if (!el->enabled)
			/* disabled elements do not count */
			continue;
		mod = dev->datsize % el->bytesused;
		if (mod)
			dev->datsize += el->bytesused - mod;
		el->location = dev->datsize;
		dev->datsize += el->bytesused;
		link_elements(dev, el);
	}
	/* prepare read buffer */
	dev->dat = realloc(dev->dat, dev->datsize);
	dev->olddat = realloc(dev->olddat, dev->datsize);
	if (dev->datsize && (!dev->dat || !dev->olddat))
		mylog(LOG_ERR, "(re)alloc %u dat for %s: %s", dev->datsize, dev->hname, ESTR(errno));
	dev->olddatvalid = 0;
	globfree(&els);
}

static void scan_all_devices(void)
{
	glob_t devs = {};
	int ret, j;

	ret = glob("/dev/iio:device*", 0, NULL, &devs);
	if (ret == GLOB_NOMATCH)
		return;
	if (ret)
		mylog(LOG_ERR, "glob /dev/iio:device* : %s", ESTR(errno));
	/* open new devices */
	for (j = 0; j < devs.gl_pathc; ++j)
		add_device(devs.gl_pathv[j]+5);

	globfree(&devs);
}

static void mqtt_fd_ready(int fd, void *dat)
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

static void mqtt_update_flags(void *dat)
{
	struct mosquitto *mosq = dat;

	if (mosq)
		libe_mod_fd(mosquitto_socket(mosq), LIBE_RD | (mosquitto_want_write(mosq) ? LIBE_WR : 0));
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
	struct item *it;
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

	case 'N':
		nomqtt = 1;
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
	if (!nomqtt) {
		mosquitto_lib_init();
		sprintf(mqtt_name, "%s-%i", NAME, getpid());
		mosq = mosquitto_new(mqtt_name, true, 0);
		if (!mosq)
			mylog(LOG_ERR, "mosquitto_new failed: %s", ESTR(errno));
		/* mosquitto_will_set(mosq, "TOPIC", 0, NULL, mqtt_qos, 1); */

		//mosquitto_log_callback_set(mosq, my_mqtt_log);
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
		libe_add_fd(mosquitto_socket(mosq), mqtt_fd_ready, mosq);
	}

	/* prepare signalfd */
	sigset_t sigmask;
	int sigfd;

	sigfillset(&sigmask);

	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0)
		mylog(LOG_ERR, "sigprocmask: %s", ESTR(errno));
	sigfd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sigfd < 0)
		mylog(LOG_ERR, "signalfd failed: %s", ESTR(errno));
	libe_add_fd(sigfd, signalrecvd, NULL);

	/* prepare epoll */
	scan_all_devices();

	/* core loop */
	while (!sigterm) {
		libt_flush();
		mqtt_update_flags(mosq);
		ret = libe_wait(libt_get_waittime());
		if (ret >= 0)
			libe_flush();
	}

	if (nomqtt)
		goto mqtt_done;

	/* stop listening to new input */
	struct iiodev *dev;
	for (dev = iiodevs; dev; dev = dev->next)
		libe_remove_fd(dev->fd);

	/* erase all data */
	for (it = items; it; it = it->next) {
		it->iio = NULL; /* quick unlink */
		mosquitto_publish(mosq, 0, it->topic, 0, NULL, mqtt_qos, 1);
	}

	/* terminate */
	send_self_sync(mosq, mqtt_qos);
	while (!ready) {
		mqtt_update_flags(mosq);
		libt_flush();
		ret = libe_wait(libt_get_waittime());
		if (ret >= 0)
			libe_flush();
	}
#if 0
	/* cleanup */
	mosquitto_disconnect(mosq);
	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();
#endif
mqtt_done:
	return 0;
}
