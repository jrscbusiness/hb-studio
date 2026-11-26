#!/usr/bin/env python3
"""Assemble ui/src/ into the single-file ui/index.html (generated artifact, committed).

Usage (stdlib only, run isolated):
    python3 -I tools/build_ui.py            # write ui/index.html
    python3 -I tools/build_ui.py --check    # exit 1 if ui/index.html is not what ui/src/ produces

Source layout (see docs/CONTRIBUTING.md):
    ui/src/index.template.html   document shell; every line of the form
                                 `<!-- @include path/to/file -->` (column 0) is replaced by that file
    ui/src/css|views|js/...      partials; they may include other partials the same way
    ui/src/fonts/*.woff2         binary fonts; `@base64(fonts/x.woff2)` inside any text is replaced by
                                 the file's base64 (no line wrapping)

Rules that keep the output deterministic and byte-exact:
  * partials are UTF-8, LF only, and end with exactly one trailing newline; an include replaces the
    marker line with the partial minus that final newline;
  * include order in the template IS the CSS cascade order and the script execution order (the script
    is one IIFE: all js/*.js partials share its scope, so a partial may use `const`s of earlier ones);
  * every file under ui/src/{css,views,js,fonts} must be referenced, otherwise the build fails
    (an orphan file would silently not ship).
"""
import base64
import difflib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "ui", "src")
OUT = os.path.join(ROOT, "ui", "index.html")
TEMPLATE = "index.template.html"
INCLUDE_RE = re.compile(r"^<!-- @include (\S+) -->$")
B64_RE = re.compile(r"@base64\(([^)\s]+)\)")
SCAN_DIRS = ("css", "views", "js", "fonts")


class BuildError(Exception):
    pass


def _resolve(rel):
    p = os.path.normpath(os.path.join(SRC, rel))
    if not p.startswith(SRC + os.sep):
        raise BuildError("path escapes ui/src: " + rel)
    return p


def _read_text(rel):
    try:
        with open(_resolve(rel), "rb") as f:
            raw = f.read()
    except OSError as e:
        raise BuildError("cannot read %s: %s" % (rel, e))
    if b"\r" in raw:
        raise BuildError(rel + ": contains CR; use LF newlines")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        raise BuildError(rel + ": not valid UTF-8")


def _expand(rel, used, stack):
    if rel in stack:
        raise BuildError("include cycle: " + " -> ".join(stack + [rel]))
    used.add(rel)
    text = _read_text(rel)
    if rel != TEMPLATE and not text.endswith("\n"):
        raise BuildError(rel + ": must end with a newline")
    out = []
    for line in text.split("\n"):
        m = INCLUDE_RE.match(line)
        if m:
            sub = _expand(m.group(1), used, stack + [rel])
            out.append(sub[:-1])  # drop the partial's final LF; the marker line's own LF stays
        else:
            out.append(line)
    return "\n".join(out)


def _inline_base64(text, used):
    def sub(m):
        rel = m.group(1)
        try:
            with open(_resolve(rel), "rb") as f:
                data = f.read()
        except OSError as e:
            raise BuildError("cannot read %s: %s" % (rel, e))
        used.add(rel)
        return base64.b64encode(data).decode("ascii")

    return B64_RE.sub(sub, text)


def assemble():
    used = set()
    html = _inline_base64(_expand(TEMPLATE, used, []), used)
    orphans = []
    for d in SCAN_DIRS:
        for dirpath, _dirs, files in os.walk(os.path.join(SRC, d)):
            for fn in files:
                rel = os.path.relpath(os.path.join(dirpath, fn), SRC).replace(os.sep, "/")
                if rel not in used:
                    orphans.append(rel)
    if orphans:
        raise BuildError("unreferenced files in ui/src (add an @include / @base64 or delete them):\n  "
                         + "\n  ".join(sorted(orphans)))
    return html.encode("utf-8")


def main(argv):
    if any(a != "--check" for a in argv):
        print("usage: build_ui.py [--check]", file=sys.stderr)
        return 2
    try:
        data = assemble()
    except BuildError as e:
        print("build_ui: " + str(e), file=sys.stderr)
        return 1
    if argv:
        try:
            with open(OUT, "rb") as f:
                cur = f.read()
        except OSError:
            cur = b""
        if cur == data:
            print("build_ui: ui/index.html is up to date")
            return 0
        print("build_ui: ui/index.html is OUT OF DATE vs ui/src/. Run: python3 -I tools/build_ui.py "
              "and commit both.", file=sys.stderr)
        a = cur.decode("utf-8", "replace").splitlines()
        b = data.decode("utf-8", "replace").splitlines()
        for n, l in enumerate(difflib.unified_diff(a, b, "ui/index.html", "assembled", lineterm="", n=0)):
            if n >= 40:
                print("...", file=sys.stderr)
                break
            print(l[:200], file=sys.stderr)
        return 1
    with open(OUT, "wb") as f:
        f.write(data)
    print("build_ui: wrote ui/index.html (%d bytes)" % len(data))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
