#!/usr/bin/env python3
"""Checks the comment conventions from CLAUDE.md; see that file for the rules."""
import io, os, re, subprocess, sys, tokenize

# CubeMX and vendor files: regeneration overwrites them, so the rules cannot hold there.
GENERATED = re.compile(
    r"(^|/)(Drivers/|Middlewares/|cmake/|build/|third_party/|\.git/|\.venv/|__pycache__/|\.cache/"
    r"|system_stm32wlxx|stm32wlxx_(hal_conf|hal_msp|it|nucleo_conf)"
    r"|syscalls\.c|sysmem\.c|startup_)")
# Vector files are emitted by tools/; the generator is the hand-written artifact.
GENERATED_VEC = re.compile(r"^$")   # the vectors are the hub's; nothing here emits them
PREFIX = {".c": "//", ".h": "//", ".py": "#", ".sh": "#", ".cmake": "#",
          ".ld": "/*"}

# /* */ blocks. Only C treats a leading * as a continuation; in a linker script
# that character opens a wildcard.
CSTYLE = {".c", ".h", ".ld"}

# Typography an English page uses; other non-ASCII is prose in another language.
ALLOWED_NON_ASCII = set("\u2014\u2013\u2212\u2192\u00a7\u2248\u00d7\u00b1"
                        "\u2264\u2265\u00b2\u00b3\u00b0\u00b5\u03a9\u0394"
                        "\u03c3\u03c7\u03b1")

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
    """Every file a human owns. --changed always saw untracked ones; the full scan did not."""
    if changed:
        return [f for f in touched_lines() if os.path.isfile(f)]
    return subprocess.run(["git", "ls-files", "--cached", "--others",
                           "--exclude-standard"], capture_output=True,
                          text=True).stdout.split()

def kind(path):
    base = os.path.basename(path)
    if base == ".gitignore" or base == "CMakeLists.txt":
        return "#"
    return PREFIX.get(os.path.splitext(path)[1])

def hash_comment_lines(text):
    """Line numbers holding a real # comment. A # inside a string is not one."""
    try:
        toks = tokenize.generate_tokens(io.StringIO(text).readline)
        return {t.start[0] for t in toks if t.type == tokenize.COMMENT}
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return None

def handwritten(text):
    """Line numbers a human owns. In a CubeMX file that is the USER CODE regions only."""
    lines = text.split("\n")
    if "USER CODE BEGIN" not in text:
        return None
    own, inside = set(), False
    for n, l in enumerate(lines, 1):
        if "USER CODE BEGIN" in l:
            # A "Header" region holds the banner CubeMX writes and rewrites.
            inside = "USER CODE BEGIN Header" not in l
        elif "USER CODE END" in l:
            inside = False
        elif inside:
            own.add(n)
    return own

def check(files, only=None):
    long_blocks, own_line, foreign, long_brief = [], [], [], []
    for p in files:
        if GENERATED.search(p) or GENERATED_VEC.search(p) or not os.path.isfile(p):
            continue
        try:
            text = open(p, errors="replace").read()
        except OSError:
            continue
        # Before the prefix lookup: .md and .ld have none, and were never checked.
        if p != "CLAUDE.md":
            bad = sorted({c for c in text if ord(c) > 127} - ALLOWED_NON_ASCII)
            if bad:
                foreign.append("%s (%s)" % (p, " ".join(bad)[:40]))
        pfx = kind(p)
        if pfx is None:
            continue
        mine = handwritten(text)
        # Shell and CMake are not tokenizable; only Python gets the string check.
        hashes = hash_comment_lines(text) if p.endswith(".py") else None
        cstyle = os.path.splitext(p)[1] in CSTYLE
        star_cont = pfx == "//"
        run, start, incomment = [], 0, False
        for n, line in enumerate(text.split("\n") + [""], 1):
            st = line.strip()
            # A USER CODE marker is structure, not a comment: it must not open a block.
            marker = "USER CODE BEGIN" in st or "USER CODE END" in st
            iscomment = not marker and (st.startswith(pfx) or (cstyle and
                                        st.startswith("/*")) or (star_cont and
                                        st.startswith("*")))
            # A /* */ body counts to its close, or a continuation dodges the limit
            # by not opening with a star.
            if cstyle:
                if incomment:
                    iscomment = True
                if iscomment and "/*" in st and "*/" not in st.split("/*", 1)[1]:
                    incomment = True
                elif incomment and "*/" in st:
                    incomment = False
            if iscomment and hashes is not None and n not in hashes:
                iscomment = False
            if iscomment:
                if not run:
                    start = n
                run.append(st)
                continue
            # A documentation path is exempt: CLAUDE.md allows it in full.
            body = re.sub(r"radio_devices_docs/[A-Za-z0-9_./-]+", "", " ".join(run))
            span = set(range(start, n))
            # CubeMX writes and rewrites this banner; the .ld has no markers.
            vendor = len(run) > 1 and run[0] == "/*" and run[1].startswith("**")
            mineok = run and not vendor and (mine is None or start in mine)
            onlyok = only is None or only.get(p) is None or (span & only[p])
            # A Doxygen block's limit is per @brief, not per block.
            if mineok and run[0].startswith("/**") and not run[0].startswith("/**<"):
                for off, line in enumerate(run):
                    m = re.search(r"@brief\s+(.*)", line)
                    if m and len(m.group(1).rstrip("*/ ")) > 100 and onlyok:
                        long_brief.append("%s:%d (%d chars)"
                                          % (p, start + off, len(m.group(1).rstrip("*/ "))))
            elif mineok and len(body) > 100 and onlyok:
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
                    if mine is not None and n not in mine:
                        continue
                    if only is None or only.get(p) is None or n in only[p]:
                        own_line.append("%s:%d" % (p, n))
    return long_blocks, own_line, foreign, long_brief

