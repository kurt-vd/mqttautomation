#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>

#include "lib/libt.h"
#include "rpnlogic.h"
#include "astronomics.h"
#include "common.h"

static const struct rpn_el dummy = { .a = NULL, .d = NAN, };
/* placeholder for quitting */
#define QUIT	((struct rpn *)0xdeadbeef)
/* manage */
static struct rpn *rpn_create(void)
{
	struct rpn *rpn;

	rpn = malloc(sizeof(*rpn));
	if (!rpn)
		mylog(LOG_ERR, "malloc failed?");
	memset(rpn, 0, sizeof(*rpn));
	rpn->value = NAN;
	return rpn;
}

static inline void *rpn_priv(struct rpn *rpn)
{
	return rpn+1;
}

/* grow private date of rpn */
static struct rpn *rpn_grow(struct rpn *rpn, int privsize)
{
	rpn = realloc(rpn, sizeof(*rpn) + privsize);
	if (!rpn)
		mylog(LOG_ERR, "realloc rpn %lu", (long)sizeof(*rpn)+privsize);
	memset(rpn+1, 0, privsize);
	return rpn;
}

static void free_lookup(struct rpn *rpn);
static void rpn_free(struct rpn *rpn)
{
	free_lookup(rpn);
	if (rpn->topic)
		free(rpn->topic);
	if (rpn->constvalue)
		free(rpn->constvalue);
	if (rpn->timeout)
		libt_remove_timeout(rpn->timeout, rpn);
	free(rpn);
}

void rpn_free_chain(struct rpn *rpn)
{
	struct rpn *tmp;

	while (rpn) {
		tmp = rpn;
		rpn = rpn->next;
		rpn_free(tmp);
	}
}

static inline int rpn_toint(double val)
{
	/* provide a NAN-safe int conversion */
	if (isnan(val))
		return 0;
	else
		return (int)val;
}

static inline int dblcmp(double a, double b, double diff)
{
	if (isnan(a) && isnan(b))
		return 0;
	else if (isnan(a))
	       return -1;
	else if (isnan(b))
	       return 1;

	else if (fpclassify(a) == FP_ZERO && fpclassify(b) == FP_ZERO)
		/* avoid /0 */
		return 0;
	else if (fabs(2*(a-b)/(a+b)) < diff)
		return 0;
	else if (a < b)
		return -1;
	else
		return 1;
}

static void rpn_reserve(struct stack *st)
{
	if (st->n >= st->s) {
		st->s += 16;
		st->v = realloc(st->v, st->s * sizeof(st->v[0]));
		if (!st->v)
			mylog(LOG_ERR, "realloc stack %u failed", st->s);
	}
}

static void rpn_push_el(struct stack *st, const struct rpn_el *el)
{
	rpn_reserve(st);
	st->v[st->n++] = *el;
}

static void rpn_push_str(struct stack *st, const char *str, double value)
{
	rpn_reserve(st);
	st->v[st->n].a = str;
	st->v[st->n++].d = value;
}
static inline void rpn_push(struct stack *st, double value)
{
	rpn_push_str(st, NULL, value);
}
static struct rpn_el *rpn_pop1(struct stack *st)
{
	if (st->n < 1) {
		st->errnum = ECANCELED;
		st->n = 0;
		return (struct rpn_el *)&dummy;
	} else {
		st->n -= 1;
		return st->v+st->n;
	}
}
static void rpn_pop(struct stack *st, int n)
{
	if (st->n < n) {
		st->errnum = ECANCELED;
		st->n = 0;
	} else
		st->n -= n;
}
static inline struct rpn_el *rpn_n(struct stack *st, int idx)
{
	if (idx >= 0 || -idx > st->n) {
		st->errnum = ECANCELED;
		return (struct rpn_el *)&dummy;
	}
	return st->v+st->n+idx;
}

/* extract JSON */
#include "jsmn/jsmn.h"
static int match_tok(struct rpn *me, const char *needle, const char *json, jsmntok_t *t, const char *topic)
{
	int ntok, j, matched __attribute__((unused));
	char *newtopic;
	size_t len;

	switch (t->type) {
	case JSMN_PRIMITIVE:
	case JSMN_STRING:
		if (!strcmp(needle, topic)) {
			/* matched */
			if (me->constvalue)
				free(me->constvalue);
			me->constvalue = strndup(json+t->start, t->end - t->start);
			matched = 1;
		} else {
			matched = 0;
		}
		//printf("%s\t%c\t%.*s\n", topic, matched ? '*' : ' ', t->end - t->start, json + t->start);
		return 1;
	case JSMN_OBJECT:
		newtopic = NULL;
		len = 0;
		ntok = 1;
		for (j = 0; j < t->size; ++j) {
			if (t[ntok].type != JSMN_STRING)
				mylog(LOG_ERR, "property with non-string name");
			if (strlen(topic) + t[ntok].end - t[ntok].start + 1 > len) {
				len = (strlen(topic) + t[ntok].end - t[ntok].start + 1 + 128) & ~127;
				newtopic = realloc(newtopic, len);
				if (!newtopic)
					mylog(LOG_ERR, "realloc %li: %s", (long)len, ESTR(errno));
			}
			sprintf(newtopic, "%s%s%.*s", topic, topic[0] ? "/" : "", t[ntok].end - t[ntok].start, json + t[ntok].start);
			++ntok;
			ntok += match_tok(me, needle, json, t+ntok, newtopic);
		}
		if (newtopic)
			free(newtopic);
		return ntok;
	case JSMN_ARRAY:
		newtopic = malloc(strlen(topic) + 16);
		ntok = 1;
		for (j = 0; j < t->size; ++j) {
			sprintf(newtopic, "%s/%i", topic, j);
			ntok += match_tok(me, needle, json, t+ntok, newtopic);
		}
		free(newtopic);
		return ntok;
	default:
		return 0;
	}
}

