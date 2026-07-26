/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/stdlib.h
 * Layer: Userland / POSIX compatibility
 * Description: Newlib stdlib wrapper exposing ArmOS porting extensions.
 */

#ifndef ARMOS_STDLIB_H
#define ARMOS_STDLIB_H

#include_next <stdlib.h>

const char *getprogname(void);
int posix_openpt(int flags);
int grantpt(int fd);
int unlockpt(int fd);
char *ptsname(int fd);

#endif /* ARMOS_STDLIB_H */
