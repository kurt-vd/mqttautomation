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
#include <regex.h>
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
	" -c, --config=FILE	Load configuration from FILE\n"
	" -l, --local=HOST[:PORT][/path] Specify local MQTT host+port and prefix (default: localhost)\n"
	" -h, --host=HOST[:PORT][/path] Specify remote MQTT host+port and prefix\n"
	"			options:\n"
	"			qos=VALUE (default 1)\n"
	"			maxqos=VALUE (default 2)\n"
	"			retain=VALUE (default 1, set to 0 to turn off retain)\n"
	"			keepalive=VALUE (default 10)\n"
	"			proto=(5,4,3) (default 5)\n"
	"			cert=FILE for SSL\n"
	"			key=FILE for SSL\n"
	" -i, --id=NAME		clientid prefix\n"
	" -n, --dryrun		Do not really publich\n"
	" -C, --connection=TOPIC publish connection state to TOPIC\n"
	" -L REGEX		conflict resolution: prefer local\n"
	"			for topics matching REGEX\n"
	" -H REGEX		conflict resolution: prefer host\n"
	"			for topics matching REGEX\n"
	"\n"
	"Parameters\n"
	" PATTERN	A pattern to subscribe for\n"
	"\n"
	"Config file lines\n"
	" prefer (local|remote) REGEX\n"
	;

