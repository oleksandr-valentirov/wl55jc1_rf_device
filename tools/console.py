#!/usr/bin/env python3
"""Drive the device console from a script: send commands, print what comes back.

The CLI echoes every character it receives, so a reply is only complete once the
">>> " prompt returns; waiting for that beats guessing a delay.

The device also emits telemetry records on its own, and one of those landing
after the prompt used to leave the buffer not ending in it - which would time
out every command on a device that is working. Records are split out here rather
than suppressed on the device: a tool that needs the port quiet is a tool that
cannot be used while the thing it is debugging is running."""
import argparse
import re
import sys
import time

import serial

PROMPT = b">>> "
RECORD = re.compile(rb"^![0-9]+ .*?\r\n", re.M)


def split_records(buf):
    """Telemetry lines out, everything else in the order it arrived."""
    records = [m.group(0) for m in RECORD.finditer(buf)]
    return RECORD.sub(b"", buf), records


def read_until_prompt(port, timeout, show_records=False):
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += port.read(port.in_waiting or 1)
        clean, records = split_records(buf)
        if show_records:
            for r in records:
                sys.stderr.write(r.decode("ascii", "replace"))
            if records:
                sys.stderr.flush()
                buf = clean
                clean, records = split_records(buf)
        if clean.endswith(PROMPT):
            return clean
        time.sleep(0.005)
    return split_records(buf)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("cmd", nargs="*", help="commands to run; none means interactive dump")
    ap.add_argument("-t", "--timeout", type=float, default=2.0)
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-r", "--records", action="store_true",
                    help="print telemetry records to stderr instead of dropping them")
    args = ap.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.05) as port:
        port.reset_input_buffer()
        port.write(b"\r")
        read_until_prompt(port, 1.0, args.records)
        if not args.cmd:
            while True:
                data = port.read(port.in_waiting or 1)
                if data:
                    sys.stdout.write(data.decode("ascii", "replace"))
                    sys.stdout.flush()
        for cmd in args.cmd:
            port.write(cmd.encode() + b"\r")
            reply = read_until_prompt(port, args.timeout, args.records)
            text = reply.decode("ascii", "replace")
            sys.stdout.write(text)
            if not text.endswith(PROMPT.decode()):
                sys.stdout.write("\n[timeout waiting for prompt]\n")
                sys.exit(1)
        sys.stdout.write("\n")


if __name__ == "__main__":
    main()
