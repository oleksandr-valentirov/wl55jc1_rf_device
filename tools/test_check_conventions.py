#!/usr/bin/env python3
"""Mutations for check_conventions.py: each arm is a defect it must or must not see.

Cases are built in a throwaway git repository, staged, and checked there.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# Overridable, so the corpus can be run against a deliberately mutated checker.
CHECKER = os.environ.get("CONV_CHECKER", os.path.join(HERE, "check_conventions.py"))

# Code points, not literals: a corpus that spells what it detects trips it.
CYR = "".join(map(chr, (0x43a, 0x438, 0x440, 0x438, 0x43b, 0x438, 0x446, 0x44f)))
HAN = "".join(map(chr, (0x6f22, 0x5b57)))
MICRO = chr(0xb5)
EMDASH = chr(0x2014)

# name, files to write, must the language check fire
ARMS = [
    ("han in .md",        {"page.md": "# T\n\n%s here.\n" % HAN},            True),
    ("cyrillic in .md",   {"page.md": "# T\n\n%s here.\n" % CYR},            True),
    ("cyrillic in .c",    {"a.c": "/* %s */\nint x;\n" % CYR},               True),
    ("cyrillic in .ld",   {"l.ld": "/* %s */\nMEMORY { }\n" % CYR},          True),
    ("cyrillic in .sh",   {"s.sh": "# %s\ntrue\n" % CYR},                    True),
    ("micro sign in .c",  {"a.c": "/* a delay of 700 %ss */\nint y;\n" % MICRO}, False),
    ("em dash in .md",    {"page.md": "# T\n\nA sentence %s and more.\n" % EMDASH},   False),
    ("cyrillic in CLAUDE.md", {"CLAUDE.md": "# Brief\n\n%s\n" % CYR},        False),
    ("clean tree",        {"a.c": "/* plain */\nint z;\n"},                  False),
]


# name, files, must the block-length check fire
BLOCK_ARMS = [
    ("linker wildcard run",
     {"l.ld": "SECTIONS\n{\n  *(.text)           /* text sections, code */\n"
              "  *(.text*)          /* text* sections, code too */\n"
              "  *(.rodata)         /* constants, strings and the rest */\n}\n"},
     False),
    ("vendor banner in .ld",
     {"l.ld": "/*\n** LinkerScript\n** Note: for specific memory allocation, linker"
              " and startup files must be customised.\n**       Refer to the"
              " STM32CubeIDE user guide.\n*/\nMEMORY { }\n"},
     False),
    ("long block in .ld",
     {"l.ld": "/* A comment written here by a human that runs past the hundred"
              " characters the brief allows for one block. */\nMEMORY { }\n"},
     True),
    ("long block in .c",
     {"a.c": "/* A comment written here by a human that runs past the hundred"
             " characters the brief allows for one block. */\nint x;\n"},
     True),
]


def run(files):
    """One arm, in its own repository, with everything staged before the check."""
    tmp = tempfile.mkdtemp(prefix="conv-")
    try:
        os.makedirs(os.path.join(tmp, "tools"))
        shutil.copy(CHECKER, os.path.join(tmp, "tools", "check_conventions.py"))
        subprocess.run(["git", "init", "-q"], cwd=tmp, check=True)
        for name, body in files.items():
            with open(os.path.join(tmp, name), "w") as fh:
                fh.write(body)
        # Staged, because whether a file is tracked decides what the checker sees.
        subprocess.run(["git", "add", "-A"], cwd=tmp, check=True)
        r = subprocess.run([sys.executable, "tools/check_conventions.py"],
                           cwd=tmp, capture_output=True, text=True)
        return r.stdout + r.stderr, r.returncode
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def fired(out):
    """The language line, under either heading this check has carried."""
    for line in out.split("\n"):
        if line.startswith("== ") and ("non-ASCII" in line or "non-English" in line):
            return int(line.rsplit(":", 1)[1].strip(" =")) > 0
    raise AssertionError("the checker printed no language line:\n" + out)


def blocks(out):
    for line in out.split("\n"):
        if line.startswith("== comment blocks"):
            return int(line.rsplit(":", 1)[1].strip(" =")) > 0
    raise AssertionError("the checker printed no block line:\n" + out)


def main():
    bad = 0
    for name, files, want in BLOCK_ARMS:
        out, rc = run(files)
        got = blocks(out)
        ok = got == want
        if not ok:
            bad += 1
        print("%-26s blocks want %-5s got %-5s exit %d  %s"
              % (name, want, got, rc, "ok" if ok else "FAIL"))
    for name, files, want in ARMS:
        out, rc = run(files)
        got = fired(out)
        ok = got == want
        # A checker that reports and exits zero is a checker nothing can gate on.
        gate = (rc != 0) if got else (rc == 0)
        if not ok or not gate:
            bad += 1
        print("%-26s want %-5s got %-5s exit %d  %s"
              % (name, want, got, rc, "ok" if ok and gate else "FAIL"))
    print("check_conventions: %s (%d arms)"
          % ("ok" if not bad else "%d FAILED" % bad, len(ARMS) + len(BLOCK_ARMS)))
    return 1 if bad else 0


sys.exit(main())
