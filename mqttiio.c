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
	"usage:	" NAME " -d DEVICE [OPTIONS ...] [PATTERN] ...\n"
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

	double hyst;
	double oldvalue;
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

static void pubinitial(struct item *it);
static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int forme;
	char *dev, *el;
	struct item *it;

	/*if (is_self_sync(msg)) {
		ready = 1;
	} else */if (!strcmp(msg->topic, "tools/loglevel")) {
		mysetloglevelstr(msg->payload);
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
		pubinitial(it);
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
	int dirty;
	uint8_t *dat;
	uint8_t *olddat;
	int datsize;
	int olddatvalid;
};
struct iioel {
	char *name;
	int index;
	int location;
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

/* round/align to @align */
const char *mydtostr_align(double d, double align)
{
	return mydtostr(d - fmod(d, align));
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
	if (!ret)
		/* TODO: recover, close in runtime */
		mylog(LOG_ERR, "/dev/%s eof", dev->name);
	if (newdatvalid != dev->olddatvalid)
		mylog(LOG_INFO, "read %u/%u from /dev/%s", ret, dev->datsize, dev->name);

	for (el = dev->els; el < &dev->els[dev->nels]; ++el) {
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
			goto payload_ready;
		}
		switch (el->bytesused) {
		case 1: {
			val32 = dev->dat[el->location];
			val32 >>= el->shift;
			val32 &= (1 << el->bitsused)-1;
			if (el->sign && val32 & (1 << (el->bitsused-1)))
				/* stuff highest bits */
				val32 |= ~((1 << el->bitsused)-1);
			valf = (val32+el->offset)*el->scale;
		} break;
		case 2: {
			memcpy(&val16, dev->dat+el->location, 2);
			val32 = el->le ? le16toh(val16) : be16toh(val16);

			val32 >>= el->shift;
			val32 &= (1 << el->bitsused)-1;
			if (el->sign && val32 & (1 << (el->bitsused-1)))
				/* stuff highest bits */
				val32 |= ~((1 << el->bitsused)-1);
			valf = (val32+el->offset)*el->scale;
		} break;
		case 4: {
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
				valf = (val32+el->offset)*el->scale;
		} break;
		case 8: {
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

				sprintf(longbuf, "%llu", val64);
				payload = longbuf;
				goto payload_ready;
			} else
			if (!el->sign)
				valf = ((uint64_t)val64+el->offset)*el->scale;
			else
				valf = (val64+el->offset)*el->scale;
		} break;
		default:
			valf = NAN;
			break;
		}

		valf *= el->si_mult;
payload_ready:
		nitems = 0;
		if (nomqtt) {
			if (fabs(el->oldvalue - valf) < el->hyst)
				;
			else {
				printf("%s %s: %s\n", dev->hname, el->name, payload ?: mydtostr_align(valf, el->hyst));
				el->oldvalue = valf;
			}
			continue;
		}

