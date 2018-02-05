#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <locale.h>
#include <syslog.h>

#include "lib/libt.h"
#include "rpnlogic.h"

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
	int j;

	if (rpn_run(&rpnstack, rpn))
		printf("failed\n");
	for (j = 0; j < rpnstack.n; ++j)
		printf("[%i] %s\n", j, (!j && rpnstack.strvalue) ?
				rpnstack.strvalue : rpn_dtostr(rpnstack.v[j]));
}

void rpn_run_again(void *dat)
{
	struct rpn *rpn = dat;
	struct rpn **rootrpn = rpn->dat;

	printf("--\n");
	my_rpn_run(*rootrpn);
}

int main(int argc, char *argv[])
{
	struct rpn *rpn = NULL;

	openlog("rpntest", LOG_PERROR, LOG_LOCAL2);
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
		libt_flush();
		sleep(1);
	}
	return 0;
}
