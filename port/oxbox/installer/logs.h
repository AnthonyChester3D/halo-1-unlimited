/* logs.h: "Send logs" (docs/log_uploads.md): Halo 1 Unlimited's logs and the installer's packed into one bundle (OXLG)
and sent to the log uploads' address, when the player chooses to: nothing is ever sent otherwise */
#ifndef OXBOX_LOGS_H
#define OXBOX_LOGS_H

#include "download.h"

/* where they go (developer.ini's logs_url takes its place; empty: the menu item isn't shown) */
#define LOGS_DEFAULT_URL ""

#define LOGS_SOURCE_GAME 1
#define LOGS_SOURCE_INSTALLER 2

/* the logs packed and sent: install, Halo 1 Unlimited's folder ("E:\Games\Halo 1 Unlimited"; NULL: none); own_log, the
installer's log; build and installer_version, what's installed and this installer. 0 when sent, with the bundle's code
(8 letters and digits, code[9]) and *files and *kilobytes (what went), else why not in error (256 bytes) */
int logs_send(char const *url, char const *install, char const *own_log, unsigned long build, unsigned long installer_version,
	char *code, int *files, unsigned long *kilobytes, struct download_report const *report, char *error);

#endif