static void rpn_do_json(struct stack *st, struct rpn *me)
{
	const char *member = rpn_pop1(st)->a;
	const char *json = rpn_pop1(st)->a;

	if (!json || !member)
		goto failed;

	/* start parsing */
	jsmn_parser prs;
	static jsmntok_t *tok;
	static size_t tokcnt;
	int len, ret;

	len = strlen(json);
	jsmn_init(&prs);
	ret = jsmn_parse(&prs, json, len, NULL, 0);
	if (ret < 0) {
		st->errnum = -ret;
		goto failed;
	}

	jsmn_init(&prs);
	if (ret > tokcnt) {
		tokcnt = (ret + 7) & ~7;
		tok = realloc(tok, sizeof(*tok)*tokcnt);
	}
	ret = jsmn_parse(&prs, json, len, tok, tokcnt);
	if (ret < 0) {
		st->errnum = -ret;
		goto failed;
	}
	if (me->constvalue)
		free(me->constvalue);
	me->constvalue = NULL;
	match_tok(me, member, json, tok, "");
	rpn_push_str(st, me->constvalue, mystrtod(me->constvalue ?: "nan", NULL));
	return;

failed:
	rpn_push_str(st, "", NAN);
}

/* algebra */
static void rpn_do_plus(struct stack *st, struct rpn *me)
{
	double tmp = rpn_n(st, -2)->d + rpn_n(st, -1)->d;
	rpn_pop(st, 2);
	rpn_push(st, tmp);
}
static void rpn_do_minus(struct stack *st, struct rpn *me)
{
	double tmp = rpn_n(st, -2)->d - rpn_n(st, -1)->d;
	rpn_pop(st, 2);
	rpn_push(st, tmp);
}
static void rpn_do_mul(struct stack *st, struct rpn *me)
{
	double tmp = rpn_n(st, -2)->d * rpn_n(st, -1)->d;
	rpn_pop(st, 2);
	rpn_push(st, tmp);
}
static void rpn_do_div(struct stack *st, struct rpn *me)
{
	double tmp = rpn_n(st, -2)->d / rpn_n(st, -1)->d;
	rpn_pop(st, 2);
	rpn_push(st, tmp);
}
static void rpn_do_mod(struct stack *st, struct rpn *me)
{
	double tmp = fmod(rpn_n(st, -2)->d, rpn_n(st, -1)->d);
	rpn_pop(st, 2);
	rpn_push(st, tmp);
}
static void rpn_do_pow(struct stack *st, struct rpn *me)
{
	double tmp = pow(rpn_n(st, -2)->d, rpn_n(st, -1)->d);
	rpn_pop(st, 2);
	rpn_push(st, tmp);
}
static void rpn_do_negative(struct stack *st, struct rpn *me)
{
	rpn_push(st, -rpn_pop1(st)->d);
}

static void rpn_do_min(struct stack *st, struct rpn *me)
{
	struct rpn_el *a = rpn_pop1(st);
	struct rpn_el *b = rpn_pop1(st);

	rpn_push_el(st, (dblcmp(a->d, b->d, 1e-9) < 0) ? a : b);
}

static void rpn_do_max(struct stack *st, struct rpn *me)
{
	struct rpn_el *a = rpn_pop1(st);
	struct rpn_el *b = rpn_pop1(st);

	rpn_push_el(st, (dblcmp(a->d, b->d, 1e-9) > 0) ? a : b);
}

/* utilities */
static void rpn_do_limit(struct stack *st, struct rpn *me)
{
	struct rpn_el *dut = rpn_n(st, -3);
	struct rpn_el *min = rpn_n(st, -2);
	struct rpn_el *max = rpn_n(st, -1);

	/* limit x-3 between x-2 & x-1 */
	if (dut->d < min->d)
		*dut = *min;
	else if (dut->d > max->d)
		*dut = *max;
	rpn_pop(st, 2);
}

static void rpn_do_inrange(struct stack *st, struct rpn *me)
{
	int result;
	struct rpn_el *dut = rpn_n(st, -3);
	struct rpn_el *min = rpn_n(st, -2);
	struct rpn_el *max = rpn_n(st, -1);

	/* test x-3 between x-2 & x-1 */
	if (min->d < max->d)
		/* regular case: DUT min max */
		result = dut->d >= min->d && dut->d <= max->d;
	else if (min->d > max->d)
		/* inverted case: DUT max min */
		result = dut->d >= min->d || dut->d <= max->d;
	else
		result = 0;
	rpn_pop(st, 3);
	rpn_push(st, result);
}

static void rpn_do_category(struct stack *st, struct rpn *me)
{
	struct rpn_el *val = rpn_n(st, -2);
	int ncat = rpn_toint(rpn_n(st, -1)->d);
	double value = rpn_toint(val->d * ncat);
	if (value < 0)
		value = 0;
	else if (value > (ncat-1))
		value = ncat-1;
	rpn_pop(st, 2);
	rpn_push(st, value);
}

static void rpn_do_ramp3(struct stack *st, struct rpn *me)
{
	double value = rpn_n(st, -4)->d;
	double lo = rpn_n(st, -3)->d;
	double hi = rpn_n(st, -2)->d;
	double step = rpn_n(st, -1)->d;

	rpn_pop(st, 4);

	value = (value - lo) / (hi - lo);
	/* make discrete steps */
	value = round(value / step)*step;
	/* limit */
	if (value < 0)
		value = 0;
	else if (value > 1)
		value = 1;
	rpn_push(st, value);
}

static void rpn_do_hyst2(struct stack *st, struct rpn *me)
{
	struct rpn_el *dut = rpn_n(st, -3);
	struct rpn_el *lo = rpn_n(st, -2);
	struct rpn_el *hi = rpn_n(st, -1);

	if (dut->d > hi->d)
		me->cookie = 1;
	else if (dut->d < lo->d)
		me->cookie = 0;
	rpn_pop(st, 3);
	rpn_push(st, me->cookie);
}

static void rpn_do_hyst1(struct stack *st, struct rpn *me)
{
	struct rpn_el *dut = rpn_n(st, -3);
	struct rpn_el *setp = rpn_n(st, -2);
	struct rpn_el *marg = rpn_n(st, -1);

	if (dut->d > setp->d+marg->d)
		me->cookie = 1;
	else if (dut->d < setp->d-marg->d)
		me->cookie = 0;
	rpn_pop(st, 3);
	rpn_push(st, me->cookie);
}

/* statistics */
struct avgtime {
	double sum;
	double n;
	double out;
	int started;
	double last_in;
	double last_t;
	int newperiod;
};

static void on_avgtime_period(void *dat)
{
	struct rpn *me = dat;
	struct avgtime *avg = rpn_priv(me);

	avg->newperiod = 1;
	rpn_run_again(me);
}

