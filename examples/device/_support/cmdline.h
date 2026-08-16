/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * _support/cmdline.h - common command-line parsing for the device examples.
 *
 * Examples share a standard option core (--vid/--pid/--host/--port/--verbose and
 * optionally --high-speed) and add their own. Describe the example in one
 * cmdline_spec - its default ids, its own options, and the callback that stores
 * them - and cmdline_parse() returns what the user asked for:
 *
 *     static const cmdline_option options[] = {
 *         {"size", CMDLINE_ARG_REQUIRED, 's', "N[K|M|G]", "disk size (default 8M)"},
 *         {0, 0, 0, 0, 0},
 *     };
 *
 *     cmdline_opts cli_opts = cmdline_parse(argc, argv, &(cmdline_spec){
 *         .vid = 0x1209,
 *         .pid = 0x0003,
 *         .high_speed_opt = 1,
 *         .options = options,
 *         .on_opt = on_opt,
 *         .user = &cli,
 *     });
 *
 * Each option is described once: the same table drives getopt and prints --help.
 */
#ifndef EXAMPLE_CMDLINE_H
#define EXAMPLE_CMDLINE_H

#include <stdint.h>

#define CMDLINE_ARG_NONE     0   /* --flag                  */
#define CMDLINE_ARG_REQUIRED 1   /* --opt VALUE             */
#define CMDLINE_ARG_OPTIONAL 2   /* --opt, or --opt=VALUE   */

/* What the user asked for - the common core, filled in by cmdline_parse(). */
typedef struct {
    uint16_t vid, pid;
    const char *host;          /* bind address */
    int port;
    int verbose;
    int high_speed;
} cmdline_opts;

/* One example-specific option, described for getopt and for --help alike. `arg` is
 * how its argument is spelled in the help line ("N[K|M|G]"), NULL for a flag; in
 * `help`, a '\n' starts a continuation line. A NULL name terminates the table. */
typedef struct {
    const char *name;          /* long name, without the leading -- */
    int has_arg;               /* CMDLINE_ARG_* */
    int val;                   /* the short letter, or >= 256 for a long-only option */
    const char *arg;
    const char *help;
} cmdline_option;

/* What the example is: its ids, its options, and where to put them. */
typedef struct {
    uint16_t vid, pid;         /* default ids (host/port default to 0.0.0.0:3240) */
    int high_speed_opt;        /* nonzero: also offer the --high-speed common option */
    const char *notes;         /* one line of prose under the synopsis, or NULL */
    const cmdline_option *options;  /* the example's own options, or NULL */
    int (*on_opt)(int c, char *arg, void *user);   /* nonzero if it consumed c */
    void *user;                /* handed back to on_opt */
} cmdline_spec;

/* strtol base-0 (accepts 0x..) into a u16 - for --vid/--pid and example ids. */
uint16_t cmdline_parse_u16(const char *s);
/* parse a size with an optional K/M/G suffix (e.g. "64M") - for --size etc. */
uint64_t cmdline_parse_size(const char *s);

/* Parse argv against `spec`. The letters V P H p v h are the common core's (and S
 * when .high_speed_opt is set), so an example's options must not reuse them. Exits
 * on --help (0) or a bad option (2). */
cmdline_opts cmdline_parse(int argc, char **argv, const cmdline_spec *spec);

#endif /* EXAMPLE_CMDLINE_H */
