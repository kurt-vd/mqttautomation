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

#define mylog(loglevel, fmt, ...) \
	({\
		syslog(loglevel, fmt, ##__VA_ARGS__); \
		if (loglevel <= LOG_ERR)\
			exit(1);\
	})
#define ESTR(num)	strerror(num)

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

double rpn_strtod(const char *str, char **endp)
{
	char *localendp;
	double value;

	if (!endp)
		endp = &localendp;
	value = strtod(str ?: "nan", endp);
	if (**endp && strchr(":h'", **endp))
		value += strtod((*endp)+1, endp)/60;
	if (**endp && strchr(":m\"", **endp))
		value += strtod((*endp)+1, endp)/3600;
	return (*endp == str) ? NAN : value;
}
const char *rpn_dtostr(double d)
{
	static char buf[64];
	char *str;
	int ptpresent = 0;

	sprintf(buf, "%lg", d);
	for (str = buf; *str; ++str) {
		if (*str == '.')
			ptpresent = 1;
		else if (*str == 'e')
			/* nothing to do anymore */
			break;
		else if (ptpresent && *str == '0') {
			int len = strspn(str, "0");
			if (!str[len]) {
				/* entire string (after .) is '0' */
				*str = 0;
				if (str > buf && *(str-1) == '.')
					/* remote '.' too */
					*(str-1) = 0;
				break;
			}
		}
	}
	return buf;
}

static inline int rpn_toint(double val)
{
	/* provide a NAN-safe int conversion */
	if (isnan(val))
		return 0;
	else
		return (int)val;
}

static inline void rpn_set_strvalue(struct stack *st, const char *value)
{
	st->strvalue = value;
	st->strvalueset = 1;
}

/* algebra */
static int rpn_do_plus(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] + st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_minus(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] - st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_mul(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] * st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_div(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] / st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_mod(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = fmod(st->v[st->n-2], st->v[st->n-1]);
	st->n -= 1;
	return 0;
}
static int rpn_do_pow(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = pow(st->v[st->n-2], st->v[st->n-1]);
	st->n -= 1;
	return 0;
}

/* utilities */
static int rpn_do_limit(struct stack *st, struct rpn *me)
{
	if (st->n < 3)
		/* stack underflow */
		return -1;
	/* limit x-3 between x-2 & x-1 */
	if (st->v[st->n-3] < st->v[st->n-2])
		st->v[st->n-3] = st->v[st->n-2];
	else if (st->v[st->n-3] > st->v[st->n-1])
		st->v[st->n-3] = st->v[st->n-1];
	st->n -= 2;
	return 1;
}

static int rpn_do_inrange(struct stack *st, struct rpn *me)
{
	if (st->n < 3)
		/* stack underflow */
		return -1;
	/* limit x-3 between x-2 & x-1 */
	if (st->v[st->n-2] < st->v[st->n-1])
		/* regular case: DUT min max */
		st->v[st->n-3] = (st->v[st->n-3] >= st->v[st->n-2]) && (st->v[st->n-3] <= st->v[st->n-1]);
	else if (st->v[st->n-2] > st->v[st->n-1])
		/* regular case: DUT max min */
		st->v[st->n-3] = (st->v[st->n-3] >= st->v[st->n-2]) || (st->v[st->n-3] <= st->v[st->n-1]);
	else
		st->v[st->n-3] = 0;
	st->n -= 2;
	return 1;
}

/* bitwise */
static int rpn_do_bitand(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = rpn_toint(st->v[st->n-2]) & rpn_toint(st->v[st->n-1]);
	st->n -= 1;
	return 0;
}
static int rpn_do_bitor(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = rpn_toint(st->v[st->n-2]) | rpn_toint(st->v[st->n-1]);
	st->n -= 1;
	return 0;
}
static int rpn_do_bitxor(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = rpn_toint(st->v[st->n-2]) ^ rpn_toint(st->v[st->n-1]);
	st->n -= 1;
	return 0;
}
static int rpn_do_bitinv(struct stack *st, struct rpn *me)
{
	if (st->n < 1)
		/* stack underflow */
		return -1;
	st->v[st->n-1] = ~rpn_toint(st->v[st->n-1]);
	return 0;
}

/* boolean */
static int rpn_do_booland(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = rpn_toint(st->v[st->n-2]) && rpn_toint(st->v[st->n-1]);
	st->n -= 1;
	return 0;
}
static int rpn_do_boolor(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = rpn_toint(st->v[st->n-2]) || rpn_toint(st->v[st->n-1]);
	st->n -= 1;
	return 0;
}
static int rpn_do_boolnot(struct stack *st, struct rpn *me)
{
	if (st->n < 1)
		/* stack underflow */
		return -1;
	st->v[st->n-1] = !rpn_toint(st->v[st->n-1]);
	return 0;
}
static int rpn_do_intequal(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = rpn_toint(st->v[st->n-2]) == rpn_toint(st->v[st->n-1]);
	st->n -= 1;
	return 0;
}
static int rpn_do_intnotequal(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = rpn_toint(st->v[st->n-2]) != rpn_toint(st->v[st->n-1]);
	st->n -= 1;
	return 0;
}

/* compare */
static int rpn_do_lt(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] < st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_gt(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] > st->v[st->n-1];
	st->n -= 1;
	return 0;
}

