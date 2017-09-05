#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <syslog.h>

#include "rpnlogic.h"

static struct stack rpnstack;

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
void rpn_run_again(void *dat)
{
}

int main(int argc, char *argv[])
{
	struct rpn *rpn;
	char *input;
	int j;

	openlog("rpntest", LOG_PERROR, LOG_LOCAL2);
	input = argv[1];
	rpn = rpn_parse(input, NULL);
	if (!rpn)
		return 1;
	if (rpn_run(&rpnstack, rpn))
		return 1;
	for (j = 0; j < rpnstack.n; ++j)
		printf("[%i] %s\n", j, rpn_dtostr(rpnstack.v[j]));
	return 0;
}
