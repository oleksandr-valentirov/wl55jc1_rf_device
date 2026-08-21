#!/usr/bin/env python3
"""Checks that what this side's documentation names still exists in the code.

Taken from the hub's tools/check_docs.py, which is the authority on the idea.
Two branches differ and both were earned on the first run over these pages;
see the header of tools/docs_allow.txt.

This does not check that a claim is true - no tool can. It checks the weaker
thing that is mechanical: an identifier or a path a page names must still be
findable, because a page describing symbols that no longer exist is a page
nobody can act on.
"""

import os
import re
import subprocess
import sys

DOCS = "../radio_devices_docs"
# radio/ and open_hub/ are checked by the hub's copy, against the hub's tree.
SCOPES = ("wl55_device",)
ALLOW = "tools/docs_allow.txt"
# Symbols owned by somebody else's build; naming them is not a claim about a tree.
FOREIGN = ("mbedtls_", "HAL_", "os", "xQueue", "ExternalProject_", "MX_", "SUBGHZ_")

IDENT = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*)(?:\(\)|\(|`)")
# Live arithmetic is written inside an expression or a fence, not alone in backticks.
# radio_devices_docs/wl55_device/radio/timebase.md
CODE = re.compile(r"```.*?```|`[^`\n]+`", re.S)
# Lowercase calls in fences only: device_id(4) inline is a width, not a call.
FENCE = re.compile(r"```([a-z]*)\n(.*?)```", re.S)
# A shell fence is not C, so a C symbol rule does not apply to what is in it.
NOT_C = ("bash", "sh", "console", "text", "json")
MACRO = re.compile(r"`(RADIO_[A-Z0-9_]+|STORE_[A-Z0-9_]+|BEACON_[A-Z0-9_]+"
                   r"|SUPERFRAME_[A-Z0-9_]+|TLM_[A-Z0-9_]+)`")
# Only a path is a claim about where something lives; a bare file name is prose.
PATH = re.compile(r"`([A-Za-z0-9_][A-Za-z0-9_./-]*/[A-Za-z0-9_.-]*"
                  r"\.(?:c|h|py|sh|md|txt|ld))(?::(\d+))?`")


def load_allow(path):
    """Exemptions as page:name. Both rules are checks, not conventions."""
    exact, bad = set(), []
    if not os.path.exists(path):
        return exact, bad
    for n, raw in enumerate(open(path, encoding="utf-8"), 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        entry, sep, reason = raw.partition("#")
        entry = entry.strip()
        if not sep or not reason.strip():
            bad.append("%s:%d: %s -- no reason given" % (path, n, entry))
        elif ":" not in entry:
            bad.append("%s:%d: %s -- not page:name" % (path, n, entry))
        else:
            exact.add(entry)
    return exact, bad


def source_text():
    """This tree plus the hub's: a shared macro is defined over there."""
    roots = ["."]
    if os.path.isdir("../OpenHub"):
        roots.append("../OpenHub")
    blob = []
    for root in roots:
        files = subprocess.run(["git", "-C", root, "ls-files",
                                "*.c", "*.h", "*.py", "*.sh", "*.txt", "*.ld",
                                "CMakeLists.txt"],
                               capture_output=True, text=True).stdout.split()
        for f in files:
            if any(s in f for s in ("third_party", "/Drivers/", "/Middlewares/")):
                continue
            # Read as source, each would prove its own names.
            if f.endswith(("docs_allow.txt", "test_check_docs.py")):
                continue
            try:
                blob.append(open(os.path.join(root, f),
                                 encoding="utf-8", errors="replace").read())
            except OSError:
                pass
    return "\n".join(blob)


def doc_pages():
    for scope in SCOPES:
        for dirpath, _, names in os.walk(os.path.join(DOCS, scope)):
            for n in sorted(names):
                if n.endswith(".md"):
                    yield os.path.join(dirpath, n)


def parse_args(argv):
    """The corpus runs this against either copy, so the three paths are arguments."""
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--docs", default=DOCS)
    ap.add_argument("--scopes", nargs="+", default=list(SCOPES))
    ap.add_argument("--allow", default=ALLOW)
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    globals()["DOCS"] = args.docs
    globals()["SCOPES"] = tuple(args.scopes)
    if not os.path.isdir(args.docs):
        sys.stderr.write("no %s; nothing to check\n" % args.docs)
        return 0
    exact, bad = load_allow(args.allow)
    words = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", source_text()))

    missing_ident, missing_path, pages = [], [], 0
    for page in doc_pages():
        pages += 1
        text = open(page, encoding="utf-8", errors="replace").read()
        rel = os.path.relpath(page, DOCS)

        def allowed(name):
            return ("%s:%s" % (rel, name)) in exact

        # Prefixed names only: an excerpt's locals resolve anyway.
        for span in CODE.finditer(text):
            body = span.group(0)
            for name in re.findall(r"\b(?:RADIO|STORE|BEACON|SUPERFRAME|TLM"
                                   r"|FRAME|CRYPTO|IPC)_[A-Z0-9_]+\b", body):
                if name not in words and not allowed(name):
                    missing_ident.append("%s: %s" % (rel, name))

        for span in FENCE.finditer(text):
            if span.group(1) in NOT_C:
                continue
            for name in re.findall(r"\b([a-z_][a-z0-9_]{3,})\s*\(", span.group(2)):
                if "_" not in name or name.startswith(FOREIGN):
                    continue
                if name not in words and not allowed(name):
                    missing_ident.append("%s: %s()" % (rel, name))
        for m in MACRO.finditer(text):
            name = m.group(1)
            if name not in words and not allowed(name):
                missing_ident.append("%s: %s" % (rel, name))
        for m in IDENT.finditer(text):
            name = m.group(1)
            if len(name) < 4 or "_" not in name:
                continue
            if name.isupper() or name.startswith(FOREIGN):
                continue
            if name not in words and not allowed(name):
                missing_ident.append("%s: %s()" % (rel, name))
        for m in PATH.finditer(text):
            p = m.group(1)
            if p.endswith(".md") or p.startswith("../") or allowed(p):
                continue
            if not any(os.path.exists(os.path.join(r, p))
                       for r in (".", "..", "../OpenHub")):
                missing_path.append("%s: %s" % (rel, p))

    missing_ident = sorted(set(missing_ident))
    missing_path = sorted(set(missing_path))
    print("scope: %d pages under %s\n" % (pages, "/, ".join(SCOPES) + "/"))
    for title, items in (("named in the docs, absent from the code", missing_ident),
                         ("file paths in the docs that do not resolve", missing_path),
                         ("exemptions this file will not accept", bad)):
        print("== %s: %d ==" % (title, len(items)))
        for i in items:
            print("   " + i)
    return 1 if (missing_ident or missing_path or bad) else 0


if __name__ == "__main__":
    sys.exit(main())
