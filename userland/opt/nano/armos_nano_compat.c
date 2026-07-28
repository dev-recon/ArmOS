/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/nano/armos_nano_compat.c
 * Layer: Userland / third-party compatibility
 *
 * Responsibilities:
 * - Provide the minimal login-name fallback required by Nano.
 * - Keep ArmOS-specific compatibility code outside the upstream source tree.
 */

#include <unistd.h>

char *
getlogin(void)
{
    return "user";
}
