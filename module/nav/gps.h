/* NMEA reader for the NEO-6M (GGA + RMC), non-blocking, on a UART.
 * The GPS is optional: it may be absent, have no fix, or go silent mid-flight
 * (seen on the bench: ~10 s silences, probably the module resetting). */
#ifndef NAV_GPS_H
#define NAV_GPS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* from GGA */
    int      quality;      /* 0 none, 1 GPS, 2 DGPS, ... */
    int      sats;
    double   hdop;
    double   lat, lon;     /* deg */
    double   alt;          /* m above MSL */
    /* from RMC */
    bool     rmc_valid;    /* status 'A' */
    double   speed;        /* m/s */
    double   course;       /* deg true */
    int      year, month, day, hour, min;
    double   sec;
    /* bookkeeping */
    uint64_t gga_t_ns;     /* CLOCK_MONOTONIC when the GGA line arrived */
    uint64_t rmc_t_ns;
    uint64_t last_byte_ns;
    unsigned sentences, bad_checksum;
} gps_fix_t;

typedef struct {
    int       fd;
    char      line[128];
    unsigned  len;
    gps_fix_t fix;
    unsigned  new_gga, new_rmc;   /* incremented on each parsed sentence; caller may reset */
} gps_t;

/* Opens the UART (9600 8N1, raw, non-blocking). Returns 0 or -errno. */
int  gps_open(gps_t *g, const char *dev);
/* Reads whatever is pending and parses complete lines. Never blocks. */
void gps_poll(gps_t *g);
void gps_close(gps_t *g);

/* Parser, exposed for tests: feeds one full sentence (with or without CR/LF). */
bool gps_parse_sentence(gps_t *g, const char *s, uint64_t t_ns);

#endif