# The library-to-be's include list, pinned per file. ROADMAP items 76 and 82.
PORTABLE = {
    "Core/Src/exchange.c":   set(),
    "Core/Src/hop.c":        {"hop.h", "radio_phy.h", "crypto.h"},
    "Core/Src/beacon.c":     {"beacon.h", "radio_phy.h", "radio_protocol.h", "radio_slots.h"},
    "Core/Src/hublogic.c":   {"hublogic.h", "phy.h", "crypto.h", "exchange.h",
                              "radio_protocol.h", "radio_slots.h"},
    "Core/Src/superframe.c": {"superframe.h", "timebase.h"},
    "Core/Inc/exchange.h":   {"radio_protocol.h"},
    "Core/Inc/beacon.h":     {"superframe.h", "radio_slots.h"},
    "Core/Inc/superframe.h": {"radio_slots.h"},
    "Core/Inc/hublogic.h":   set(),
    "Core/Inc/hop.h":        set(),
}
# Freestanding only; <stdio.h> pulls newlib into a file with no part yet.
PORTABLE_ANGLE = {"stddef.h", "stdint.h", "stdbool.h", "string.h", "limits.h"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
# Tracked debts. Each is a tripwire: it fails once the debt is paid.
PORTABLE_OWED = {("Core/Inc/exchange.h", "sha256.h"): "item 82"}


def check_portable():
    """The include list of the library-to-be; ROADMAP items 76 and 82.

    Why each file's list is what it is, and why the list is per file rather than
    a union, is radio_devices_docs ADR-0028 and ADR-0029.

    A missing file is a failure, not a skip: a file renamed out from under this
    list is otherwise indistinguishable from a file that passes."""
    present = [p for p in PORTABLE if os.path.isfile(p)]
    # None present is another tree; some present is a file renamed away.
    if not present:
        return []
    bad, seen = [], set()
    for path, allowed in sorted(PORTABLE.items()):
        if not os.path.isfile(path):
            bad.append("%s: MISSING - the list is stale, not the file portable" % path)
            continue
        text = io.open(path, encoding="utf-8", errors="replace").read()
        own = os.path.basename(path).replace(".c", ".h")
        for bracket, name in INCLUDE_RE.findall(text):
            if bracket == "<":
                if name not in PORTABLE_ANGLE:
                    bad.append("%s: <%s> is not freestanding" % (path, name))
            elif name in allowed or name == own:
                pass
            elif (path, name) in PORTABLE_OWED:
                seen.add((path, name))
            else:
                bad.append("%s: \"%s\" is not on this file's list" % (path, name))
    for key, owed in sorted(PORTABLE_OWED.items()):
        if key not in seen and os.path.isfile(key[0]):
            bad.append("%s: \"%s\" is gone - %s is paid, delete the PORTABLE_OWED line"
                       % (key[0], key[1], owed))
    return bad


def main():
    changed = "--changed" in sys.argv
    files = tracked(changed)
    longb, own, bad, brief = check(files, touched_lines() if changed else None)
    scope = "lines changed against HEAD" if changed else "every file a human owns"
    print("scope: %s (%d), generated and vendored excluded\n" % (scope, len(files)))
    # Whole-list even under --changed: a HAL include is wrong either way.
    port = check_portable()
    for title, items in (("non-ASCII outside CLAUDE.md and the allowed typography", bad),
                         ("comment blocks over 100 characters", longb),
                         ("struct-field comments on their own line", own),
                         ("Doxygen @brief over 100 characters", brief),
                         ("includes outside the library-to-be's list (item 76)", port)):
        print("== %s: %d ==" % (title, len(items)))
        for i in items[:15]:
            print("   " + i)
        if len(items) > 15:
            print("   ... and %d more" % (len(items) - 15))
    return 1 if (longb or own or bad or brief or port) else 0

sys.exit(main())
