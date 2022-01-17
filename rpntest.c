#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <locale.h>
#include <poll.h>
#include <syslog.h>

#include "lib/libt.h"
#include "rpnlogic.h"
#include "common.h"

const char *rpn_lookup_env(const char *str, struct rpn *rpn)
{
	return getenv(str);
}
int rpn_write_env(const char *value, const char *str, struct rpn *rpn)
{
	printf("%c{%s} '%s'\n", rpn->cookie ? '=' : '>', str, value);
	return 0;
}
int rpn_env_isnew(void)
{
	return 0;
}

static void my_rpn_run(struct rpn *rpn)
{
	struct stack rpnstack = {};
	struct rpn_el *el;
	int j;

	if (rpn_run(&rpnstack, rpn))
		printf("failed\n");
	for (j = 0; j < rpnstack.n; ++j) {
		el = rpnstack.v+j;
		if (el->a)
			printf("%s\"%s\"", j ? " " : "", el->a);
		else
			printf("%s%s", j ? " " : "", mydtostr(el->d));
	}
	printf("\n");
	fflush(stdout);
}

void rpn_run_again(void *dat)
{
	struct rpn *rpn = dat;
	struct rpn **rootrpn = rpn->dat;

	my_rpn_run(*rootrpn);
}

int main(int argc, char *argv[])
{
	struct rpn *rpn = NULL;

	myopenlog("rpntest", 0, LOG_LOCAL2);
	myloglevel(LOG_INFO);
	setlocale(LC_TIME, "");

	for (++argv; *argv; ++argv) {
		if (rpn_parse_append(*argv, &rpn, &rpn) < 0)
			return 1;
	}
	rpn_parse_done(rpn);
	if (!rpn)
		return 1;

	my_rpn_run(rpn);
	for (; libt_get_waittime() >= 0;) {
		poll(NULL, 0, libt_get_waittime());
		libt_flush();
	}
	return 0;
}
