#!/usr/bin/env python3
"""Follow the device's telemetry stream, and say what is missing rather than
only what arrived.

Three properties are checked here rather than trusted, because each one is a
mistake this bench has already made:

  * a gap in the sequence number is loss, and it is reported as loss - the
    device spends a sequence number whether or not the record fitted its ring,
    so absence is never silent;
  * micros() wraps every 71.6 minutes, and a timestamp that goes backwards
    between consecutive sequence numbers is a wrap rather than a fault, which
    only holds because beats bound the gap;
  * silence is a finding. A device that stops emitting has either been asked to
    be quiet or has wedged, and those are different - so the reader says how
    long it has been quiet instead of waiting patiently for ever.
"""
import argparse
import re
import sys
import time

import serial

RECORD = re.compile(
    r"^!(?P<seq>\d+) (?P<us>\d+) (?P<kind>[a-z.]+)(?P<fields>(?: [a-z]+=-?\d+)*)\s*$")
WRAP = 1 << 32


class Stream:
    """Sequence continuity and wrap stitching, kept apart from the I/O."""

    def __init__(self):
        self.seq = None
        self.lost = 0
        self.epoch = 0
        self.last_us = None
        self.records = 0

    def feed(self, line):
        m = RECORD.match(line.strip())
        if not m:
            return None
        seq = int(m.group("seq"))
        us = int(m.group("us"))
        fields = dict(
            (k, int(v)) for k, v in
            (f.split("=") for f in m.group("fields").split() if "=" in f))

        gap = 0
        restart = m.group("kind") == "boot"
        if self.seq is not None and not restart:
            gap = seq - self.seq - 1
            if gap < 0:
                # Restarted without its boot record being seen.
                self.epoch = 0
                self.last_us = None
                gap = 0
            else:
                self.lost += gap
        if restart:
            # A gap is loss unless a boot sits in it; the record settles it.
            self.epoch = 0
            self.last_us = None
        self.seq = seq

        # Valid only because beats keep the gap far below the wrap period.
        if self.last_us is not None and us < self.last_us:
            self.epoch += 1
        self.last_us = us
        self.records += 1

        return {
            "seq": seq, "us": us, "kind": m.group("kind"), "fields": fields,
            "gap": gap, "t": (self.epoch * WRAP + us) / 1e6,
        }


def render(rec):
    fields = " ".join("%s=%d" % kv for kv in rec["fields"].items())
    return "%12.3f  %-4d %-9s %s" % (rec["t"], rec["seq"], rec["kind"], fields)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-q", "--quiet-after", type=float, default=12.0,
                    help="seconds of silence before saying so; beats are every 5")
    ap.add_argument("-c", "--cmd", action="append", default=[],
                    help="send a console command, then keep following")
    ap.add_argument("-n", "--seconds", type=float, default=0.0,
                    help="stop after this long; 0 follows for ever")
    args = ap.parse_args()

    stream = Stream()
    started = time.time()
    last_seen = started
    warned = False
    buf = ""

    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        for cmd in args.cmd:
            port.write(cmd.encode() + b"\r")

        while True:
            if args.seconds and time.time() - started > args.seconds:
                break
            chunk = port.read(port.in_waiting or 1)
            if chunk:
                buf += chunk.decode("ascii", "replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                if not line.startswith("!"):
                    # Console output: shown, never parsed as a record.
                    if line.strip():
                        sys.stdout.write("             |  %s\n" % line.strip())
                    continue
                rec = stream.feed(line)
                if rec is None:
                    sys.stdout.write("             ?  unparsed: %s\n" % line.strip())
                    continue
                if rec["gap"]:
                    sys.stdout.write("             !  %d record(s) lost before seq %d\n"
                                     % (rec["gap"], rec["seq"]))
                sys.stdout.write(render(rec) + "\n")
                sys.stdout.flush()
                last_seen = time.time()
                warned = False

            quiet = time.time() - last_seen
            if quiet > args.quiet_after and not warned:
                sys.stdout.write("             !  quiet for %.1f s - wedged, or "
                                 "\"tlm off\"\n" % quiet)
                sys.stdout.flush()
                warned = True

    sys.stdout.write("\n%d records, %d lost\n" % (stream.records, stream.lost))
    return 1 if stream.lost else 0


if __name__ == "__main__":
    sys.exit(main())
