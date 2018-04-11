#ifndef _common_h_
#define _common_h_
#ifdef __cplusplus
extern "C" {
#endif

/* generic error logging */
#define ESTR(num)	strerror(num)
__attribute__((format(printf,2,3)))
extern void mylog(int loglevel, const char *fmt, ...);
extern void myopenlog(const char *name, int options, int facility);
extern void myloglevel(int loglevel);
extern int mysetloglevelstr(char *str);

#ifdef __cplusplus
}
#endif
#endif