/* generic */
static void rpn_push(struct stack *st, double value)
{
	if (st->n >= st->s) {
		st->s += 16;
		st->v = realloc(st->v, st->s * sizeof(st->v[0]));
		if (!st->v)
			mylog(LOG_ERR, "realloc stack %u failed", st->s);
	}
	st->v[st->n++] = value;
}

static int rpn_do_const(struct stack *st, struct rpn *me)
{
	rpn_set_strvalue(st, me->strvalue);
	rpn_push(st, me->value);
	return 0;
}

static int rpn_do_strconst(struct stack *st, struct rpn *me)
{
	rpn_set_strvalue(st, me->strvalue);
	rpn_push(st, rpn_strtod(st->strvalue ?: "nan", NULL));
	return 0;
}

static int rpn_do_env(struct stack *st, struct rpn *me)
{
	rpn_set_strvalue(st, rpn_lookup_env(me->topic, me));
	rpn_push(st, rpn_strtod(st->strvalue ?: "nan", NULL));
	return 0;
}

static int rpn_do_writeenv(struct stack *st, struct rpn *me)
{
	if (st->n < 1)
		/* stack underflow */
		return -1;

	rpn_write_env(st->strvalue ?: rpn_dtostr(st->v[st->n-1]), me->topic, me);
	st->n -= 1;
	return 0;
}

static int rpn_do_dup(struct stack *st, struct rpn *me)
{
	if (st->n < 1)
		/* stack underflow */
		return -1;
	rpn_push(st, st->v[st->n-1]);
	return 0;
}
static int rpn_do_swap(struct stack *st, struct rpn *me)
{
	double tmp;

	if (st->n < 2)
		/* stack underflow */
		return -1;
	tmp = st->v[st->n-2];
	st->v[st->n-2] = st->v[st->n-1];
	st->v[st->n-1] = tmp;
	return 0;
}
static int rpn_do_ifthenelse(struct stack *st, struct rpn *me)
{
	if (st->n < 3)
		/* stack underflow */
		return -1;

	st->v[st->n-3] = rpn_toint(st->v[st->n-3]) ? st->v[st->n-2] : st->v[st->n-1];
	st->n -= 2;
	return 0;
}

/* timer functions */
static void on_delay(void *dat)
{
	struct rpn *me = dat;

	/* clear output */
	me->cookie ^= 1;
	rpn_run_again(me);
}
static int rpn_do_offdelay(struct stack *st, struct rpn *me)
{
	int inval;

	if (st->n < 2)
		/* stack underflow */
		return -1;

	inval = rpn_toint(st->v[st->n-2]);

	if (!inval && (me->cookie & 2)) {
		/* falling edge: schedule timeout */
		libt_add_timeout(st->v[st->n-1], on_delay, me);
		/* clear cache */
		me->cookie &= ~2;
	} else if (inval && !(me->cookie & 2)) {
		/* rising edge: cancel timeout */
		libt_remove_timeout(on_delay, me);
		/* set cache & output */
		me->cookie = 2+1;
	}
	/* write output to stack */
	st->v[st->n-2] = me->cookie & 1;
	st->n -= 1;
	return 0;
}
static int rpn_do_ondelay(struct stack *st, struct rpn *me)
{
	int inval;

	if (st->n < 2)
		/* stack underflow */
		return -1;

	inval = rpn_toint(st->v[st->n-2]);

	if (inval && !(me->cookie & 2)) {
		/* rising edge: schedule timeout */
		libt_add_timeout(st->v[st->n-1], on_delay, me);
		/* set cache */
		me->cookie |= 2;
	} else if (!inval && (me->cookie & 2)) {
		/* falling edge: cancel timeout */
		libt_remove_timeout(on_delay, me);
		/* clear cache & output */
		me->cookie = 0;
	}
	/* write output to stack */
	st->v[st->n-2] = me->cookie & 1;
	st->n -= 1;
	return 0;
}
static int rpn_do_offtimer(struct stack *st, struct rpn *me)
{
	/* schedule low out when input becomes high */
	/* I didn't get timeouts correct without this */
	int inval;

	if (st->n < 2)
		/* stack underflow */
		return -1;

	inval = rpn_toint(st->v[st->n-2]);

	if (inval) {
		me->cookie = 1;
		libt_add_timeout(st->v[st->n-1], on_delay, me);
	} else if (me->cookie) {
		me->cookie = 0;
		libt_remove_timeout(on_delay, me);
	}
	/* write output to stack */
	st->v[st->n-2] = me->cookie & 1;
	st->n -= 1;
	return 0;
}

