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
#include <wordexp.h>
#include <sys/stat.h>
#include <mosquitto.h>

#include "lib/libt.h"
#include "common.h"

#define NAME "mqttfile"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* program options */
static const char help_msg[] =
	NAME ": file-cache for MQTT topics\n"
	"usage:	" NAME " [OPTIONS ...]\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -h, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -C, --cd=PATH		store files under PATH (default /var/lib/mqttfile)\n"
	" -p, --prefix=PREFIX	listen to PREFIX/# (default 'file')\n"
	;

#ifdef _GNU_SOURCE
static struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "mqtt", required_argument, NULL, 'h', },
	{ "cd", required_argument, NULL, 'C', },
	{ "prefix", required_argument, NULL, 'p', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?h:C:p:";

/* logging */
static int loglevel = LOG_WARNING;

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;

static const char *mqtt_prefix = "file";
static const char *repo = "/var/lib/mqttfile";

/* state */
static struct mosquitto *mosq;

static const char *topicfmt(const char *fmt, ...)
{
	va_list va;
	static char *str;

	if (str)
		free(str);
	va_start(va, fmt);
	vasprintf(&str, fmt, va);
	va_end(va);
	return str;
}

__attribute__((unused))
static const char *payloadfmt(const char *fmt, ...)
{
	va_list va;
	static char *str;

	if (str)
		free(str);
	va_start(va, fmt);
	vasprintf(&str, fmt, va);
	va_end(va);
	return str;
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

static const char *const we_err[] = {
	[WRDE_BADCHAR] = "Illegal occurrence of newline or one of |, &, ;, <, >, (, ), {, }",
	[WRDE_BADVAL] = "An undefined shell variable was referenced, with WRDE_UNDEF",
	[WRDE_CMDSUB] = "Command substitution occurred with WRDE_NOCMD",
	[WRDE_NOSPACE] = "Out of memory",
	[WRDE_SYNTAX] = "Shell syntax error, such as unbalanced parentheses or unmatched quotes",
};

static char txt[2048];

static int my_mqtt_pub(const char *topic, const char *payload, int retain)
{
	int ret;

	ret = mosquitto_publish(mosq, NULL, topic, strlen(payload ?: ""), payload, mqtt_qos, retain);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", topic, mosquitto_strerror(ret));
	return ret;
}

static void my_mqtt_sub(const char *topic)
{
	int ret;

	ret = mosquitto_subscribe(mosq, NULL, topic, mqtt_qos);
	if (ret)
		mylog(LOG_ERR, "mosquitto_subscribe '%s': %s", topic, mosquitto_strerror(ret));
}

static int my_file_pub(const char *file)
{
	const char *path;
	int ret, fd;

	path = topicfmt("%s/%s", repo, file);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		mylog(LOG_WARNING, "open rd %s: %s", path, ESTR(errno));
		return -1;
	}
	ret = read(fd, txt, sizeof(txt)-1);
	close(fd);
	if (ret < 0) {
		mylog(LOG_WARNING, "read %s: %s", path, ESTR(errno));
		return -1;
	}
	/* null terminate */
	txt[ret] = 0;
	if (txt[ret-1] == '\n')
		txt[ret-1] = 0;
	return my_mqtt_pub(topicfmt("%s/%s", mqtt_prefix, file), txt, 1);
}
static int my_file_store(const char *file, const char *payload)
{
	const char *path;
	int ret, fd;

	path = topicfmt("%s/%s", repo, file);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		mylog(LOG_WARNING, "open wr %s: %s", path, ESTR(errno));
		return -1;
	}
	ret = write(fd, payload, strlen(payload ?: ""));
	close(fd);
	if (ret < 0) {
		mylog(LOG_WARNING, "write %s: %s", path, ESTR(errno));
		return -1;
	}
	return 0;
}

static int initial_pub(void)
{
	int ret, j;
	int repolen;
	char *path;
	wordexp_t we = {};
	const char *pattern;

	repolen = strlen(repo);
	pattern = topicfmt("%s/*", repo);
	mylog(LOG_NOTICE, "initial run on %s", pattern);
	ret = wordexp(pattern, &we, WRDE_NOCMD | WRDE_REUSE);
	if (ret) {
		mylog(LOG_WARNING, "'%s': %s", pattern, we_err[ret] ?: "uknonwn error");
		return -1;
	}
	for (j = 0; j < we.we_wordc; ++j) {
		path = we.we_wordv[j];
		if (stat(path, &(struct stat){}) != 0) {
			mylog(LOG_WARNING, "'%s' failed: %s", path, ESTR(errno));
			continue;
		}

		my_file_pub(path+repolen+1);
	}
	return 0;
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int topiclen = strlen(msg->topic ?: "");
	char *topic, *file, *payload;

	payload = (char *)msg->payload ?: "";

	if (!strcmp(msg->topic, "tools/loglevel")) {
		mysetloglevelstr(msg->payload);

	} else if (topiclen > 4 && !strcmp("/set", msg->topic + topiclen - 4)) {
		/* file/# pattern */
		topic = msg->topic;
		topic[topiclen-4] = 0;
		file = strrchr(topic, '/') ?: topic;
		if (*file == '/')
			++file;

		mylog(LOG_INFO, "update %s = '%s'", file, payload);
		if (my_file_store(file, payload) >= 0)
			my_mqtt_pub(topic, payload, 1);
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
	case 'h':
		mqtt_host = optarg;
		str = strrchr(optarg, ':');
		if (str > mqtt_host && *(str-1) != ']') {
			/* TCP port provided */
			*str = 0;
			mqtt_port = strtoul(str+1, NULL, 10);
		}
		break;
	case 'p':
		mqtt_prefix = optarg;
		break;
	case 'C':
		repo = optarg;
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

	my_mqtt_sub(topicfmt("tools/loglevel"));
	my_mqtt_sub(topicfmt("%s/+/set", mqtt_prefix));

	initial_pub();

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
