/* ArmOS paths used by the imported FreeBSD shell. */
#ifndef ARMOS_FREEBSD_SH_PATHS_H
#define ARMOS_FREEBSD_SH_PATHS_H

/*
 * Keep mash installed as ArmOS' recovery /bin/sh until the promotion gate is
 * met, but make the port's POSIX ENOEXEC fallback recurse through itself.
 * This is the path used for executable text files without a shebang.
 */
#define _PATH_BSHELL   "/bin/freebsd-sh"
#define _PATH_CONSOLE  "/dev/tty0"
#define _PATH_DEVNULL  "/dev/null"
#define _PATH_TTY      "/dev/tty"
#define _PATH_TMP      "/tmp"
#define _PATH_DEFPATH  "/sbin:/bin:/usr/sbin:/usr/bin:/opt/freebsd-sh/bin"
#define _PATH_STDPATH  _PATH_DEFPATH

#endif
