#!/usr/bin/env python3
# Copyright (C) 2026 Jabez Winston
# SPDX-License-Identifier: MIT
#
# Turn a .syms file (one exported symbol per line, '#' comments) into the
# per-platform export-control file the linker wants:
#   --format map   GNU ld version script      (Linux)
#   --format def   PE module-definition file  (Windows/mingw)
#   --format sym   exported_symbols_list      (macOS ld, '_'-prefixed)
import argparse
import sys


def read_syms(path):
    syms = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                syms.append(line)
    if not syms:
        sys.exit(f"{path}: no symbols found")
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("syms_file")
    ap.add_argument("--format", required=True, choices=["map", "def", "sym"])
    ap.add_argument("--library", required=True,
                    help="library name, e.g. libusbip-device")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    syms = read_syms(args.syms_file)
    out = []
    if args.format == "map":
        node = args.library.upper().replace("-", "_").removeprefix("LIB")
        out.append(f"{node}_0 {{")
        out.append("global:")
        out.extend(f"    {s};" for s in syms)
        out.append("local:")
        out.append("    *;")
        out.append("};")
    elif args.format == "def":
        out.append(f"LIBRARY {args.library}.dll")
        out.append("EXPORTS")
        out.extend(f"    {s}" for s in syms)
    else:
        out.extend(f"_{s}" for s in syms)

    with open(args.output, "w") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
