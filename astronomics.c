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

struct sunpos sun_pos_strous(time_t t, double lat, double lon)
{
	/* https://www.aa.quae.nl/en/reken/zonpositie.html */

	/* julian day relative to 1jan2000,12h */
	double J = (t - t_1jan2000_12h)/86400.0 + 0.0008;

	/* mean anomaly of the sun */
	/* M = M0 + M1 * (J-J2000) */
#define M0 357.5291
#define M1 0.98560028
	double M = fmod(M0 + M1 * J, 360);

	/* Equation of Center */
	double C = 1.9148*sin(torad(M)) + 0.0200*sin(torad(2*M)) + 0.0003*sin(torad(3*M));

	/* Ecliptic longitude & obliquity */
#define majorPI 102.9373
	double lambda = M + majorPI + C + 180;

	#define epsilon 23.4393
	double alpha = todeg(atan2(sin(torad(lambda))*cos(torad(epsilon)), cos(torad(lambda))));
	double delta = todeg(asin(sin(torad(lambda))*sin(torad(epsilon))));

	/* sidereal time */
	double theta = fmod(280.1470 + 360.9856235 * J + lon, 360);

	/* hour angle */
	double H = fmod(theta - alpha, 360);
	/* alt */
	double alt = asin(sin(torad(lat))*sin(torad(delta)) + cos(torad(lat))*cos(torad(delta))*cos(torad(H)));

	/* azimuth */
	double az = atan2(sin(torad(H)), cos(torad(H))*sin(torad(lat)) - tan(torad(delta))*cos(torad(lat)));

	struct sunpos result = {
		.elevation = todeg(alt),
		.azimuth = todeg(az),
	};

#define J0 0.0009
#define J1 0.0053
#define J2 -0.0068
#define J3 1.0
	double nx = (J - J0)/J3 + lon/360;
	double Jx = J + J3 * -fmod(nx, 1);
	double Mx = fmod(M0 + M1 * Jx, 360);
	double Lsunx = Mx + majorPI + 180;
	double Jtransit = Jx + J1*sin(torad(Mx))+J2*sin(torad(2*Lsunx));
	printf("%lf, %lf, %lf\n", J, Jx, Jtransit);
#define h0 -0.83

	double Ht = acos((sin(torad(h0))-sin(torad(lat))*sin(torad(delta)))/(cos(torad(lat))*cos(torad(delta))));
	printf("Ht %lf\n", Ht);
	double Jrise = Jtransit - (todeg(Ht)/360)*J3;
	double Jset = Jtransit + (todeg(Ht)/360)*J3;

	result.sunnoon = t + (Jtransit-J)*86400;
	result.sunrise = t + (Jrise-J)*86400;
	result.sunset = t + (Jset-J)*86400;

	return result;
}