static void rpn_do_avgtime(struct stack *st, struct rpn *me)
{
	struct avgtime *avg = rpn_priv(me);
	double v, period, stabletime;
	double now, next;

	period = rpn_pop1(st)->d;
	v = rpn_pop1(st)->d;

	now = libt_now();

	if (avg->started) {
		stabletime = now - avg->last_t;
		avg->sum += avg->last_in * stabletime;
		avg->n += stabletime;

		if (avg->newperiod) {
			/* new period starts here */
			avg->out = avg->sum / avg->n;
			avg->sum = avg->n = 0;
			libt_remove_timeout(on_avgtime_period, me);

		} else {
			/* schedule summary on next period */
			next = period - fmod(walltime(), period);
			libt_add_timeout(next, on_avgtime_period, me);
			me->timeout = on_avgtime_period;
		}
	} else {
		/* forward input immediately */
		avg->out = v;
		avg->sum = avg->n = 0;
	}

	/* mark next value */
	avg->last_in = v;
	avg->last_t = now;

	avg->newperiod = 0;
	avg->started = 1;
	rpn_push(st, avg->out);
}

struct running {
	struct sample {
		double t;
		double v;
	} *table;
	int tsize, tfill, told;
};

static void free_running(struct rpn *me)
{
	struct running *run = rpn_priv(me);

	if (run->table)
		free(run->table);
}

static void rpn_collect_running(struct rpn *me, double now, double period, double value)
{
	struct running *run = rpn_priv(me);
	double from;
	int j;

	from = now - period;

	/* clean history */
	for (j = run->told; j < run->tfill; ++j) {
		if (run->table[j].t > from)
			break;
	}
	if (j > run->told)
		--j;
	run->told = j;

	/* add storage */
	if (run->tfill >= run->tsize && run->told) {
		memmove(run->table, run->table+run->told,
				(run->tfill-run->told)*sizeof(run->table[0]));
		run->tfill -= run->told;
		run->told = 0;
	}
	if (run->tfill >= run->tsize) {
		run->tsize = run->tsize*2 ?: 32;
		run->table = realloc(run->table, sizeof(run->table[0])*run->tsize);
	}

	run->table[run->tfill].t = now;
	run->table[run->tfill].v = value;
	++run->tfill;
}

static void rpn_do_running_avg(struct stack *st, struct rpn *me)
{
	struct running *run = rpn_priv(me);
	double v, period;
	double now;
	int j;
	double sum;

	period = rpn_pop1(st)->d;
	v = rpn_pop1(st)->d;
	now = libt_now();

	rpn_collect_running(me, now, period, v);

	sum = 0;
	for (j = run->told+1; j < run->tfill; ++j) {
		if (!isnan(run->table[j-1].v))
			sum += (run->table[j].t - run->table[j-1].t) * run->table[j-1].v;
	}
	/* append current slice */
	if (!isnan(run->table[j-1].v))
		sum += (now - run->table[j-1].t) * run->table[j-1].v;

	rpn_push(st, sum/(now - run->table[run->told].t));
}

static void rpn_do_running_min(struct stack *st, struct rpn *me)
{
	struct running *run = rpn_priv(me);
	double v, period;
	double now;
	int j;

	period = rpn_pop1(st)->d;
	v = rpn_pop1(st)->d;

	now = libt_now();

	rpn_collect_running(me, now, period, v);

	v = run->table[run->told].v;
	for (j = run->told+1; j < run->tfill; ++j) {
		if (!(run->table[j].v > v))
			v = run->table[j].v;
	}

	rpn_push(st, v);
}

static void rpn_do_running_max(struct stack *st, struct rpn *me)
{
	struct running *run = rpn_priv(me);
	double v, period;
	double now;
	int j;

	period = rpn_pop1(st)->d;
	v = rpn_pop1(st)->d;

	now = libt_now();

	rpn_collect_running(me, now, period, v);

	v = run->table[run->told].v;
	for (j = run->told+1; j < run->tfill; ++j) {
		if (!(run->table[j].v < v))
			v = run->table[j].v;
	}

	rpn_push(st, v);
}

struct slope {
	double out;
	double setpoint;
	double step;
	double delay;
	int timer;
	int busy;
	double *pos;
	int npos, spos;
};
static void on_slope_step(void *dat);

static void free_slope(struct rpn *me)
{
	struct slope *priv = rpn_priv(me);

	if (priv->pos)
		free(priv->pos);
}

static void slope_test_final(void *dat, int dir)
{
	struct rpn *me = dat;
	struct slope *priv = rpn_priv(me);

	if (!dblcmp(priv->out, priv->setpoint, 0.01)) {
		priv->busy = 0;
	} else if ((dir < 0 && priv->out < priv->setpoint)
		|| (dir > 0 && priv->out > priv->setpoint)) {
		priv->out = priv->setpoint;
		priv->busy = 0;
	} else {
		libt_add_timeout(priv->delay, on_slope_step, me);
	}
}

static void slope_step(void *dat)
{
	struct rpn *me = dat;
	struct slope *priv = rpn_priv(me);
	double step;
	int dir;
	int j;

	if (priv->pos && (priv->setpoint < priv->out)) {
		for (j = priv->npos-1; j >= 0; --j) {
			if (priv->pos[j] < priv->out) {
				priv->out = priv->pos[j];
				/* test for end decreasing */
				slope_test_final(dat, -1);
				return;
			}
		}
		priv->out = priv->pos[0];
		priv->busy = 0;

	} else if (priv->pos && (priv->setpoint > priv->out)) {
		for (j = 0; j < priv->npos; ++j) {
			if (priv->pos[j] > priv->out) {
				priv->out = priv->pos[j];
				/* test for end increasing */
				slope_test_final(dat, +1);
				return;
			}
		}
		priv->out = priv->pos[priv->npos-1];
		priv->busy = 0;

	} else if (priv->pos) {
		priv->busy = 0;

	} else if (!priv->pos) {
		step = priv->step;

		if (priv->setpoint > priv->out)
			/* increase */
			dir = +1;
		else
			/* decrease */
			dir = -1;

		/* avoid floating point small deviations */
		priv->out = round((priv->out + step*dir)/step)*step;
		//priv->out += step*dir;
		slope_test_final(dat, dir);
	}
}

