/* oxbox_tls_config.h: Mbed TLS as the game's update check has it (oxbox_update.c, the XBE section OXUPD): the
installer's choice of options (installer/tls_config.h), less what a check of 128 bytes doesn't need. */
#ifndef OXBOX_GAME_TLS_CONFIG_H
#define OXBOX_GAME_TLS_CONFIG_H
#include "../installer/tls_config.h"
/* (what this machine sends is one short request) */
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#endif
