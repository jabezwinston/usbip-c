/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * _support/cmdline.c - see cmdline.h.
 */
#include "cmdline.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OPTS    64   /* the example's options plus the common core */
#define HELP_COLUMN 25   /* where an option's description starts */

uint16_t cmdline_parse_u16(const char *s) { return (uint16_t)strtol(s, NULL, 0); }

uint64_t cmdline_parse_size(const char *s)
{
    char *end;
    uint64_t v = strtoull(s, &end, 10);
    switch (*end)
    { /* optional K/M/G suffix */
    case 'k':
    case 'K':
        v <<= 10;
        break;
    case 'm':
    case 'M':
        v <<= 20;
        break;
    case 'g':
    case 'G':
        v <<= 30;
        break;
    }
    return v;
}

/* The common core, listed after the example's own options. --high-speed is dropped
 * unless the example asked for it. */
static const cmdline_option core_opts[] = {
    {"vid", CMDLINE_ARG_REQUIRED, 'V', "V", "USB vendor id (0x-hex ok)"},
    {"pid", CMDLINE_ARG_REQUIRED, 'P', "P", "USB product id (0x-hex ok)"},
    {"host", CMDLINE_ARG_REQUIRED, 'H', "H", "bind address (default 0.0.0.0)"},
    {"port", CMDLINE_ARG_REQUIRED, 'p', "N", "TCP port (default 3240)"},
    {"high-speed", CMDLINE_ARG_NONE, 'S', NULL, "report a high-speed device (default: full speed)"},
    {"verbose", CMDLINE_ARG_NONE, 'v', NULL, "log every event (default: grouped summaries)"},
    {"help", CMDLINE_ARG_NONE, 'h', NULL, "show this help"},
    {0, 0, 0, 0, 0},
};

/* One table of every option this run offers - it drives getopt and --help alike. */
static int merge_opts(const cmdline_spec *spec, cmdline_option *all)
{
    int n = 0;

    for (int i = 0; spec->options && spec->options[i].name && n < MAX_OPTS; i++)
        all[n++] = spec->options[i];

    for (int i = 0; core_opts[i].name && n < MAX_OPTS; i++)
    {
        if (core_opts[i].val == 'S' && !spec->high_speed_opt)
            continue;
        all[n++] = core_opts[i];
    }
    return n;
}

/* Spell the merged table the two ways getopt_long() wants it. */
static void build_getopt(const cmdline_option *all, int n, struct option *longs, char *shorts)
{
    size_t s = 0;

    for (int i = 0; i < n; i++)
    {
        longs[i] = (struct option){all[i].name, all[i].has_arg, 0, all[i].val};

        if (all[i].val <= 0 || all[i].val > 127) /* long-only: no short letter */
            continue;

        shorts[s++] = (char)all[i].val;

        if (all[i].has_arg != CMDLINE_ARG_NONE)
            shorts[s++] = ':';

        if (all[i].has_arg == CMDLINE_ARG_OPTIONAL)
            shorts[s++] = ':';
    }
    longs[n] = (struct option){0, 0, 0, 0};
    shorts[s] = 0;
}

/* "  --size N[K|M|G]        disk size (default 8M)", continuation lines aligned. */
static void print_opt(FILE *out, const cmdline_option *o)
{
    int col = fprintf(out, "  --%s", o->name);

    if (o->arg && o->has_arg == CMDLINE_ARG_OPTIONAL)
        col += fprintf(out, "%s", o->arg); /* must be attached: --floppy[=FORMAT] */
    else if (o->arg)
        col += fprintf(out, " %s", o->arg);

    if (col < HELP_COLUMN)
        fprintf(out, "%*s", HELP_COLUMN - col, "");
    else
        fputc(' ', out); /* an option too wide for the column still gets a space */

    for (const char *p = o->help ? o->help : ""; *p; p++)
    {
        fputc(*p, out);

        if (*p == '\n')
            fprintf(out, "%*s", HELP_COLUMN, "");
    }
    fputc('\n', out);
}

static void print_usage(FILE *out, const char *argv0, const char *notes,
                        const cmdline_option *all, int n)
{
    fprintf(out, "usage: %s [options]\n", argv0);

    if (notes && *notes)
        fprintf(out, "  %s\n", notes);

    for (int i = 0; i < n; i++)
        print_opt(out, &all[i]);
}

cmdline_opts cmdline_parse(int argc, char **argv, const cmdline_spec *spec)
{
    cmdline_opts o = {
        .vid  = spec->vid,
        .pid  = spec->pid,
        .host = "0.0.0.0",
        .port = 3240,
    };
    cmdline_option all[MAX_OPTS];
    struct option longs[MAX_OPTS + 1];
    char shorts[MAX_OPTS * 3 + 1];
    int n = merge_opts(spec, all);

    build_getopt(all, n, longs, shorts);

    int opt;
    while ((opt = getopt_long(argc, argv, shorts, longs, NULL)) != -1)
    {
        if (opt == 'V')
        {
            o.vid = cmdline_parse_u16(optarg);
            continue;
        }
        if (opt == 'P')
        {
            o.pid = cmdline_parse_u16(optarg);
            continue;
        }
        if (opt == 'H')
        {
            o.host = optarg;
            continue;
        }
        if (opt == 'p')
        {
            o.port = atoi(optarg);
            continue;
        }
        if (opt == 'v')
        {
            o.verbose = 1;
            continue;
        }
        if (opt == 'h')
        {
            print_usage(stdout, argv[0], spec->notes, all, n);
            exit(0);
        }
        if (opt == 'S' && spec->high_speed_opt)
        {
            o.high_speed = 1;
            continue;
        }
        if (opt == '?')
        {
            print_usage(stderr, argv[0], spec->notes, all, n);
            exit(2);
        }
        if (spec->on_opt && spec->on_opt(opt, optarg, spec->user))
            continue;

        print_usage(stderr, argv[0], spec->notes, all, n);
        exit(2);
    }
    return o;
}