static void on_slope_step(void *dat)
{
	struct rpn *me = dat;
	struct slope *priv = rpn_priv(me);

	priv->timer = 1;
	rpn_run_again(dat);
}

static void parse_slope(struct rpn *me, char *str)
{
	struct slope *priv = rpn_priv(me);
	char *tok;

	if (!str)
		return;
	for (tok = strtok(str, ","); tok; tok = strtok(NULL, ",")) {
		if (priv->npos >= priv->spos) {
			priv->spos += 16;
			priv->pos = realloc(priv->pos, sizeof(*priv->pos)*priv->spos);
		}
		priv->pos[priv->npos++] = mystrtod(tok, NULL);
	}
}

static void rpn_do_slope(struct stack *st, struct rpn *me)
{
	struct slope *priv = rpn_priv(me);
	double curr, step, delay;

	delay = rpn_pop1(st)->d;
	step = rpn_pop1(st)->d;
	curr = rpn_pop1(st)->d;
	priv->setpoint = rpn_pop1(st)->d;

	if (!priv->busy && dblcmp(curr, priv->setpoint, 0.005)) {
		priv->out = curr;
		priv->step = step;
		priv->delay = delay;

		if (isnan(priv->out))
			priv->out = 0;

		/* run slope */
		priv->busy = 1;
		priv->timer = 1;
		me->timeout = on_slope_step;
	}

	if (priv->timer) {
		priv->timer = 0;
		slope_step(me);
	}

	rpn_push(st, priv->out);
}

/* bitwise */
static void rpn_do_bitand(struct stack *st, struct rpn *me)
{
	struct rpn_el *a = rpn_n(st, -2);
	struct rpn_el *b = rpn_n(st, -1);

	rpn_pop(st, 2);
	rpn_push(st, rpn_toint(a->d) & rpn_toint(b->d));
}
static void rpn_do_bitor(struct stack *st, struct rpn *me)
{
	struct rpn_el *a = rpn_n(st, -2);
	struct rpn_el *b = rpn_n(st, -1);

	rpn_pop(st, 2);
	rpn_push(st, rpn_toint(a->d) | rpn_toint(b->d));
}
static void rpn_do_bitxor(struct stack *st, struct rpn *me)
{
	struct rpn_el *a = rpn_n(st, -2);
	struct rpn_el *b = rpn_n(st, -1);

	rpn_pop(st, 2);
	rpn_push(st, rpn_toint(a->d) ^ rpn_toint(b->d));
}
static void rpn_do_bitinv(struct stack *st, struct rpn *me)
{
	rpn_push(st, ~rpn_toint(rpn_pop1(st)->d));
}

/* boolean */
static void rpn_do_booland(struct stack *st, struct rpn *me)
{
	struct rpn_el *a = rpn_n(st, -2);
	struct rpn_el *b = rpn_n(st, -1);

	rpn_pop(st, 2);
	rpn_push(st, rpn_toint(a->d) && rpn_toint(b->d));
}
static void rpn_do_boolor(struct stack *st, struct rpn *me)
{
	struct rpn_el *a = rpn_n(st, -2);
	struct rpn_el *b = rpn_n(st, -1);

	rpn_pop(st, 2);
	rpn_push(st, rpn_toint(a->d) || rpn_toint(b->d));
}
static void rpn_do_boolnot(struct stack *st, struct rpn *me)
{
	rpn_push(st, !rpn_toint(rpn_pop1(st)->d));
}
static void rpn_do_intequal(struct stack *st, struct rpn *me)
{
	struct rpn_el *a = rpn_n(st, -2);
	struct rpn_el *b = rpn_n(st, -1);
	int result;

	if (a->a && b->a)
		result = !strcasecmp(a->a, b->a);
	else
		result = rpn_toint(a->d) == rpn_toint(b->d);
	rpn_pop(st, 2);
	rpn_push(st, result);
}
static void rpn_do_intnotequal(struct stack *st, struct rpn *me)
{
	rpn_do_intequal(st, me);
	rpn_push(st, !rpn_toint(rpn_pop1(st)->d));
}

/* compare */
static void rpn_do_lt(struct stack *st, struct rpn *me)
{
	struct rpn_el *a = rpn_n(st, -2);
	struct rpn_el *b = rpn_n(st, -1);

	rpn_pop(st, 2);
	rpn_push(st, a->d < b->d);
}
static void rpn_do_gt(struct stack *st, struct rpn *me)
{
	struct rpn_el *a = rpn_n(st, -2);
	struct rpn_el *b = rpn_n(st, -1);

	rpn_pop(st, 2);
	rpn_push(st, a->d > b->d);
}

/* generic */
static void rpn_do_const(struct stack *st, struct rpn *me)
{
	rpn_push_str(st, me->constvalue, me->value);
}

static void rpn_do_env(struct stack *st, struct rpn *me)
{
	const char *str = rpn_lookup_env(me->topic, me);
	rpn_push_str(st, str, mystrtod(str ?: "nan", NULL));
}

static void rpn_do_writeenv(struct stack *st, struct rpn *me)
{
	struct rpn_el *val = rpn_pop1(st);
	rpn_write_env(val->a ?: mydtostr(val->d), me->topic, me);
}

static void rpn_do_dup(struct stack *st, struct rpn *me)
{
	rpn_push_el(st, rpn_n(st, -1));
}
static void rpn_do_swap(struct stack *st, struct rpn *me)
{
	struct rpn_el a = *rpn_pop1(st);
	struct rpn_el b = *rpn_pop1(st);

	rpn_push_el(st, &a);
	rpn_push_el(st, &b);
}
static void rpn_do_ifthenelse(struct stack *st, struct rpn *me)
{
	struct rpn_el *f = rpn_pop1(st); /* false */
	struct rpn_el *t = rpn_pop1(st); /* true */
	struct rpn_el *c = rpn_pop1(st); /* condition */

	rpn_push_el(st, rpn_toint(c->d) ? t : f);
}

