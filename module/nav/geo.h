/* Local tangent-plane conversion (equirectangular around an origin).
 * Error is negligible over a drone's range (a few km). */
#ifndef NAV_GEO_H
#define NAV_GEO_H

#include <math.h>
#include <stdbool.h>

#define GEO_R 6371008.8  /* mean Earth radius, m */

typedef struct {
    bool   valid;
    double lat, lon, alt;   /* deg, deg, m */
} geo_origin_t;

static inline void geo_to_enu(const geo_origin_t *o, double lat, double lon, double alt, double e[3])
{
    double d2r = M_PI / 180.0;
    e[0] = (lon - o->lon) * d2r * GEO_R * cos(o->lat * d2r);
    e[1] = (lat - o->lat) * d2r * GEO_R;
    e[2] = alt - o->alt;
}

static inline void geo_from_enu(const geo_origin_t *o, const double e[3], double *lat, double *lon, double *alt)
{
    double r2d = 180.0 / M_PI;
    *lat = o->lat + e[1] / GEO_R * r2d;
    *lon = o->lon + e[0] / (GEO_R * cos(o->lat * M_PI / 180.0)) * r2d;
    *alt = o->alt + e[2];
}

#endif