		for (it = items; it; it = it->next) {
			if (strcmp(it->device, dev->hname) ||
					strcmp(it->element, el->name))
				continue;
			++nitems;
			/* inherit hysteris if not set */
			if (isnan(it->hyst))
				it->hyst = el->hyst;
			/* test against hysteresis */
			if (fabs(it->oldvalue - valf) < it->hyst)
				continue;
			pubitem(it, mydtostr_align(valf, it->hyst));
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

static struct iiodev *find_iiodev(const char *devname);
static void pubinitial(struct item *it)
{
	struct iiodev *dev;
	struct iioel *el;

	dev = find_iiodev(it->device);
	if (!dev)
		return;
	for (el = dev->els; el < dev->els+dev->nels; ++el) {
		if (!strcmp(el->name, it->element)) {
			if (isnan(it->hyst))
				it->hyst = el->hyst;
			if (isnan(el->oldvalue))
				/* don't publish yet */
			it->oldvalue = el->oldvalue;
			pubitem(it, mydtostr_align(el->oldvalue, it->hyst));
			return;
		}
	}
}

static struct iiodev *find_iiodev(const char *devname)
{
	struct iiodev *iio;

	for (iio = iiodevs; iio; iio = iio->next) {
		if (!strcmp(iio->name, devname))
			return iio;
	}
	return NULL;
}

static int elementcmp(const void *va, const void *vb)
{
	const struct iioel *a = va, *b = vb;

	return a->index - b->index;
}

static void load_element(const struct iiodev *dev, struct iioel *el)
{
	static char filename[2048];
	static char prop[1024];
	int fd, ret;

	/* read index */
	sprintf(filename, "/sys/bus/iio/devices/%s/scan_elements/in_%s_index", dev->name, el->name);
	ret = fd = open(filename, O_RDONLY);
	if (ret < 0)
		mylog(LOG_ERR, "open %s: %s", filename, ESTR(errno));
	ret = read(fd, prop, sizeof(prop)-1);
	if (ret < 0)
		mylog(LOG_ERR, "read %s: %s", filename, ESTR(errno));
	close(fd);
	prop[ret] = 0;
	/* parse */
	el->index = strtoul(prop, NULL, 0);

	/* decode type */
	sprintf(filename, "/sys/bus/iio/devices/%s/scan_elements/in_%s_type", dev->name, el->name);
	ret = fd = open(filename, O_RDONLY);
	if (ret < 0)
		mylog(LOG_ERR, "open %s: %s", filename, ESTR(errno));
	ret = read(fd, prop, sizeof(prop)-1);
	if (ret < 0)
		mylog(LOG_ERR, "read %s: %s", filename, ESTR(errno));
	close(fd);
	prop[ret] = 0;
	/* parse */
	char le, sign;
	int bitsfill;
	if (sscanf(prop, "%ce:%c%u/%u>>%u", &le, &sign,
			&el->bitsused, &bitsfill, &el->shift) != 5)
		mylog(LOG_ERR, "wrong format for type '%s' of %s", prop, filename);
	el->le = le == 'l';
	el->sign = sign == 's';
	el->bytesused = bitsfill / 8;

	/* decode offset */
	sprintf(filename, "/sys/bus/iio/devices/%s/in_%s_offset", dev->name, el->name);
	ret = fd = open(filename, O_RDONLY);
	if (ret < 0 && errno != ENOENT)
		mylog(LOG_ERR, "open %s: %s", filename, ESTR(errno));
	if (ret < 0) {
		prop[0] = 0;
		goto parse_offset;
	}
	ret = read(fd, prop, sizeof(prop)-1);
	if (ret < 0)
		mylog(LOG_ERR, "read %s: %s", filename, ESTR(errno));
	close(fd);
	prop[ret] = 0;
parse_offset:
	el->offset = strtod(prop, NULL);

	/* decode scale */
	sprintf(filename, "/sys/bus/iio/devices/%s/in_%s_scale", dev->name, el->name);
	ret = fd = open(filename, O_RDONLY);
	if (ret < 0 && errno != ENOENT)
		mylog(LOG_ERR, "open %s: %s", filename, ESTR(errno));
	if (ret < 0) {
		strcpy(prop, "1");
		goto parse_scale;
	}
	ret = read(fd, prop, sizeof(prop)-1);
	if (ret < 0)
		mylog(LOG_ERR, "read %s: %s", filename, ESTR(errno));
	close(fd);
	prop[ret] = 0;
parse_scale:
	el->scale = strtod(prop, NULL);

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

static void scan_iio(int destroy)
{
	glob_t devs = {}, els = {};
	int j, fd, ret;
	struct iiodev *dev;
	struct iioel *el;
	static char filename[2048];
	char *devname, *humanname;

	/* clean dirty */
	for (dev = iiodevs; dev; dev = dev->next)
		dev->dirty = 0;

	ret = glob("/dev/iio:device*", 0, NULL, &devs);
	if (ret == GLOB_NOMATCH)
		goto done_device;
	if (ret)
		mylog(LOG_ERR, "glob devices: %s", ESTR(errno));
	/* open new devices */
	for (j = 0; j < devs.gl_pathc; ++j) {
		dev = find_iiodev(devs.gl_pathv[j]+5);
		if (dev) {
			dev->dirty = 1;
			continue;
		}
		if (destroy)
			continue;
		mylog(LOG_INFO, "probe %s", devs.gl_pathv[j]);
		/* device name */
		devname = devs.gl_pathv[j]+5;
		/* find human name */
		sprintf(filename, "/sys/bus/iio/devices/%s/name", devname);
		fd = ret = open(filename, O_RDONLY);
		if (fd < 0 && errno != ENOENT)
			humanname = devname;
		else if (fd < 0)
			mylog(LOG_ERR, "open %s: %s", filename, ESTR(errno));
		else {
			static char hname[128];

			ret = read(fd, hname, sizeof(hname)-1);
			if (ret < 0)
				mylog(LOG_ERR, "read %s: %s", filename, ESTR(errno));
			close(fd);
			hname[ret] = 0;
			if (ret && hname[ret-1] == '\n')
				hname[ret-1] = 0;
			humanname = hname;
		}

		/* verify if iio device is buffered
		 * I only handle buffered iio devices ...
		 * non-buffered devices can be handled like hwmon devices
		 */
		sprintf(filename, "/sys/bus/iio/devices/%s/buffer/enable", devname);
		if (access(filename, F_OK) < 0) {
			if (errno != ENOENT && errno != ENOTDIR)
				mylog(LOG_ERR, "access %s failed: %s", filename, ESTR(errno));
			mylog(LOG_INFO, "%s (%s) is not buffered, skipping", devname, humanname);
			continue;
		}

		/* create new device */
		dev = malloc(sizeof(*dev));
		if (!dev)
			mylog(LOG_ERR, "malloc iiodev: %s", ESTR(errno));
		memset(dev, 0, sizeof(*dev));
		dev->name = strdup(devname);
		dev->hname = strdup(humanname);
		dev->dirty = 2;

		/* open file */
		dev->fd = open(devs.gl_pathv[j], O_RDONLY | O_NONBLOCK);
		libe_add_fd(dev->fd, iiodev_data, dev);

		mylog(LOG_INFO, "%s (%s) new device", devname, humanname);
		/* insert in linked list */
		if (dev->fd < 0)
			mylog(LOG_ERR, "open %s: %s", devs.gl_pathv[j], ESTR(errno));
		dev->next = iiodevs;
		if (dev->next)
			dev->next->prev = dev;
		dev->prev = (struct iiodev *)&iiodevs; /* trickery */
		iiodevs = dev;
	}
	globfree(&devs);
done_device: ;

	/* close obsolete devices */
	struct iiodev *next;
	for (dev = iiodevs; dev; dev = next) {
		next = dev->next;
		if (!dev->dirty || destroy) {
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
	}

	/* grab scan elements for new devs */
	int dirlen, devnamelen;
	static const char dir[] = "/sys/bus/iio/devices/";
	char *elname;

	ret = glob("/sys/bus/iio/devices/iio:device*/scan_elements/in_*_en", 0, NULL, &els);
	if (ret == GLOB_NOMATCH)
		goto done_elements;
	if (ret)
		mylog(LOG_ERR, "glob scan_elements: %s", ESTR(errno));

	dirlen = strlen(dir);
	for (dev = iiodevs; dev; dev = dev->next) {
		if (dev->dirty != 2)
			continue;
		dev->dirty = 1;
		devnamelen = strlen(dev->name);
		/* find scan elements for this device */
		for (j = 0; j < els.gl_pathc; ++j) {
			if (strncmp(els.gl_pathv[j], dir, dirlen) ||
					strncmp(els.gl_pathv[j]+dirlen, dev->name, devnamelen) ||
					els.gl_pathv[j][dirlen+devnamelen] != '/')
				/* other device */
				continue;
			elname = strrchr(els.gl_pathv[j], '/');
			if (!elname)
				continue;
			/* skip '/in_' */
			elname += 4;
			/* pre-alloc room */
			if (dev->nels+1 > dev->sels) {
				dev->sels += 16;
				dev->els = realloc(dev->els, dev->sels*sizeof(*dev->els));
				if (!dev->els)
					mylog(LOG_ERR, "realloc %i elements: %s", dev->sels, ESTR(errno));
			}
			el = dev->els+dev->nels++;
			memset(el, 0, sizeof(*el));
			/* fill element */
			el->name = strndup(elname, strlen(elname)-3); /* strip _en from name */
			load_element(dev, el);
			mylog(LOG_INFO, "new channel (%s) %s:%s", dev->name, dev->hname, el->name);
		}
		/* sort by index */
		qsort(dev->els, dev->nels, sizeof(*dev->els), elementcmp);
		/* determine locaion (offsets in the byte stream) */
		for (el = dev->els; el < dev->els+dev->nels; ++el) {
			int mod;

			mod = dev->datsize % el->bytesused;
			if (mod)
				dev->datsize += el->bytesused - mod;
			el->location = dev->datsize;
			dev->datsize += el->bytesused;
		}
		/* prepare read buffer */
		dev->dat = malloc(dev->datsize);
		dev->olddat = malloc(dev->datsize);
		if (!dev->dat || !dev->olddat)
			mylog(LOG_ERR, "alloc %u dat for %s: %s", dev->datsize, dev->hname, ESTR(errno));
	}
	globfree(&els);
done_elements: ;
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
		libe_add_fd(mosquitto_socket(mosq), mqtt_fd_ready, mosq);
	}

	/* prepare epoll */
	scan_iio(0);

	/* core loop */
	while (1) {
		libt_flush();
		mqtt_update_flags(mosq);
		ret = libe_wait(libt_get_waittime());
		if (ret >= 0)
			libe_flush();
	}

	/* close all IIO */
	scan_iio(1);

	/* flush all data */
	for (it = items; it; it = it->next)
		mosquitto_publish(mosq, 0, it->topic, 0, NULL, mqtt_qos, 1);

	/* terminate */
#if 0
	send_self_sync(mosq, mqtt_qos);
	while (!ready) {
		mqtt_update_flags(mosq);
		libt_flush();
		ret = libe_wait(libt_get_waittime());
		if (ret >= 0)
			libe_flush();
	}
#endif
#if 0
	/* cleanup */
	mosquitto_disconnect(mosq);
	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();
#endif
	return 0;
}