/* timer functions */
static void on_delay(void *dat)
{
	struct rpn *me = dat;

	/* clear output */
	me->cookie ^= 1;
	rpn_run_again(me);
}
static void rpn_do_offdelay(struct stack *st, struct rpn *me)
{
	struct rpn_el *delay = rpn_pop1(st);
	struct rpn_el *input = rpn_pop1(st);
	int inval = rpn_toint(input->d);

	if (!inval && (me->cookie & 2)) {
		/* falling edge: schedule timeout */
		libt_add_timeout(delay->d, on_delay, me);
		me->timeout = on_delay;
		/* clear cache */
		me->cookie &= ~2;
	} else if (inval && !(me->cookie & 2)) {
		/* rising edge: cancel timeout */
		libt_remove_timeout(on_delay, me);
		/* set cache & output */
		me->cookie = 2+1;
	}
	/* write output to stack */
	rpn_push(st, me->cookie & 1);
}
static void rpn_do_afterdelay(struct stack *st, struct rpn *me)
{
	struct rpn_el *delay = rpn_pop1(st);
	struct rpn_el *input = rpn_pop1(st);
	int inval = rpn_toint(input->d);

	if (!inval && (me->cookie & 2)) {
		/* falling edge: schedule timeout */
		libt_add_timeout(delay->d, on_delay, me);
		me->timeout = on_delay;
		/* set output, clear cached input */
		me->cookie = 1;
	} else if (inval && !(me->cookie & 2)) {
		/* set cached input */
		me->cookie |= 2;
	}
	/* write output to stack */
	rpn_push(st, me->cookie & 1);
}
static void rpn_do_ondelay(struct stack *st, struct rpn *me)
{
	struct rpn_el *delay = rpn_pop1(st);
	struct rpn_el *input = rpn_pop1(st);
	int inval = rpn_toint(input->d);

	if (inval && !(me->cookie & 2)) {
		/* rising edge: schedule timeout */
		libt_add_timeout(delay->d, on_delay, me);
		me->timeout = on_delay;
		/* set cache */
		me->cookie |= 2;
	} else if (!inval && (me->cookie & 2)) {
		/* falling edge: cancel timeout */
		libt_remove_timeout(on_delay, me);
		/* clear cache & output */
		me->cookie = 0;
	}
	/* write output to stack */
	rpn_push(st, me->cookie & 1);
}
static void rpn_do_debounce(struct stack *st, struct rpn *me)
{
	struct rpn_el *delay = rpn_pop1(st);
	struct rpn_el *input = rpn_pop1(st);
	int inval = rpn_toint(input->d);

	if (inval != !!(me->cookie & 2)) {
		/* input changed from last value */
		if (inval != (me->cookie & 1)) {
			/* schedule timeout if different */
			libt_add_timeout(delay->d, on_delay, me);
			me->timeout = on_delay;
		} else
			/* value equals output, cancel timer */
			libt_remove_timeout(on_delay, me);
		me->cookie = (me->cookie & ~2) | (inval ? 2 : 0);
	}
	/* write output to stack */
	rpn_push(st, me->cookie & 1);
}
static void rpn_do_debounce2(struct stack *st, struct rpn *me)
{
	struct rpn_el *delay = rpn_pop1(st);
	struct rpn_el *input = rpn_pop1(st);

	/* this works like a debounce, but emits the first event immediately
	 * it is usefull to throttle changes
	 * bit 0: set when throttling
	 */
	if ((isnan(input->d) && !strcmp(input->a ?: "", me->strvalue ?: "")) ||
			(!isnan(input->d) && !dblcmp(input->d, me->value, 0.001))) {
		/* equal value, no throttling */

	} else if (!(me->cookie & 1)) {
		/* throttle */
		me->cookie |= 1;
		/* abuse on_delay to clear the flag */
		libt_add_timeout(delay->d, on_delay, me);
		me->timeout = on_delay;
		/* store value-to-be-propagated */
		me->value = input->d;
		if (me->constvalue)
			free(me->constvalue);
		me->strvalue = me->constvalue = input->a ? strdup(input->a) : NULL;
	}
	/* write output to stack */
	rpn_push_str(st, me->strvalue, me->value);
}

static void rpn_do_autoreset(struct stack *st, struct rpn *me)
{
	struct rpn_el *delay = rpn_pop1(st);
	struct rpn_el *input = rpn_pop1(st);
	int inval = rpn_toint(input->d);

	if (inval && !(me->cookie & 2)) {
		/* rising edge, schedule reset */
		libt_add_timeout(delay->d, on_delay, me);
		me->timeout = on_delay;
		me->cookie |= 2+1;
	} else if (!inval && (me->cookie & 2)) {
		/* falling edge, cancel reset */
		libt_remove_timeout(on_delay, me);
		me->cookie &= ~(2+1);
	}

	/* write output to stack */
	rpn_push(st, me->cookie & 1);
}

/* event functions */
static void rpn_do_edge(struct stack *st, struct rpn *me)
{
	struct rpn_el *input = rpn_pop1(st);
	int inval = rpn_toint(input->d);

	rpn_push(st, inval != me->cookie);
	me->cookie = inval;
}

static void rpn_do_rising(struct stack *st, struct rpn *me)
{
	struct rpn_el *input = rpn_pop1(st);
	int inval = rpn_toint(input->d);

	rpn_push(st, inval && !me->cookie);
	me->cookie = inval;
}

static void rpn_do_falling(struct stack *st, struct rpn *me)
{
	struct rpn_el *input = rpn_pop1(st);
	int inval = rpn_toint(input->d);

	rpn_push(st, !inval && me->cookie);
	me->cookie = inval;
}
static void rpn_do_isnew(struct stack *st, struct rpn *me)
{
	if (st->n < 1) {
		st->errnum = ECANCELED;
		return;
	}
	rpn_push(st, rpn_env_isnew());
}
static void on_timeout(void *dat)
{
	struct rpn *me = dat;

	/* clear output */
	me->cookie ^= 1;
	rpn_run_again(me);
}
static void rpn_do_timeout(struct stack *st, struct rpn *me)
{
	double delay = rpn_pop1(st)->d;
	int inval = rpn_env_isnew();

	if (inval || !(me->cookie & 2)) {
		/* input active, schedule timeout */
		libt_add_timeout(delay, on_timeout, me);
		me->timeout = on_timeout;
		me->cookie = 2;
	}

	rpn_push(st, me->cookie & 1);
}

static void rpn_do_setreset(struct stack *st, struct rpn *me)
{
	struct rpn_el *inreset = rpn_pop1(st);
	struct rpn_el *inset = rpn_pop1(st);

	if (rpn_toint(inset->d))
		me->cookie = 1;
	else if (rpn_toint(inreset->d))
		me->cookie = 0;

	rpn_push(st, me->cookie);
}


