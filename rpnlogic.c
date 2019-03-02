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
#include "sun.h"
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
	return rpn;
}

static void rpn_free(struct rpn *rpn)
{
	if (rpn->topic)
		free(rpn->topic);
	if (rpn->strvalue)
		free(rpn->strvalue);
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
	rpn_push_str(st, me->strvalue, me->value);
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
	if (!rpn_env_isnew()) {
		rpn_pop1(st);
		rpn_push_el(st, &dummy);
	}
}

/* date/time functions */
static void rpn_do_wakeup(struct stack *st, struct rpn *me)
{
	struct rpn_el *delay = rpn_pop1(st);
	time_t t, next;
	int align;

	/* leave the diff with the latest value on stack */
	align = rpn_toint(delay->d);
	if (!align)
		align = 1;
	time(&t);

	next = t - t % align + align;
	libt_add_timeout(next - t, rpn_run_again, me);
	me->timeout = rpn_run_again;
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

/* sun position */
static void rpn_do_sun(struct stack *st, struct rpn *me)
{
	struct rpn_el *lon = rpn_pop1(st);
	struct rpn_el *lat = rpn_pop1(st);

	double incl, azm;
	int ret;

	ret = sungetpos(time(NULL), lat->d, lon->d, &incl, &azm, NULL);
	rpn_push(st, (ret >= 0) ? incl : NAN);
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
	{ "?:", rpn_do_ifthenelse, },

	{ "limit", rpn_do_limit, },
	{ "inrange", rpn_do_inrange, },
	{ "category", rpn_do_category, },
	{ "hyst1", rpn_do_hyst1, },
	{ "hyst2", rpn_do_hyst2, },
	{ "hyst", rpn_do_hyst2, },

	{ "ondelay", rpn_do_ondelay, },
	{ "offdelay", rpn_do_offdelay, },
	{ "afterdelay", rpn_do_afterdelay, },
	{ "debounce", rpn_do_debounce, },
	{ "autoreset", rpn_do_autoreset, },

	{ "isnew", rpn_do_isnew, },
	{ "edge", rpn_do_edge, },
	{ "rising", rpn_do_rising, },
	{ "falling", rpn_do_falling, },
	{ "changed", rpn_do_edge, },
	{ "pushed", rpn_do_rising, },

	{ "wakeup", rpn_do_wakeup, RPNFN_PERIODIC | RPNFN_WALLTIME, },
	{ "timeofday", rpn_do_timeofday, RPNFN_WALLTIME, },
	{ "dayofweek", rpn_do_dayofweek, },
	{ "abstime", rpn_do_abstime, },
	{ "uptime", rpn_do_uptime, },
	{ "strftime", rpn_do_strftime, },

	{ "sun", rpn_do_sun, RPNFN_WALLTIME, },

	{ "if", rpn_do_if, },
	{ "else", rpn_do_else, },
	{ "fi", rpn_do_fi, },
	{ "quit", rpn_do_quit, },
	{ "", },
};

static const struct lookup *do_lookup(const char *tok)
{
	const struct lookup *lookup;

	for (lookup = lookups; lookup->str[0]; ++lookup) {
		if (!strcmp(lookup->str, tok))
			return lookup;
	}
	return NULL;
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
			} else {
				/* start ok token */
				savedstr = str;
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

static const char digits[] = "0123456789";
int rpn_parse_append(const char *cstr, struct rpn **proot, void *dat)
{
	char *savedstr;
	char *tok;
	int result;
	struct rpn *last = NULL, *rpn, **localproot;
	const struct lookup *lookup;
	const struct constant *constant;

	/* find current 'last' rpn */
	for (last = *proot; last && last->next; last = last->next);
	localproot = last ? &last->next : proot;
	/* parse */
	savedstr = strdup(cstr);
	for (tok = mystrtok(savedstr, " \t"), result = 0; tok;
			tok = mystrtok(NULL, " \t"), ++result) {
		rpn = rpn_create();
		if (strchr(digits, *tok) || (tok[1] && strchr("+-", *tok) && strchr(digits, tok[1]))) {
			rpn->run = rpn_do_const;
			rpn->value = mystrtod(tok, NULL);

		} else if (*tok == '"') {
			if (tok[strlen(tok)-1] == '"')
				tok[strlen(tok)-1] = 0;
			++tok;
			rpn->run = rpn_do_const;
			rpn->strvalue = strdup(tok);
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
		} else if ((lookup = do_lookup(tok)) != NULL) {
			rpn->run = lookup->run;
			rpn->flags = lookup->flags;

		} else if ((constant = do_constant(tok)) != NULL) {
			rpn->run = rpn_do_const;
			rpn->value = constant->value;
			rpn->strvalue = strdup(tok);

		} else {
			mylog(LOG_INFO, "unknown token '%s'", tok);
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

	for (; rpn; rpn = rpn->next)
		flags |= rpn->flags;
	return flags;
}
