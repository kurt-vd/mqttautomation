#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <unistd.h>
#include <getopt.h>
#include <syslog.h>
#include <sys/signalfd.h>
#include <mosquitto.h>

#include "lib/libt.h"
#include "lib/libe.h"
#include "lib/liburi.h"
#include "common.h"

#define NAME "mqttbridge"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

#define MQTT_SUB_OPT_NO_LOCAL 0x04
#define MQTT_SUB_OPT_RETAIN_AS_PUBLISHED 0x08

/* program options */
static const char help_msg[] =
	NAME ": an MQTT bridge\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	"\n"
	" -l, --local=HOST[:PORT][/path] Specify local MQTT host+port and prefix (default: localhost)\n"
	" -h, --host=HOST[:PORT][/path] Specify remote MQTT host+port and prefix\n"
	" -i, --id=NAME		clientid prefix\n"
	"\n"
	"Parameters\n"
	" PATTERN	A pattern to subscribe for\n"
	;

#ifdef _GNU_SOURCE
static struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "local", required_argument, NULL, 'l', },
	{ "host", required_argument, NULL, 'h', },
	{ "id", required_argument, NULL, 'i', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?l:h:i:";

/* logging */
static int loglevel = LOG_WARNING;

/* signal handler */
static int sigterm;

/* MQTT parameters */
static const char *clientid_prefix;
struct host {
	const char *name;
	const char *host;
	int port;
	int keepalive;
	int qos;
	const char *prefix;
	int prefixlen;
	struct uri uri;

	struct host *remote;
	struct mosquitto *mosq;
	int req_mid;
	int ack_mid;
	int connected;
	struct host *peer;
};
extern struct host local, remote;

struct host local = {
	.name = "local",
	.host = "localhost",
	.port = 1883,
	.keepalive = 10,
	.qos = 1,
	.prefix = "bridge/",
	.prefixlen = 7,
	.peer = &remote,
}, remote = {
	.name = "remote",
	.port = 1883,
	.keepalive = 10,
	.qos = 1,
	.peer = &local,
};

#define myfree(x) ({ if (x) { free(x); (x) = NULL; }})

/* MQTT iface */
static void mqtt_log_cb(struct mosquitto *mosq, void *dat, int level, const char *str)
{
	struct host *h = dat;

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
			mylog(logpri_map[j+1], "[%s] %s", h->name, str);
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

	ret = mosquitto_publish(local.mosq, &local.req_mid, mqtt_log_levels[purelevel], strlen(payload), payload, 1, 0);
	if (ret < 0)
		mylog(LOG_ERR, "[%s] publish %s: %s", local.name, mqtt_log_levels[purelevel], mosquitto_strerror(ret));
}

/* reception */
static void mqtt_msg_cb(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	struct host *h = dat;
	char *topic;
	int ret;

	if (h->peer->prefixlen) {
		int topiclen = strlen(msg->topic) - h->prefixlen + h->peer->prefixlen;

		topic = malloc(topiclen+1);
		strcpy(topic, h->peer->prefix);
		strcpy(topic + h->peer->prefixlen, msg->topic + h->prefixlen);
	} else {
		topic = (char *)msg->topic + h->prefixlen;
	}

	ret = mosquitto_publish(h->peer->mosq, &h->peer->req_mid, topic, msg->payloadlen, msg->payload, msg->qos, msg->retain);
	if (ret < 0)
		mylog(LOG_ERR, "[%s] publish %s: %s", h->peer->name, topic, mosquitto_strerror(ret));

	if (h->peer->prefixlen)
		free(topic);
}

static void mqtt_pub_cb(struct mosquitto *mosq, void *dat, int mid)
{
	struct host *h = dat;

	h->ack_mid = mid;
}
static void mqtt_conn_cb(struct mosquitto *mosq, void *dat, int rc)
{
	struct host *h = dat;

	mylog(LOG_NOTICE, "[%s] connect: %i, %s", h->name, rc, mosquitto_connack_string(rc));
	h->connected = 1;
}
static void mqtt_disconn_cb(struct mosquitto *mosq, void *dat, int rc)
{
	struct host *h = dat;

	/* avoid waiting at the end test to succeed */
	h->ack_mid = h->req_mid;
	if (!h->connected) {
		mylog(LOG_WARNING, "[%s] disconnect before connect, verify your setup", h->name);
	} else {
		mylog(LOG_INFO, "[%s] disconnect: %i", h->name, rc);
	}
}

static void mqtt_maintenance(void *dat)
{
	struct host *h = dat;
	int ret;

	ret = mosquitto_loop_misc(h->mosq);
	if (ret)
		mylog(LOG_ERR, "mosquitto_loop_misc: %s", mosquitto_strerror(ret));
	libt_add_timeout(2.3, mqtt_maintenance, dat);
}

static void recvd_mosq(int fd, void *dat)
{
	struct host *h = dat;
	int evs = libe_fd_evs(fd);
	int ret;

	if (evs & LIBE_RD) {
		/* mqtt read ... */
		ret = mosquitto_loop_read(h->mosq, 1);
		if (ret)
			mylog(LOG_ERR, "mosquitto_loop_read: %s", mosquitto_strerror(ret));
	}
	if (evs & LIBE_WR) {
		/* flush mqtt write queue _after_ the timers have run */
		ret = mosquitto_loop_write(h->mosq, 1);
		if (ret)
			mylog(LOG_ERR, "mosquitto_loop_write: %s", mosquitto_strerror(ret));
	}
}

void mosq_update_flags(void)
{
	if (local.mosq)
		libe_mod_fd(mosquitto_socket(local.mosq), LIBE_RD | (mosquitto_want_write(local.mosq) ? LIBE_WR : 0));
	if (remote.mosq)
		libe_mod_fd(mosquitto_socket(remote.mosq), LIBE_RD | (mosquitto_want_write(remote.mosq) ? LIBE_WR : 0));
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

static void parse_url(const char *url, struct host *h)
{
	const char *str;

	lib_clean_uri(&h->uri);
	lib_parse_uri(url, &h->uri);

	if (h->uri.host)
		h->host = h->uri.host;
	if (h->uri.port)
		h->port = h->uri.port;
	if (h->uri.path)
		h->prefix = h->uri.path+1/* skip leading / */;
	else
		h->prefix = NULL;

	h->prefixlen = strlen(h->prefix ?: "");
	str = lib_uri_param(&h->uri, "keepalive");
	if (str)
		h->keepalive = strtol(str, NULL, 0);
	str = lib_uri_param(&h->uri, "qos");
	if (str)
		h->qos = strtol(str, NULL, 0);
}

static void setup_mqtt(struct host *h, const char *clientid, char *argv[])
{
	int ret;

	if (h->prefixlen && h->prefix[h->prefixlen-1] != '/')
		mylog(LOG_NOTICE, "[%s] prefix '%s' does not end in '/'", h->name, h->prefix);
	h->mosq = mosquitto_new(clientid, true, h);
	if (!h->mosq)
		mylog(LOG_ERR, "[%s] new: %s", h->name, ESTR(errno));

	mosquitto_message_callback_set(h->mosq, mqtt_msg_cb);
	mosquitto_publish_callback_set(h->mosq, mqtt_pub_cb);
	mosquitto_disconnect_callback_set(h->mosq, mqtt_disconn_cb);
	mosquitto_connect_callback_set(h->mosq, mqtt_conn_cb);
	mosquitto_log_callback_set(h->mosq, mqtt_log_cb);
	mosquitto_int_option(h->mosq, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V5);

	ret = mosquitto_connect(h->mosq, h->host, h->port, h->keepalive);
	if (ret)
		mylog(LOG_ERR, "[%s] connect %s:%i: %s", h->name, h->host, h->port, mosquitto_strerror(ret));

	libt_add_timeout(0, mqtt_maintenance, h);
	libe_add_fd(mosquitto_socket(h->mosq), recvd_mosq, h);

	int subopts = MQTT_SUB_OPT_NO_LOCAL | MQTT_SUB_OPT_RETAIN_AS_PUBLISHED;
	if (!argv || !*argv) {
		/* use 1 catch-all filter */
		argv = (char *[]){ "#", NULL, };
	}
	for (; *argv; ++argv) {
		char *subtopic;

		if (h->prefix)
			asprintf(&subtopic, "%s%s", h->prefix, *argv);
		else
			subtopic = *argv;
		ret = mosquitto_subscribe_v5(h->mosq, NULL, subtopic, h->qos, subopts, NULL);
		if (ret)
			mylog(LOG_ERR, "[%s] subscribe %s: %s", h->name, subtopic, mosquitto_strerror(ret));
		if (h->prefix)
			free(subtopic);
	}
}
static int mqtt_idle(struct host *h)
{
    if (!h->mosq || (h->req_mid != h->ack_mid))
        return 0;

    libe_remove_fd(mosquitto_socket(h->mosq));
    libt_remove_timeout(mqtt_maintenance, h);

    mosquitto_disconnect(h->mosq);
    mosquitto_destroy(h->mosq);
    h->mosq = NULL;
    mylog(LOG_INFO, "[%s] finished", h->name);
    return 1;
}

int main(int argc, char *argv[])
{
	int opt, ret;

	/* argument parsing */
	while ((opt = getopt_long(argc, argv, optstring, long_opts, NULL)) >= 0)
	switch (opt) {
	case 'V':
		fprintf(stderr, "%s %s\nCompiled on %s %s\n",
				NAME, VERSION, __DATE__, __TIME__);
		exit(0);
	case 'v':
		myloglevel(++loglevel);
		break;
	case 'l':
		parse_url(optarg, &local);
		break;
	case 'h':
		parse_url(optarg, &remote);
		break;
	case 'i':
		clientid_prefix = optarg;
		break;

	default:
		fprintf(stderr, "unknown option '%c'\n", opt);
	case '?':
		fputs(help_msg, stderr);
		exit(1);
		break;
	}

	myopenlog(NAME, 0, LOG_LOCAL2);

	if (!remote.host)
		mylog(LOG_ERR, "no host for bridging, add -h parameter");

	/* MQTT start */
	mosquitto_lib_init();
	char *clientid;
	asprintf(&clientid, "%s%s-%i", clientid_prefix ?: "", NAME, getpid());

	setup_mqtt(&local, clientid, argv+optind);
	setup_mqtt(&remote, clientid, argv+optind);

	/* prepare signalfd (turn off for debugging) */
	sigset_t sigmask;
	int sigfd;

	sigfillset(&sigmask);

	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0)
		mylog(LOG_ERR, "sigprocmask: %s", ESTR(errno));
	sigfd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sigfd < 0)
		mylog(LOG_ERR, "signalfd failed: %s", ESTR(errno));
	libe_add_fd(sigfd, signalrecvd, NULL);

	/* catch LOG_MQTT-marked logs via MQTT */
	mylogsethook(mqttloghook);

	/* core loop */
	for (; !sigterm; ) {
		libt_flush();
		mosq_update_flags();
		ret = libe_wait(libt_get_waittime());
		if (ret >= 0)
			libe_flush();
	}
	/* cleanup */
	mylog(LOG_INFO, "terminate");

	for (; local.mosq || remote.mosq;) {
		if (mqtt_idle(&remote))
			continue;
		if (mqtt_idle(&local))
			continue;
		libt_flush();
		mosq_update_flags();
		ret = libe_wait(libt_get_waittime());
		if (ret >= 0)
			libe_flush();
	}
	mosquitto_lib_cleanup();
	return 0;
}