/* event functions */
static int rpn_do_edge(struct stack *st, struct rpn *me)
{
	int inval;

	if (st->n < 1)
		/* stack underflow */
		return -1;

	inval = rpn_toint(st->v[st->n-1]);
	st->v[st->n-1] = inval != me->cookie;
	me->cookie = inval;
	return 0;
}

static int rpn_do_rising(struct stack *st, struct rpn *me)
{
	int inval;

	if (st->n < 1)
		/* stack underflow */
		return -1;

	inval = rpn_toint(st->v[st->n-1]);
	/* set output on rising edge */
	st->v[st->n-1] = inval && !me->cookie;
	me->cookie = inval;
	return 0;
}

static int rpn_do_falling(struct stack *st, struct rpn *me)
{
	int inval;

	if (st->n < 1)
		/* stack underflow */
		return -1;

	inval = rpn_toint(st->v[st->n-1]);
	/* set output on rising edge */
	st->v[st->n-1] = !inval && me->cookie;
	me->cookie = inval;
	return 0;
}
static int rpn_do_isnew(struct stack *st, struct rpn *me)
{
	if (st->n < 1)
		/* stack underflow */
		return -1;
	if (!rpn_env_isnew())
		st->v[st->n-1] = NAN;
	return 0;
}

/* date/time functions */
static inline int next_minute(struct tm *tm)
{
	int next;

	next = 60 - tm->tm_sec;
	return (next <= 0 || next > 60) ? 60 : next;
}

static int rpn_do_wakeup(struct stack *st, struct rpn *me)
{
	time_t t, next;
	int align;

	if (st->n < 1)
		/* stack underflow */
		return -1;
	/* leave the diff with the latest value on stack */
	align = rpn_toint(st->v[st->n-1]);
	time(&t);

	next = t - t % align + align;
	libt_add_timeout(next - t, rpn_run_again, me);

	st->n -= 1;
	return 0;
}

static int rpn_do_timeofday(struct stack *st, struct rpn *me)
{
	time_t t;
	struct tm *tm;

	time(&t);
	tm = localtime(&t);
	rpn_push(st, tm->tm_hour + tm->tm_min/60.0 + tm->tm_sec/3600.0);
	return 0;
}

static int rpn_do_dayofweek(struct stack *st, struct rpn *me)
{
	time_t t;
	struct tm *tm;

	time(&t);
	tm = localtime(&t);
	rpn_push(st, tm->tm_wday ?: 7 /* push 7 for sunday */);
	return 0;
}

static int rpn_do_abstime(struct stack *st, struct rpn *me)
{
	rpn_push(st, time(NULL));
	return 0;
}

static int rpn_do_uptime(struct stack *st, struct rpn *me)
{
	int ret, fd;
	char buf[128];

	fd = open("/proc/uptime", O_RDONLY);
	if (fd < 0)
		return -errno;
	ret = read(fd, buf, sizeof(buf));
	if (ret > 0) {
		buf[ret] = 0;
		rpn_push(st, strtoul(buf, 0, 0));
	}
	close(fd);
	return ret;
}

static int rpn_do_strftime(struct stack *st, struct rpn *me)
{
	static char buf[1024];

	if (st->n < 2)
		/* stack underflow */
		return -1;

	if (!st->strvalue)
		return -1;

	long stamp = st->v[st->n-2];
	strftime(buf, sizeof(buf), st->strvalue, localtime(&stamp));
	st->n -= 1;
	rpn_set_strvalue(st, buf);
	return 0;
}