/* date/time functions */
static void rpn_do_wakeup(struct stack *st, struct rpn *me)
{
	struct rpn_el *delay = rpn_pop1(st);

	/* leave the diff with the latest value on stack */
	if (!(delay->d > 0.01)) {
		mylog(LOG_WARNING, "wakeup: delay %.3fs too small, corrected to 1s", delay->d);
		delay->d = 1;
	}
	/* find 10msec after scheduled time */
	double wait = libt_timetointerval4(libt_walltime(), delay->d, 0.01, 0.000);
	libt_add_timeout(wait, on_timeout, me);
	me->timeout = on_timeout;
}
static void rpn_do_wakeup2(struct stack *st, struct rpn *me)
{
	rpn_do_wakeup(st, me);
	/* forward result of my timer */
	rpn_push(st, me->cookie);
	me->cookie = 0;
}
static void rpn_do_delta(struct stack *st, struct rpn *me)
{
	rpn_do_wakeup(st, me);

	double saved = rpn_pop1(st)->d;
	double value = rpn_pop1(st)->d;

	if (me->cookie & 1) {
		if (isnan(saved))
			saved = 0;
		if (isnan(value))
			value = 0;

		/* produce new delta */
		me->value = value - saved;

		if ((value-saved)/(value+saved) < 1e-6)
			goto nochange;

		rpn_push(st, me->value);
		rpn_push(st, value);
		rpn_push(st, 1);
		me->cookie |= 0x02;
	} else {
nochange:
		if (me->cookie & 0x02)
			/* re-publish saved value */
			rpn_push(st, me->value);
		rpn_push(st, 0);
	}
	me->cookie &= ~0x01;
}

static void rpn_do_timeofday(struct stack *st, struct rpn *me)
{
	time_t t;
	struct tm *tm;

	time(&t);
	tm = localtime(&t);
	rpn_push(st, tm->tm_hour*3600 + tm->tm_min*60 + tm->tm_sec);
}

static void rpn_do_dayofweek(struct stack *st, struct rpn *me)
{
	time_t t;
	struct tm *tm;

	time(&t);
	tm = localtime(&t);
	rpn_push(st, tm->tm_wday ?: 7 /* push 7 for sunday */);
}

static void rpn_do_abstime(struct stack *st, struct rpn *me)
{
	rpn_push(st, time(NULL));
}

static void rpn_do_uptime(struct stack *st, struct rpn *me)
{
	int ret, fd;
	char buf[128];

	fd = open("/proc/uptime", O_RDONLY);
	if (fd < 0) {
		st->errnum = errno;
		return;
	}
	ret = read(fd, buf, sizeof(buf));
	if (ret > 0) {
		buf[ret] = 0;
		rpn_push(st, strtoul(buf, 0, 0));
	} else
		rpn_push(st, NAN);
	close(fd);
}

static void rpn_do_strftime(struct stack *st, struct rpn *me)
{
	static char buf[1024];
	struct rpn_el *fmt = rpn_pop1(st);
	struct rpn_el *t = rpn_pop1(st);

	time_t stamp = t->d;
	strftime(buf, sizeof(buf), fmt->a, localtime(&stamp));
	rpn_push_str(st, buf, NAN);
}

static void rpn_do_delaytostr(struct stack *st, struct rpn *me)
{
	static char buf[128];
	char *str = buf;

	double value = rpn_pop1(st)->d;

	if (str > buf || value > 2*7*24*60*60) {
		str += sprintf(str, "%.0fw", value / (7*24*60*60));
		value = fmod(value, 7*24*60*60);
	}
	if (str > buf || value > 1.5*24*60*60) {
		str += sprintf(str, "%.0fd", value / (24*60*60));
		value = fmod(value, 24*60*60);
	}
	if (str > buf || value > 70*60) {
		str += sprintf(str, "%.0fh", value / (60*60));
		value = fmod(value, 60*60);
	}
	if (str > buf || value > 60) {
		str += sprintf(str, "%.0fm", value / (60));
		value = fmod(value, 60);
	}
	if (str > buf || value > 0) {
		str += sprintf(str, "%.0fs", value);
		value = fmod(value, 1);
	}
	rpn_push_str(st, buf, NAN);
}

static void rpn_do_fmtvalue(struct stack *st, struct rpn *me)
{
	static char buf[128];
	struct rpn_el *fmt = rpn_pop1(st);
	struct rpn_el *v = rpn_pop1(st);

	snprintf(buf, sizeof(buf), fmt->a, v->d);
	rpn_push_str(st, buf, v->d);
}

/* sin/cos */
static double degtorad(double d)
{
	return d * M_PI / 180;
}
static double radtodeg(double d)
{
	return d * 180 / M_PI;
}
static void rpn_do_degtorad(struct stack *st, struct rpn *me)
{
	rpn_push(st, degtorad(rpn_pop1(st)->d));
}
static void rpn_do_radtodeg(struct stack *st, struct rpn *me)
{
	rpn_push(st, radtodeg(rpn_pop1(st)->d));
}
static void rpn_do_sin(struct stack *st, struct rpn *me)
{
	rpn_push(st, sin(rpn_pop1(st)->d));
}
static void rpn_do_cos(struct stack *st, struct rpn *me)
{
	rpn_push(st, cos(rpn_pop1(st)->d));
}

/* sun position */
static void rpn_do_sun(struct stack *st, struct rpn *me)
{
	struct rpn_el *lon = rpn_pop1(st);
	struct rpn_el *lat = rpn_pop1(st);

	struct sunpos pos;

	pos = sun_pos_strous(time(NULL), lat->d, lon->d);
	rpn_push(st, pos.elevation);
}

static void rpn_do_sun3(struct stack *st, struct rpn *me)
{
	struct rpn_el *lon = rpn_pop1(st);
	struct rpn_el *lat = rpn_pop1(st);
	struct rpn_el *t = rpn_pop1(st);

	struct sunpos pos;

	pos = sun_pos_strous(t->d, lat->d, lon->d);
	rpn_push(st, pos.elevation);
}

static void rpn_do_azimuth3(struct stack *st, struct rpn *me)
{
	struct rpn_el *lon = rpn_pop1(st);
	struct rpn_el *lat = rpn_pop1(st);
	struct rpn_el *t = rpn_pop1(st);

	struct sunpos pos;

	pos = sun_pos_strous(t->d, lat->d, lon->d);
	rpn_push(st, pos.azimuth);
}

