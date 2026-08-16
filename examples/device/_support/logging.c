/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * _support/logging.c - see logging.h.
 */
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void usbip_log_init(usbip_logger *lg, const char *prefix, int verbose) {
    lg->prefix = prefix;
    lg->verbose = verbose;
    lg->key[0] = lg->first[0] = 0;
    lg->count = 0;
    lg->nbytes = 0;
}

void usbip_log_flush(usbip_logger *lg) {
    if (!lg->count)
        return;
    if (lg->count == 1)
        fprintf(stderr, "[%s] %s\n", lg->prefix, lg->first);
    else if (lg->nbytes)
        fprintf(stderr, "[%s] %s \xc3\x97%d, %lu KiB\n",
                lg->prefix, lg->key, lg->count, lg->nbytes / 1024);
    else
        fprintf(stderr, "[%s] %s \xc3\x97%d\n", lg->prefix, lg->key, lg->count);
    lg->key[0] = 0;
    lg->count = 0;
    lg->nbytes = 0;
}

void usbip_log_emit(usbip_logger *lg, const char *key, const char *text) {
    if (lg->verbose) {                          /* print every event verbatim */
        fprintf(stderr, "[%s] %s\n", lg->prefix, text);
        return;
    }
    if (strcmp(key, lg->key) != 0) {            /* key changed -> flush the previous run */
        usbip_log_flush(lg);
        snprintf(lg->key, sizeof(lg->key), "%s", key);
        snprintf(lg->first, sizeof(lg->first), "%s", text);
    }
    lg->count++;
    const char *paren = strrchr(text, '(');     /* sum a trailing "(NNNB)" byte count */
    if (paren && strstr(paren, "B)"))
        lg->nbytes += strtoul(paren + 1, NULL, 10);
}

void usbip_log_cb(void *logger, const char *text) {
    usbip_logger *lg = logger;
    /* default key: the text up to the first " (" (keeps the byte count out of the key) */
    const char *paren = strstr(text, " (");
    size_t key_len = paren ? (size_t)(paren - text) : strlen(text);
    char key[sizeof(lg->key)];
    if (key_len >= sizeof(key))
        key_len = sizeof(key) - 1;
    memcpy(key, text, key_len);
    key[key_len] = 0;
    usbip_log_emit(lg, key, text);
}
