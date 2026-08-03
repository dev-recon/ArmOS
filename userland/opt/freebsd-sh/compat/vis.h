#ifndef ARMOS_FREEBSD_SH_VIS_H
#define ARMOS_FREEBSD_SH_VIS_H

#define VIS_NL    0x01
#define VIS_WHITE 0x02

int strvis(char *, const char *, int);
int strunvis(char *, const char *);

#endif
