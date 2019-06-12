#include <time.h>

#ifndef _astronomics_h_
#define _astronomics_h_

#ifdef __cplusplus
extern "C" {
#endif

struct sunpos {
	double azimuth;
	double elevation;
	time_t sunrise, sunset;
	time_t sunnoon;
};

extern struct sunpos sun_pos_strous(time_t t, double lat, double lon);

#ifdef __cplusplus
}
#endif
#endif