#ifdef _GNU_SOURCE
static struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "config", required_argument, NULL, 'c', },
	{ "local", required_argument, NULL, 'l', },
	{ "host", required_argument, NULL, 'h', },
	{ "id", required_argument, NULL, 'i', },
	{ "dryrun", required_argument, NULL, 'n', },
	{ "connection", required_argument, NULL, 'C', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?c:l:h:i:nC:L:H:";

/* logging */
static int loglevel = LOG_WARNING;

/* signal handler */
static int sigterm;

/* MQTT parameters */
static const char *clientid_prefix;
static int dryrun;
struct queue;
struct host {
	const char *name;
	const char *host;
	int port;
	uint8_t keepalive;
	uint8_t qos;
	uint8_t maxqos;
	uint8_t retain;
	uint8_t proto;
	const char *prefix;
	int prefixlen;
	struct uri uri;
	const char *conntopic;

	struct queue *q; /* next */
	struct queue *ql; /* prev */
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
	.maxqos = 2,
	.retain = 1,
	.proto = MQTT_PROTOCOL_V5,
	.prefix = "bridge/",
	.prefixlen = 7,
	.peer = &remote,
}, remote = {
	.name = "remote",
	.port = 1883,
	.keepalive = 10,
	.qos = 1,
	.maxqos = 2,
	.retain = 1,
	.proto = MQTT_PROTOCOL_V5,
	.peer = &local,
};

#define myfree(x) ({ if (x) { free(x); (x) = NULL; }})

/* conflict resolving */
struct prefer {
	struct prefer *next;
	struct host *host;
	regex_t regex;
};

static struct prefer *prefer, *preferlast;

static const char *regex_errstr(int regex_errno, regex_t *pregex)
{
	static char msg[256];

	regerror(regex_errno, pregex, msg, sizeof(msg));
	return msg;
}

static void add_prefer(struct host *host, const char *str)
{
	struct prefer *pr;

	if (!str)
		return;
	pr = malloc(sizeof(*pr));
	if (!pr)
		mylog(LOG_ERR, "malloc prefer: %s", ESTR(errno));
	memset(pr, 0, sizeof(*pr));

	pr->host = host;

	int ret = regcomp(&pr->regex, str, REG_NOSUB);
	if (ret)
		mylog(LOG_ERR, "regex '%s': %s", str, regex_errstr(ret, &pr->regex));
	if (preferlast)
		preferlast->next = pr;
	else
		/* first one in list */
		prefer = pr;
	preferlast = pr;
}

static struct host *resolve_conflict(const char *topic)
{
	struct prefer *pr;

	for (pr = prefer; pr; pr = pr->next) {
		if (!regexec(&pr->regex, topic, 0, NULL, 0))
			return pr->host;
	}
	return NULL;
}

/* cache */
static void mqtt_forward(struct host *h, const char *topic, int len, const void *dat, int qos, int retain);

static int forwarding;
struct cache {
	char *topic;
	struct cpayload {
		void *dat;
		uint16_t len;
		uint8_t qos;
		uint8_t retain;
	} lrecv, rrecv;
};

static struct cache *cachetable;
static int cachesize, cachefill;

static struct cache *find_cache(const char *topic, int create)
{
	struct cache *it;

	for (it = cachetable; it < cachetable+cachefill; ++it) {
		if (!strcmp(topic, it->topic))
			return it;
	}
	if (!create)
		return NULL;
	if (cachefill >= cachesize) {
		cachesize = cachesize*2 ?: 1024;
		cachetable = realloc(cachetable, sizeof(*cachetable)*cachesize);
		if (!cachetable)
			mylog(LOG_ERR, "realloc cache table %u: %s", cachesize, ESTR(errno));
	}
	/* 'it' may have been invalidated due to realloc, make it again */
	it = &cachetable[cachefill++];
	memset(it, 0, sizeof(*it));
	it->topic = strdup(topic);
	return it;
}

static int payloadcmp(const void *data, int lena, const void *datb, int lenb)
{
	if (!lena && !lenb)
		return 0;
	if (lena == lenb)
		return memcmp(data, datb, lena);
	if (lena < lenb)
		return memcmp(data, datb, lena) ?: -1;
	else
		return memcmp(data, datb, lenb) ?: +1;
}

static void start_forwarding(void *dat)
{
	struct cache *it;
	struct host *master;

	forwarding = 1;
	mylog(LOG_NOTICE, "start sync");

	for (it = cachetable; it < cachetable+cachefill; ++it) {
		if (!payloadcmp(it->lrecv.dat, it->lrecv.len, it->rrecv.dat, it->rrecv.len))
			/* no diff, no action */
			continue;

		if (it->rrecv.len && !it->lrecv.len)
			master = &remote;
		else if (!it->rrecv.len && it->lrecv.len)
			master = &local;
		else if (it->rrecv.len && it->lrecv.len) {
			master = resolve_conflict(it->topic);
			if (master)
				mylog(LOG_WARNING, "conflict on %s: use %s", it->topic, master->name);
		} else
			master = NULL;

		if (master == &remote)
			mqtt_forward(&local, it->topic, it->rrecv.len, it->rrecv.dat, it->rrecv.qos, it->rrecv.retain);

		else if (master == &local)
			mqtt_forward(&remote, it->topic, it->lrecv.len, it->lrecv.dat, it->lrecv.qos, it->lrecv.retain);

		else
			mylog(LOG_WARNING, "conflict on %s", it->topic);
		myfree(it->topic);
		myfree(it->lrecv.dat);
		myfree(it->rrecv.dat);
	}
	myfree(cachetable);
	cachetable = NULL;
	cachefill = cachesize = 0;
	mylog(LOG_NOTICE, "start forward");
}

/* echo cancelling: keep queue of outgoing msgs */
struct queue {
	struct queue *next;
	struct queue *prev;
	char *topic;
	void *dat;
	int len;
};

static void add_queue(struct host *h, const char *topic, const void *dat, int len)
{
	struct queue *q;

	int pktlen = sizeof(*q) + strlen(topic) + 1 + len + 1;
	q = malloc(pktlen);
	if (!q)
		mylog(LOG_ERR, "malloc q %u: %s", pktlen, ESTR(errno));
	memset(q, 0, sizeof(*q));
	q->len = len;
	q->dat = q+1;
	q->topic = ((char *)q->dat)+len;
	/* null-terminate payload too */
	*q->topic++ = 0;
	strcpy(q->topic, topic);
	memcpy(q->dat, dat, len);
	if (!h->q) {
		q->prev = (struct queue *)&h->q;
		h->q = h->ql = q;
	} else {
		q->prev = h->ql;
		q->prev->next = q;
	}
}

static int remove_queue(struct host *h, const char *topic, const void *dat, int len)
{
	struct queue *q;

	for (q = h->q; q; q = q->next) {
		if (!strcmp(topic, q->topic)) {
			if (!payloadcmp(dat, len, q->dat, q->len)) {
				q->prev->next = q->next;
				if (q->next)
					q->next->prev = q->prev;
				else
					h->ql = q->prev;
				free(q);
				return 1;
			}
			/* don't process further, we encountered the first in queue */
			return 0;
		}
	}
	return 0;
}

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
static void mqtt_forward(struct host *h, const char *topic, int len, const void *dat, int qos, int retain)
{
	char *topic2 = (char *)topic;
	int ret;

	if (dryrun) {
		mylog(LOG_NOTICE, "[%s] ... publish %s", h->name, topic);
		return;
	}

	if (h->prefixlen) {
		topic2 = malloc(strlen(topic) + h->prefixlen + 1);
		strcpy(topic2, h->prefix);
		strcpy(topic2 + h->prefixlen, topic);
	}
	if (qos > h->maxqos)
		qos = h->maxqos;
	if (retain)
		retain = h->retain;

	mylog(LOG_INFO, "[%s] forward %s", h->name, topic);
	ret = mosquitto_publish(h->mosq, &h->req_mid, topic2, len, dat, qos, retain);
	if (ret < 0)
		mylog(LOG_ERR, "[%s] publish %s: %s", h->name, topic2, mosquitto_strerror(ret));

	if (h->prefixlen)
		free(topic2);
	if (h->proto < MQTT_PROTOCOL_V5)
		add_queue(h, topic, dat, len);
}

static void mqtt_msg_cb(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	struct host *h = dat;

	if (!forwarding) {
		struct cache *c;
		struct cpayload *cp;

		c = find_cache(msg->topic + h->prefixlen, 1);
		cp = (h == &local) ? &c->lrecv : &c->rrecv;

		myfree(cp->dat);
		cp->len = msg->payloadlen;
		cp->dat = malloc(cp->len + 1);
		memcpy(cp->dat, msg->payload, cp->len);
		((uint8_t *)cp->dat)[cp->len] = 0;
		cp->qos = msg->qos;
		cp->retain = msg->retain;
		return;
	}

	if (remove_queue(h, msg->topic + h->prefixlen, msg->payload, msg->payloadlen)) {
		/* was echo, no more processing */
		mylog(LOG_INFO, "[%s] cancel echo for %s", h->name, msg->topic + h->prefixlen);
		return;
	}

	mqtt_forward(h->peer, msg->topic + h->prefixlen, msg->payloadlen, msg->payload, msg->qos, msg->retain);
}

static void mqtt_pub_conntopic(struct host *h, const char *value)
{
	int ret;

	if (h->conntopic) {
		if (dryrun) {
			mylog(LOG_NOTICE, "[%s] publish %s = %s", h->name, h->conntopic, value);
			return;
		}

		ret = mosquitto_publish(h->mosq, &h->req_mid, h->conntopic, strlen(value ?: ""), value, 1, 1);
		if (ret)
			mylog(LOG_ERR, "[%s] publish %s: %s", h->name, h->conntopic, mosquitto_strerror(ret));
	}
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
	mqtt_pub_conntopic(h->peer, "1");

	if (h->peer->connected)
		libt_add_timeout(1, start_forwarding, NULL);
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
	mqtt_pub_conntopic(h->peer, "0");
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
	str = lib_uri_param(&h->uri, "maxqos");
	if (str)
		h->maxqos = strtol(str, NULL, 0);
	str = lib_uri_param(&h->uri, "retain");
	if (str)
		h->retain = strtol(str, NULL, 0);
	str = lib_uri_param(&h->uri, "proto");
	if (str) {
		h->proto = strtoul(str, NULL, 0);
		if (h->proto < MQTT_PROTOCOL_V31 || h->proto > MQTT_PROTOCOL_V5)
			mylog(LOG_ERR, "protocol v%u unsupported", h->proto);
	}
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
	mosquitto_int_option(h->mosq, MOSQ_OPT_PROTOCOL_VERSION, h->proto);
	mylog(LOG_NOTICE, "[%s] proto %u", h->name, h->proto);
#ifdef MOSQ_OPT_PRIVATE
	if (h->proto < 5) {
		ret = mosquitto_int_option(h->mosq, MOSQ_OPT_PRIVATE, 0x80);
		if (ret)
			mylog(LOG_WARNING, "[%s] private: %s", h->name, mosquitto_strerror(ret));
		else {
			mylog(LOG_NOTICE, "[%s] private ok", h->name);
		}
	}
#endif

	if (h->conntopic && !dryrun) {
		ret = mosquitto_will_set(h->mosq, h->conntopic, 4, "lost", 1, 1);
		if (ret)
			mylog(LOG_ERR, "mosquitto_will_set: %s", mosquitto_strerror(ret));
	}

	const char *cert, *key;
	cert = lib_uri_param(&h->uri, "cert");
	key = lib_uri_param(&h->uri, "key");
	if (cert && key) {
		ret = mosquitto_int_option(h->mosq, MOSQ_OPT_TLS_USE_OS_CERTS, 1);
		if (ret)
			mylog(LOG_ERR, "mosquitto_int_option TLS_USE_OS_CERTS 1: %s", mosquitto_strerror(ret));
		ret = mosquitto_tls_set(h->mosq, NULL, NULL, cert, key, NULL);
		if (ret)
			mylog(LOG_ERR, "mosquitto_tls_set %s %s: %s", cert, key, mosquitto_strerror(ret));
	} else if (cert) {
		mylog(LOG_ERR, "[%s] cert %s, no key", h->name, cert);
	} else if (key) {
		mylog(LOG_ERR, "[%s] key %s, no cert", h->name, key);
	}

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
	mqtt_pub_conntopic(h, "0");
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

static void load_config(const char *file)
{
	FILE *fp;
	char *line = NULL, *tok;
	size_t linesize = 0;
	int ret;

	if (!strcmp(file, "-")) {
		fp = stdin;
	} else {
		fp = fopen(file, "r");
		if (!fp)
			mylog(LOG_ERR, "fopen %s r: %s", file, ESTR(errno));
	}

	for (;;) {
		ret = getline(&line, &linesize, fp);
		if (ret <= 0) {
			if (feof(fp))
				break;
			mylog(LOG_ERR, "getline %s: %s", file, ESTR(errno));
		}
		if (*line == '#')
			continue;
		static const char sep[] = " \t\r\n\v\f";
		tok = strtok(line, sep);
		if (!tok || !*tok) {
			continue;
		} else if (!strcmp(tok, "prefer")) {
			tok = strtok(NULL, sep) ?: "";
			if (!strcmp(tok, "local")) {
				add_prefer(&local, strtok(NULL, sep));
			} else if (!strcmp(tok, "remote")) {
				add_prefer(&remote, strtok(NULL, sep));
			} else {
				mylog(LOG_WARNING, "%s: prefer %s: unsupported", file, tok);
			}
		} else {
			mylog(LOG_WARNING, "%s: %s: unsupported", file, tok);
		}
	}

	fclose(fp);
	if (line)
		free(line);
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
	case 'n':
		dryrun = 1;
	case 'C':
		local.conntopic = optarg;
		break;
	case 'L':
		add_prefer(&local, optarg);
		break;
	case 'H':
		add_prefer(&remote, optarg);
		break;
	case 'c':
		load_config(optarg);
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
	mqtt_pub_conntopic(&local, "0");

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
