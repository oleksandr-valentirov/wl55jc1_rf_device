#!/usr/bin/env python3
"""Checks the comment conventions from CLAUDE.md; see that file for the rules."""
import os, re, subprocess, sys

# CubeMX and vendor files: regeneration overwrites them, so the rules cannot hold there.
GENERATED = re.compile(
    r"(^|/)(Drivers/|Middlewares/|cmake/|build/|\.git/|\.venv/|__pycache__/|\.cache/"
    r"|system_stm32wlxx|stm32wlxx_(hal_conf|hal_msp|it|nucleo_conf)"
    r"|syscalls\.c|sysmem\.c|startup_)")
# Vector files are emitted by tools/; the generator is the hand-written artifact.
GENERATED_VEC = re.compile(r"^$")   # the vectors are the hub's; nothing here emits them
PREFIX = {".c": "//", ".h": "//", ".py": "#", ".sh": "#", ".cmake": "#"}

def touched_lines():
    """Line numbers added or changed against HEAD, per file. None = whole file."""
    diff = subprocess.run(["git", "diff", "-U0", "HEAD"],
                          capture_output=True, text=True).stdout
    hits, cur = {}, None
    for line in diff.split("\n"):
        if line.startswith("+++ b/"):
            cur = line[6:]
            hits.setdefault(cur, set())
        elif line.startswith("@@") and cur:
            m = re.search(r"\+(\d+)(?:,(\d+))?", line)
            if m:
                start, count = int(m.group(1)), int(m.group(2) or 1)
                hits[cur].update(range(start, start + count))
    for f in subprocess.run(["git", "ls-files", "--others", "--exclude-standard"],
                            capture_output=True, text=True).stdout.split():
        hits[f] = None
    return hits

def tracked(changed):
    if changed:
        return [f for f in touched_lines() if os.path.isfile(f)]
    return subprocess.run(["git", "ls-files"], capture_output=True,
                          text=True).stdout.split()

def kind(path):
    base = os.path.basename(path)
    if base == ".gitignore" or base == "CMakeLists.txt":
        return "#"
    return PREFIX.get(os.path.splitext(path)[1])

def check(files, only=None):
    long_blocks, own_line, cyrillic = [], [], []
    for p in files:
        if GENERATED.search(p) or GENERATED_VEC.search(p) or not os.path.isfile(p):
            continue
        pfx = kind(p)
        if pfx is None:
            continue
        try:
            text = open(p, errors="replace").read()
        except OSError:
            continue
        if p != "CLAUDE.md" and re.search("[\u0400-\u04ff]", text):
            cyrillic.append(p)
        run, start = [], 0
        for n, line in enumerate(text.split("\n") + [""], 1):
            st = line.strip()
            iscomment = st.startswith(pfx) or (pfx == "//" and
                                               (st.startswith("/*") or st.startswith("*")))
            if iscomment:
                if not run:
                    start = n
                run.append(st)
                continue
            # A documentation path is exempt: CLAUDE.md allows it in full.
            body = re.sub(r"radio_devices_docs/[A-Za-z0-9_./-]+", "", " ".join(run))
            if run and len(body) > 100:
                span = set(range(start, n))
                if only is None or only.get(p) is None or (span & only[p]):
                    long_blocks.append("%s:%d (%d chars)" % (p, start, len(body)))
            run = []
        if p.endswith((".c", ".h")):
            inside = 0
            for n, l in enumerate(text.split("\n"), 1):
                if re.match(r"\s*(typedef\s+)?struct\b.*\{", l):
                    inside = 1
                elif inside and re.match(r"\s*\}", l):
                    inside = 0
                elif inside and re.match(r"\s*(/\*|//|\*)", l):
                    if only is None or only.get(p) is None or n in only[p]:
                        own_line.append("%s:%d" % (p, n))
    return long_blocks, own_line, cyrillic

def main():
    changed = "--changed" in sys.argv
    files = tracked(changed)
    longb, own, cyr = check(files, touched_lines() if changed else None)
    scope = "lines changed against HEAD" if changed else "all tracked files"
    print("scope: %s (%d), generated and vendored excluded\n" % (scope, len(files)))
    for title, items in (("non-English outside CLAUDE.md", cyr),
                         ("comment blocks over 100 characters", longb),
                         ("struct-field comments on their own line", own)):
        print("== %s: %d ==" % (title, len(items)))
        for i in items[:15]:
            print("   " + i)
        if len(items) > 15:
            print("   ... and %d more" % (len(items) - 15))
    return 1 if (longb or own or cyr) else 0

sys.exit(main())
