#pragma once

/*
 * Where things are in viralOS's filesystem (the kernel and programs both use these, like BSD's and
 * glibc's <paths.h>). The layout follows the usual Unix one:
 *
 *   /usr/bin                   programs                 /etc        system settings
 *   /usr/lib                   shared libraries         /home/NAME  each user's files
 *   /usr/share/applications    app launchers (.desktop) /media      drives (USB sticks...)
 *   /usr/share/dde-res         the desktop's resources  /dev        disks and partitions
 *                              (fonts, icons, sounds,   /tmp        scratch space
 *                              cursors, wallpapers)
 *
 * Paths are without a trailing slash: _PATH_BIN "/" "shell".
 */

#define _PATH_BIN           "/usr/bin"
#define _PATH_LIB           "/usr/lib"
#define _PATH_SHARE         "/usr/share"
#define _PATH_APPLICATIONS  "/usr/share/applications"
#define _PATH_DDE_RES       "/usr/share/dde-res"
#define _PATH_DEV           "/dev"
#define _PATH_DEVNULL       "/dev/null"
#define _PATH_TMP           "/tmp"
#define _PATH_MEDIA         "/media"
#define _PATH_HOME          "/home"
#define _PATH_ETC           "/etc"
#define _PATH_RUN           "/run"                       /* what programs keep while running (sudo's timestamps) */

/* Programs the system starts by name. */
#define _PATH_SHELL         "/usr/bin/shell"             /* the text shell (the fallback first program) */
#define _PATH_DESKTOP       "/usr/bin/divine-desktop"

/* /etc */
#define _PATH_HOSTNAME      "/etc/hostname"
#define _PATH_MOTD          "/etc/motd"
#define _PATH_OS_RELEASE    "/etc/os-release"
#define _PATH_INIT_CONF     "/etc/init"                  /* the first program to start (one line) */
#define _PATH_PASSWD        "/etc/passwd"                /* the accounts */
#define _PATH_SHADOW        "/etc/shadow"                /* their password hashes */
#define _PATH_GROUP         "/etc/group"
#define _PATH_FSTAB         "/etc/fstab"                 /* where drives get mounted (the Disks app) */
#define _PATH_NETWORK_CONF  "/etc/network.conf"          /* network adapters' settings (netctl) */
#define _PATH_DDE_DEFAULTS  "/etc/dde/settings.conf"     /* DDE's settings until a user changes them */

/* In a home folder: DDE's settings for that user (see <config.h>). */
#define _PATH_DDE_USER_DIR  ".config/dde"
