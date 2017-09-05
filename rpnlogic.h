#ifndef _RPNLOGIC_H_
#define _RPNLOGIC_H_

struct stack {
	double *v; /* element array */
	int n; /* used elements */
	int s; /* allocated elements */
	const char *strvalue;
	struct rpn *jumpto;
};

struct rpn {
	struct rpn *next;
	int (*run)(struct stack *st, struct rpn *me);
	void *dat;
	char *topic;
	double value;
	char *strvalue;
	int cookie;
	struct rpn *rpn; /* cached rpn for flow control */
};

/* functions */
struct rpn *rpn_parse(const char *cstr, void *dat);

void rpn_stack_reset(struct stack *st);
int rpn_run(struct stack *st, struct rpn *rpn);

void rpn_free_chain(struct rpn *rpn);
void rpn_rebase(struct rpn *first, struct rpn **newptr);

/* imported function */
extern const char *rpn_lookup_env(const char *str, struct rpn *);
extern int rpn_write_env(const char *value, const char *str, struct rpn *);
extern int rpn_env_isnew(void);
extern void rpn_run_again(void *dat); /* dat is the calling rpn * */

extern double rpn_strtod(const char *str, char **endp);
extern const char *rpn_dtostr(double d);
#endif