/* flow control */
static int rpn_do_if(struct stack *st, struct rpn *me)
{
	if (st->n < 1)
		/* stack underflow */
		return -1;

	if (!rpn_toint(st->v[st->n-1]))
		st->jumpto = me->rpn ?: QUIT;
	st->n -= 1;
	st->strvalueset = 1;
	return 0;
}

static int rpn_do_else(struct stack *st, struct rpn *me)
{
	st->jumpto = me->rpn ?: QUIT;
	st->strvalueset = 1;
	return 0;
}

static int rpn_do_fi(struct stack *st, struct rpn *me)
{
	/* this is a marker */
	st->strvalueset = 1;
	return 0;
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

static int rpn_do_quit(struct stack *st, struct rpn *me)
{
	st->jumpto = QUIT;
	/* real quit is done in rpn_run */
	st->strvalueset = 1;
	return 0;
}

/* run time functions */
void rpn_stack_reset(struct stack *st)
{
	st->n = 0;
	st->strvalue = NULL;
	st->jumpto = NULL;
}

int rpn_run(struct stack *st, struct rpn *rpn)
{
	int ret;

	for (; rpn; rpn = st->jumpto ?: rpn->next) {
		if (rpn == QUIT)
			break;
		st->jumpto = NULL;
		st->strvalueset = 0;
		ret = rpn->run(st, rpn);
		if (!st->strvalueset)
			/* keep st->strvalue valid only 1 iteration */
			st->strvalue = NULL;
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* parser */
static struct lookup {
	const char *str;
	int (*run)(struct stack *, struct rpn *);
} const lookups[] = {
	{ "+", rpn_do_plus, },
	{ "-", rpn_do_minus, },
	{ "*", rpn_do_mul, },
	{ "/", rpn_do_div, },
	{ "%", rpn_do_mod, },
	{ "**", rpn_do_pow, },

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

	{ "ondelay", rpn_do_ondelay, },
	{ "offdelay", rpn_do_offdelay, },
	{ "offtimer", rpn_do_offtimer, },

	{ "isnew", rpn_do_isnew, },
	{ "edge", rpn_do_edge, },
	{ "rising", rpn_do_rising, },
	{ "falling", rpn_do_falling, },
	{ "changed", rpn_do_edge, },
	{ "pushed", rpn_do_rising, },

	{ "wakeup", rpn_do_wakeup, },
	{ "timeofday", rpn_do_timeofday, },
	{ "dayofweek", rpn_do_dayofweek, },
	{ "abstime", rpn_do_abstime, },
	{ "uptime", rpn_do_uptime, },
	{ "strftime", rpn_do_strftime, },

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
struct rpn *rpn_parse(const char *cstr, void *dat)
{
	char *savedstr;
	char *tok;
	struct rpn *root = NULL, *last = NULL, *rpn;
	const struct lookup *lookup;
	const struct constant *constant;

	savedstr = strdup(cstr);
	for (tok = mystrtok(savedstr, " \t"); tok; tok = mystrtok(NULL, " \t")) {
		rpn = rpn_create();
		if (strchr(digits, *tok) || (tok[1] && strchr("+-", *tok) && strchr(digits, tok[1]))) {
			rpn->run = rpn_do_const;
			rpn->value = rpn_strtod(tok, NULL);

		} else if (*tok == '"') {
			if (tok[strlen(tok)-1] == '"')
				tok[strlen(tok)-1] = 0;
			++tok;
			rpn->run = rpn_do_strconst;
			rpn->strvalue = strdup(tok);

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

		} else if ((constant = do_constant(tok)) != NULL) {
			rpn->run = rpn_do_const;
			rpn->value = constant->value;
			rpn->strvalue = strdup(tok);

		} else {
			mylog(LOG_INFO, "unknown token '%s'", tok);
			rpn_free(rpn);
			if (root)
				rpn_free_chain(root);
			root = NULL;
			break;
		}
		rpn->dat = dat;
		if (last)
			last->next = rpn;
		if (!root)
			root = rpn;
		last = rpn;
	}
	free(savedstr);

	/* do static tests */
	for (rpn = root; rpn; rpn = rpn->next) {
		if (rpn->run == rpn_do_if)
			rpn_test_if(rpn);
		else if (rpn->run == rpn_do_else)
			rpn_test_else(rpn);
	}
	return root;
}
