/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/sys/ioctl.h
 * Layer: Userland / public header
 * Description: Userspace ABI or library declarations for ArmOS programs.
 */

#ifndef ARM_OS_NEWLIB_SYS_IOCTL_H
#define ARM_OS_NEWLIB_SYS_IOCTL_H

#define TCGETS  0x5401
#define TCSETS  0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TCFLSH  0x540B

#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCSCTTY  0x540E
#define TIOCGPTN   0x80045430u
#define TIOCSPTLCK 0x40045431u

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int ioctl(int fd, unsigned long request, ...);

#endif
