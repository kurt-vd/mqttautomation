#ifndef _RPNLOGIC_H_
#define _RPNLOGIC_H_

struct stack {
	struct rpn_el {
		double d;
		const char *a;
	} *v /* element array */;
	int n; /* used elements */
	int s; /* allocated elements */
	struct rpn *jumpto;
	int errnum;
};

struct rpn {
	struct rpn *next;
	void (*run)(struct stack *st, struct rpn *me);
	int flags;
	void *dat;
	char *topic;
	double value;
	char *strvalue;
	int cookie;
	struct rpn *rpn; /* cached rpn for flow control */
	void (*timeout)(void *dat); /* scheduled timeout,
				       usefull to free resources */
};

/* functions */
int rpn_parse_append(const char *cstr, struct rpn **proot, void *dat);
void rpn_parse_done(struct rpn *root);

struct rpn *rpn_parse(const char *cstr, void *dat);

void rpn_stack_reset(struct stack *st);
int rpn_run(struct stack *st, struct rpn *rpn);

void rpn_free_chain(struct rpn *rpn);
void rpn_rebase(struct rpn *first, struct rpn **newptr);

int rpn_collect_flags(struct rpn *);
#define RPNFN_PERIODIC	1 /* This will generate output without external events */
#define RPNFN_WALLTIME	2 /* depends on wall time */

/* imported function */
extern const char *rpn_lookup_env(const char *str, struct rpn *);
extern int rpn_write_env(const char *value, const char *str, struct rpn *);
extern int rpn_env_isnew(void);
extern void rpn_run_again(void *dat); /* dat is the calling rpn * */

extern double rpn_strtod(const char *str, char **endp);
extern const char *rpn_dtostr(double d);
#endif