static void rpn_do_celestial_angle(struct stack *st, struct rpn *me)
{
	double elv2 = rpn_pop1(st)->d;
	double azm2 = rpn_pop1(st)->d;
	double elv1 = rpn_pop1(st)->d;
	double azm1 = rpn_pop1(st)->d;

	elv2 = degtorad(elv2);
	azm2 = degtorad(azm2);
	elv1 = degtorad(elv1);
	azm1 = degtorad(azm1);
	double spheric_cos = sin(elv1)*sin(elv2)
		+ cos(elv1)*cos(elv2)*cos(azm2-azm1);
	double angle = acos(spheric_cos);
	angle = radtodeg(angle);
	rpn_push(st, angle);
}

/* flow control */
static void rpn_do_if(struct stack *st, struct rpn *me)
{
	struct rpn_el *cond = rpn_pop1(st);

	if (!rpn_toint(cond->d))
		st->jumpto = me->rpn ?: QUIT;
}

static void rpn_do_else(struct stack *st, struct rpn *me)
{
	st->jumpto = me->rpn ?: QUIT;
}

static void rpn_do_fi(struct stack *st, struct rpn *me)
{
	/* this is a marker */
}

static void rpn_find_fi_else(struct rpn *rpn,
		struct rpn **pelse, struct rpn **pfi)
{
	int nested = 0;

	*pelse = *pfi = NULL;
	for (rpn = rpn->next; rpn; rpn = rpn->next) {
		if (rpn->run == rpn_do_if) {
			++nested;

		} else if (rpn->run == rpn_do_fi) {
			if (!nested) {
				*pfi = rpn;
				return;
			}
			--nested;

		} else if (rpn->run == rpn_do_else) {
			if (!nested)
				*pelse = rpn;
		}
	}
}

static void rpn_test_if(struct rpn *me)
{
	struct rpn *pelse, *pfi;

	rpn_find_fi_else(me, &pelse, &pfi);
	if (!pfi)
		mylog(LOG_WARNING, "if without fi");
	me->rpn = pelse ? pelse->next : pfi;
}

static void rpn_test_else(struct rpn *me)
{
	struct rpn *pelse, *pfi;

	rpn_find_fi_else(me, &pelse, &pfi);
	if (pelse)
		mylog(LOG_WARNING, "2nd else unexpected");
	me->rpn = pfi;
}

static void rpn_do_quit(struct stack *st, struct rpn *me)
{
	st->jumpto = QUIT;
}

/* run time functions */
void rpn_stack_reset(struct stack *st)
{
	st->n = 0;
	st->jumpto = NULL;
	st->errnum = 0;
}

int rpn_run(struct stack *st, struct rpn *rpn)
{
	for (; rpn; rpn = st->jumpto ?: rpn->next) {
		if (rpn == QUIT)
			break;
		st->jumpto = NULL;
		rpn->run(st, rpn);
		if (st->errnum)
			return -st->errnum;
	}
	return 0;
}

/* parser */
static struct lookup {
	const char *str;
	void (*run)(struct stack *, struct rpn *);
	int flags;
	int privsize;
	void (*free)(struct rpn *);
	void (*parse)(struct rpn *, char *str);
} const lookups[] = {
	{ "+", rpn_do_plus, },
	{ "-", rpn_do_minus, },
	{ "*", rpn_do_mul, },
	{ "/", rpn_do_div, },
	{ "%", rpn_do_mod, },
	{ "**", rpn_do_pow, },
	{ "neg", rpn_do_negative, },

	{ "&", rpn_do_bitand, },
	{ "|", rpn_do_bitor, },
	{ "^", rpn_do_bitxor, },
	{ "~", rpn_do_bitinv, },

	{ "&&", rpn_do_booland, },
	{ "||", rpn_do_boolor, },
	{ "!", rpn_do_boolnot, },
	{ "not", rpn_do_boolnot, },
	{ "==", rpn_do_intequal, },
	{ "!=", rpn_do_intnotequal, },

	{ "<", rpn_do_lt, },
	{ ">", rpn_do_gt, },

	{ "dup", rpn_do_dup, },
	{ "swap", rpn_do_swap, },
	{ "json", rpn_do_json, },
	{ "?:", rpn_do_ifthenelse, },

	{ "min", rpn_do_min, },
	{ "max", rpn_do_max, },
	{ "limit", rpn_do_limit, },
	{ "inrange", rpn_do_inrange, },
	{ "category", rpn_do_category, },
	{ "hyst1", rpn_do_hyst1, },
	{ "hyst2", rpn_do_hyst2, },
	{ "hyst", rpn_do_hyst2, },
	{ "throttle", rpn_do_debounce2, },
	{ "avgtime", rpn_do_avgtime, RPNFN_PERIODIC | RPNFN_WALLTIME, sizeof(struct avgtime), },
	{ "ravg", rpn_do_running_avg, 0, sizeof(struct running),
		.free = free_running, },
	{ "rmin", rpn_do_running_min, 0, sizeof(struct running),
		.free = free_running, },
	{ "rmax", rpn_do_running_max, 0, sizeof(struct running),
		.free = free_running, },
	{ "ramp3", rpn_do_ramp3, },
	{ "slope", rpn_do_slope, 0, sizeof(struct slope),
		.free = free_slope, .parse = parse_slope, },

	{ "ondelay", rpn_do_ondelay, },
	{ "offdelay", rpn_do_offdelay, },
	{ "afterdelay", rpn_do_afterdelay, },
	{ "debounce", rpn_do_debounce, },
	{ "debounce2", rpn_do_debounce2, },
	{ "autoreset", rpn_do_autoreset, },

	{ "isnew", rpn_do_isnew, },
	{ "timeout", rpn_do_timeout, },
	{ "edge", rpn_do_edge, },
	{ "rising", rpn_do_rising, },
	{ "falling", rpn_do_falling, },
	{ "changed", rpn_do_edge, },
	{ "pushed", rpn_do_rising, },
	{ "setreset", rpn_do_setreset, },

	{ "wakeup", rpn_do_wakeup, RPNFN_PERIODIC | RPNFN_WALLTIME, },
	{ "wakeup2", rpn_do_wakeup2, RPNFN_PERIODIC | RPNFN_WALLTIME, },
	{ "delta", rpn_do_delta, RPNFN_PERIODIC | RPNFN_WALLTIME, },
	{ "timeofday", rpn_do_timeofday, RPNFN_WALLTIME, },
	{ "dayofweek", rpn_do_dayofweek, RPNFN_WALLTIME, },
	{ "abstime", rpn_do_abstime, RPNFN_WALLTIME, },
	{ "uptime", rpn_do_uptime, },
	{ "strftime", rpn_do_strftime, },
	{ "delaytostr", rpn_do_delaytostr, },

	{ "printf", rpn_do_fmtvalue, },

	{ "degtorad", rpn_do_degtorad, },
	{ "radtodeg", rpn_do_radtodeg, },
	{ "sin", rpn_do_sin, },
	{ "cos", rpn_do_cos, },

	{ "sun", rpn_do_sun, RPNFN_WALLTIME, },
	{ "sun3", rpn_do_sun3, },
	{ "azimuth3", rpn_do_azimuth3, },
	{ "celestial_angle", rpn_do_celestial_angle, }, /* azm1 elv1 azm2 elv2 celestial_angle */

	{ "if", rpn_do_if, },
	{ "else", rpn_do_else, },
	{ "fi", rpn_do_fi, },
	{ "quit", rpn_do_quit, },
	{ "", },
};

