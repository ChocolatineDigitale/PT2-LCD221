#!/usr/bin/env python3
"""Makes a release copy of the built watch face without anything that identifies
the build machine.

The SDK puts a debugging source map in the app file, and its helper files carry the
path of the SDK on the build computer (with the user name in it). The map is only
for debugging, so this drops it, then scans every remaining file for home-directory
paths and any extra words given with --deny, and fails if it finds one.

    python3 tools/strip_pbw.py                      # build/PebbleFace1.pbw -> build/release.pbw
    python3 tools/strip_pbw.py IN.pbw OUT.pbw --deny SomeName --deny another
"""
import argparse
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOME_PATH = re.compile(rb"/(?:home|Users)/[A-Za-z0-9._-]+|[A-Za-z]:\\Users\\[A-Za-z0-9._-]+")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", nargs="?", default=str(ROOT / "build/PebbleFace1.pbw"))
    ap.add_argument("target", nargs="?", default=str(ROOT / "build/release.pbw"))
    ap.add_argument("--deny", action="append", default=[], help="a word that must not appear (repeatable)")
    args = ap.parse_args()

    kept, dropped = [], []
    with zipfile.ZipFile(args.source) as zin, zipfile.ZipFile(args.target, "w", zipfile.ZIP_DEFLATED) as zout:
        for info in zin.infolist():
            if info.filename.endswith(".map"):
                dropped.append(info.filename)
                continue
            zout.writestr(info, zin.read(info.filename))
            kept.append(info.filename)

    problems = []
    deny = [d.lower().encode() for d in args.deny]
    with zipfile.ZipFile(args.target) as z:
        for name in kept:
            data = z.read(name)  # binary files (the app, its resources) are scanned too
            for m in HOME_PATH.finditer(data):
                problems.append("%s: %s" % (name, m.group(0).decode(errors="replace")))
            lower = data.lower()
            for d in deny:
                if d in lower:
                    problems.append("%s: contains %r" % (name, d.decode()))

    print("wrote %s (%d files; dropped %s)" % (args.target, len(kept), ", ".join(dropped) or "nothing"))
    if problems:
        print("FOUND identifying text:")
        for p in problems:
            print("  " + p)
        return 1
    print("clean: no home-directory paths%s" % (" or denied words" if deny else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
