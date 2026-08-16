/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * _support/logging.h - grouped event logger shared by the device examples.
 *
 * Collapses consecutive identical events into one summary line (e.g.
 * "READ(10) x42, 168 KiB") so a high-traffic device doesn't flood the terminal;
 * verbose mode prints every event verbatim. The grouping key defaults to the
 * text up to the first " (" - so a trailing "(NNNB)" byte count is summed across
 * the run rather than treated as part of the key. Callers that group by
 * something else (e.g. msc, by lba) supply their own key via usbip_log_emit().
 */
#ifndef EXAMPLE_LOGGING_H
#define EXAMPLE_LOGGING_H

typedef struct {
    const char *prefix;          /* printed as "[prefix] ..." (e.g. "msc") */
    int verbose;                 /* 1: print every event; 0: group identical runs */
    char key[96], first[160];    /* current group key + first full line of the run */
    int count;                   /* events in the current run */
    unsigned long nbytes;        /* bytes summed from "(NNNB)" suffixes in the run */
} usbip_logger;

/* Initialize a logger (clears the run state). */
void usbip_log_init(usbip_logger *lg, const char *prefix, int verbose);

/* Default event sink, shaped like the class on_event/on_command callbacks: pass the
 * usbip_logger* as their `user`. Groups by the text up to the first " (". */
void usbip_log_cb(void *logger, const char *text);

/* Like usbip_log_cb but with a caller-supplied grouping key, for examples whose
 * key isn't "text up to ' ('" (e.g. msc grouping reads/writes by command). */
void usbip_log_emit(usbip_logger *lg, const char *key, const char *text);

/* Flush any pending grouped summary (call before exit / when a stream stops). */
void usbip_log_flush(usbip_logger *lg);

#endif /* EXAMPLE_LOGGING_H */