static const struct lookup *do_lookup(const char *tok, char **pendp)
{
	const struct lookup *lookup;
	char *endp;

	if (!pendp)
		pendp = &endp;

	*pendp = strchr(tok, ',');
	if (*pendp) {
		**pendp = 0;
		(*pendp)++;
	}

	for (lookup = lookups; lookup->str[0]; ++lookup) {
		if (!strcmp(lookup->str, tok))
			return lookup;
	}
	return NULL;
}

static void free_lookup(struct rpn *rpn)
{
	if (rpn->lookup && rpn->lookup->free)
		rpn->lookup->free(rpn);
}

static struct constant {
	const char *name;
	double value;
} const constants[] = {
	{ "pi", M_PI, },
	{ "e", M_E, },
	{ "", NAN, },
};

static const struct constant *do_constant(const char *tok)
{
	const struct constant *lp;

	for (lp = constants; *lp->name; ++lp) {
		if (!strcmp(lp->name, tok))
			return lp;
	}
	return NULL;
}

/* modified strtok:
 * don't seperate between " chars
 * This keeps the " characters in the string.
 */
static char *mystrtok(char *newstr, const char *sep)
{
	static char *str;
	char *savedstr = NULL;
	int instring = 0;

	if (newstr)
		str = newstr;
	for (; *str; ++str) {
		if (!instring && strchr(sep, *str)) {
			if (savedstr) {
				/* end reached */
				*str++ = 0;
				return savedstr;
			}
		} else {
			if (!savedstr)
				savedstr = str;
			if (*str == '"')
				instring = !instring;
		}
	}
	return savedstr;
}

int rpn_parse_append(const char *cstr, struct rpn **proot, void *dat)
{
	char *savedstr;
	char *tok, *endp;
	int result;
	struct rpn *last = NULL, *rpn, **localproot;
	const struct lookup *lookup;
	const struct constant *constant;
	double tmp;

	/* find current 'last' rpn */
	for (last = *proot; last && last->next; last = last->next);
	localproot = last ? &last->next : proot;
	/* parse */
	savedstr = strdup(cstr);
	for (tok = mystrtok(savedstr, " \t"), result = 0; tok;
			tok = mystrtok(NULL, " \t"), ++result) {
		rpn = rpn_create();
		tmp = mystrtod(tok, &endp);
		if ((endp > tok) && !*endp) {
			rpn->run = rpn_do_const;
			rpn->value = tmp;

		} else if (*tok == '"') {
			if (tok[strlen(tok)-1] == '"')
				tok[strlen(tok)-1] = 0;
			++tok;
			rpn->run = rpn_do_const;
			rpn->constvalue = strdup(tok);
			rpn->value = mystrtod(tok, NULL);

		} else if (strchr("$>=", *tok) && tok[1] == '{' && tok[strlen(tok)-1] == '}') {
			rpn->topic = strndup(tok+2, strlen(tok+2)-1);
			switch (*tok) {
			case '$':
				rpn->run = rpn_do_env;
				break;
			case '=':
				rpn->run = rpn_do_writeenv;
				rpn->cookie = 1;
				break;
			case '>':
				rpn->run = rpn_do_writeenv;
				break;
			}
		} else if ((lookup = do_lookup(tok, &endp)) != NULL) {
			rpn->run = lookup->run;
			rpn->flags = lookup->flags;
			rpn->lookup = lookup;
			if (lookup->privsize)
				rpn = rpn_grow(rpn, lookup->privsize);
			if (lookup->parse)
				lookup->parse(rpn, endp);
		} else if ((constant = do_constant(tok)) != NULL) {
			rpn->run = rpn_do_const;
			rpn->value = constant->value;
			rpn->constvalue = strdup(tok);

		} else {
			mylog(LOG_INFO | LOG_MQTT, "unknown token '%s'", tok);
			rpn_free(rpn);
			goto failed;
		}
		rpn->dat = dat;
		if (last)
			last->next = rpn;
		if (!*proot)
			*proot = rpn;
		last = rpn;
	}
	free(savedstr);
	return result;

failed:
	rpn_free_chain(*localproot);
	*localproot = 0;
	return -1;
}

void rpn_parse_done(struct rpn *root)
{
	struct rpn *rpn;

	/* do static tests */
	for (rpn = root; rpn; rpn = rpn->next) {
		if (rpn->run == rpn_do_if)
			rpn_test_if(rpn);
		else if (rpn->run == rpn_do_else)
			rpn_test_else(rpn);
	}
}

struct rpn *rpn_parse(const char *cstr, void *dat)
{
	struct rpn *rpns = NULL;

	rpn_parse_append(cstr, &rpns, dat);
	rpn_parse_done(rpns);
	return rpns;
}

int rpn_collect_flags(struct rpn *rpn)
{
	int flags = 0;

	for (; rpn; rpn = rpn->next) {
		flags |= rpn->flags;
		if (rpn->lookup)
			flags |= RPNFN_LOGIC;
	}
	return flags;
}
