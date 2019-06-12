#include <math.h>
#include <time.h>

#include "astronomics.h"

static inline double torad(double deg)
{
	return deg*M_PI_2/90;
}
static inline double todeg(double rad)
{
	return rad*90/M_PI_2;
}

static const time_t t_1jan2000_12h = 946728000;
double julian_day(time_t t)
{
	/* from wikipedia
	 * https://en.wikipedia.org/wiki/Julian_day
	 */
	return 2451545 + (t-t_1jan2000_12h)/86400.0;
}

double sun_pos_strous(time_t t, double lat, double lon)
{
	/* https://www.aa.quae.nl/en/reken/zonpositie.html */

	/* julian day relative to 1jan2000,12h */
	double J = (t - t_1jan2000_12h)/86400.0 + 0.0008;

	/* mean anomaly of the sun */
	/* M = M0 + M1 * (J-J2000) */
	double M = fmod(357.5291 + 0.98560028 * J, 360);

	/* Equation of Center */
	double C = 1.9148*sin(torad(M)) + 0.0200*sin(torad(2*M)) + 0.0003*sin(torad(3*M));

	/* Ecliptic longitude & obliquity */
	double lambda = M + 102.9373 + C + 180;

	#define epsilon 23.4393
	double alpha = todeg(atan2(sin(torad(lambda))*cos(torad(epsilon)), cos(torad(lambda))));
	double delta = todeg(asin(sin(torad(lambda))*sin(torad(epsilon))));

	/* sidereal time */
	double theta = fmod(280.1470 + 360.9856235 * J + lon, 360);

	/* hour angle */
	double H = fmod(theta - alpha, 360);
	/* alt */
	double alt = asin(sin(torad(lat))*sin(torad(delta)) + cos(torad(lat))*cos(torad(delta))*cos(torad(H)));

	return todeg(alt);
}
