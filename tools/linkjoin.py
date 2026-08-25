#!/usr/bin/env python3
"""Join this device's transmit records to the hub's arrivals, in one process.

The device knows what it sent and cannot see where it landed; the hub knows what
arrived and cannot know what was sent. Every question about delivery needs both
halves, and this bench has twice paid for taking them from two windows that did
not coincide - the numerator and the denominator came from different populations
and the fraction looked like a measurement.

So the two streams are read here by one process, bounded by one window, and
joined on the superframe and the opportunity rather than on a wall clock. A
device timestamp and a host timestamp are two clocks; the superframe is the one
number both sides compute.

  device  tools/telemetry.py's record stream over the VCP: tx.up sf slot off grid
  hub     openhub-server's /api/events cursor: arrival_us against the hub's grid

What comes out is per opportunity: how many frames left, how many were accepted,
and how far into its slot each one landed as the hub measured it - which is the
quantity ROADMAP item 26 says only the hub can see, and item 41 predicts the
scatter of.

Read the caveat printed under the summary before quoting anything from it.
"""
import argparse
import json
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from telemetry import Stream                                    # noqa: E402

import serial                                                   # noqa: E402

# The library's, not the hub's; this tree no longer reaches ../OpenHub. ADR-0032
HEADER_DIR = "radio_stack/inc"
PROFILE_DIR = "radio_stack/profiles"
# Read from the build, never restated: a tool naming a number names a band.
PROFILE_FROM = "CMakeLists.txt"

# Copied constants are ROADMAP item 43; a host tool is not exempt.
PROBE = r"""
#include <stdio.h>
#include "radio_slots.h"
int main(void) {
    printf("UPLINK_OFFSET_US %u\n", (unsigned)RADIO_UPLINK_OFFSET_US);
    printf("SLOT_US %u\n",          (unsigned)RADIO_SLOT_US);
    printf("SLOT_OPPS %u\n",        (unsigned)RADIO_SLOT_OPPS);
    printf("SLOT_STRIDE %u\n",      (unsigned)RADIO_SLOT_STRIDE);
    printf("SLOT_GUARD_US %u\n",    (unsigned)RADIO_SLOT_GUARD_US);
    printf("UPLINK_AIR_US %u\n",    (unsigned)RADIO_UPLINK_AIR_US);
    return 0;
}
"""


def built_profile():
    """The profile this tree builds, or an exception rather than a guess."""
    m = re.search(r'set\(RADIO_PROFILE "(RADIO_PROFILE_[A-Z]+)"',
                  open(PROFILE_FROM, encoding="utf-8").read())
    if not m:
        sys.exit("no RADIO_PROFILE in %s: this tool must not choose a band"
                 % PROFILE_FROM)
    return m.group(1)


def geometry(header_dir):
    """The slot geometry, compiled from the header both firmwares build against."""
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = os.path.join(tmp, "g.c"), os.path.join(tmp, "g")
        with open(src, "w") as f:
            f.write(PROBE)
        cc = subprocess.run(["cc", "-std=c11", "-I", header_dir,
                             "-I", PROFILE_DIR,
                             "-DRADIO_PROFILE=%s" % built_profile(),
                             src, "-o", exe],
                            capture_output=True, text=True)
        if cc.returncode:
            sys.exit("cannot compile the geometry probe against %s:\n%s"
                     % (header_dir, cc.stderr.strip()))
        out = subprocess.run([exe], capture_output=True, text=True).stdout
    return dict((k, int(v)) for k, v in (l.split() for l in out.split("\n") if l))


def nominal_us(g, dev_slot, k):
    """Where opportunity k of device dev_slot opens, from the superframe boundary."""
    return g["UPLINK_OFFSET_US"] + (dev_slot + k * g["SLOT_STRIDE"]) * g["SLOT_US"]


def opportunity(g, arrival_us, dev_slot):
    """Which opportunity an arrival belongs to. The three are 611 ms apart, so the
    unknown origin offset cannot reach the next one."""
    span = g["SLOT_STRIDE"] * g["SLOT_US"]
    k = int(round((arrival_us - nominal_us(g, dev_slot, 0)) / float(span)))
    return min(max(k, 0), g["SLOT_OPPS"] - 1)


class Hub:
    """The server's device state, rebuilt from the event cursor.

    An event carries only the fields that changed, so the superframe belongs to
    the running state and the arrival to the event. Reading an event alone gets
    an arrival with no superframe to join it on.
    """

    def __init__(self, base):
        self.base = base.rstrip("/")
        self.fields = {}
        self.cursor = 0
        self.boot_id = None
        self.arrivals = []
        self.substituted = 0
        self.lock = threading.Lock()

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=5) as r:
            return json.load(r)

    def seed(self):
        """Cursor first, then state: a replayed event rewrites a field with its own
        value, while the reverse order would drop an arrival.

        **The cursor is `last_id` and never the last row of a page.** `/api/events`
        answers `since` with at most `limit` rows, default 200, taken from the
        *oldest* end - so reading `since=0` and keeping `events[-1]["id"]` seeds
        the cursor at the 200th oldest event of a buffer holding up to 2000, and
        the first polls then replay everything after it. Every historical uplink
        in that backlog arrives as an arrival with a superframe from before the
        window, joins nothing, and is counted as a hub-only arrival. That is the
        151 unmatched arrivals of run 2026-08-25-2, against 38 frames the hub's
        own ladder counted in the window - a population four times larger than
        the air could produce, which is what gave it away. The same response
        carries `last_id` and it was being thrown away.
        """
        health = self.get("/api/health")
        if not health.get("hub_connected"):
            sys.exit("server is up and no hub is connected - check `telem` on the hub")
        if not health.get("schema_agrees_with_hub"):
            sys.stderr.write("! schema disagrees with the hub; fields may arrive by id\n")
        self.cursor = self.get("/api/events?since=0&limit=1")["last_id"]
        # A list here and a dict on /api/snapshot and the stream: take either.
        devices = self.get("/api/devices")["devices"]
        if isinstance(devices, dict):
            devices = list(devices.values())
        for dev in devices:
            self.fields[dev["dev_id"]] = dict(
                (k, v.get("value")) for k, v in dev["fields"].items())

    def rxdiag(self):
        """The hub's receive counters, and the boot they were counted under.

        A hub reset restarts the superframe and zeroes these counters, so rows
        either side of one are two populations and the ladder is a difference
        between them. The events cursor carries no boot, so the id is read here
        - the two calls that bracket a run are the two ends of the check.
        """
        try:
            hub = self.get("/api/hub")
        except (urllib.error.URLError, OSError, ValueError):
            return None
        self.boot_id = (hub.get("hello") or {}).get("boot_id", self.boot_id)
        diag = hub.get("rxdiag")
        if not isinstance(diag, dict):
            return None
        return dict((k, v.get("value")) for k, v in diag.items()
                    if isinstance(v.get("value"), int))

    def slot_of(self, dev_id):
        return self.fields.get(dev_id, {}).get("slot")

    def find_by_slot(self, dev_slot):
        for dev_id, f in self.fields.items():
            if f.get("slot") == dev_slot:
                return dev_id
        return None

    def poll(self):
        """One cursor step. Returns the uplink arrivals it learned about."""
        try:
            events = self.get("/api/events?since=%d" % self.cursor)["events"]
        except (urllib.error.URLError, OSError, ValueError) as exc:
            sys.stderr.write("! server read failed: %s\n" % exc)
            return []
        found = []
        for ev in events:
            self.cursor = max(self.cursor, ev["id"])
            objects = ev.get("data", {}).get("objects", {})
            changed = {}
            for key, fields in objects.items():
                dev_id = key.split(":", 1)[1] if key.startswith("device:") else None
                if dev_id is None:
                    continue
                state = self.fields.setdefault(dev_id, {})
                for name, val in fields.items():
                    state[name] = val.get("value")
                changed[dev_id] = dict(
                    (n, v.get("value")) for n, v in fields.items())
            if ev.get("kind") != "uplink":
                continue
            dev_id = ev.get("dev_id")
            state = self.fields.get(dev_id, {})
            # A substituted value is the previous frame's and reads as a measurement.
            fresh = changed.get(dev_id, {})
            if "arrival_us" in fresh:
                arrival = fresh["arrival_us"]
            else:
                arrival = state.get("arrival_us")
                if arrival is not None:
                    self.substituted += 1
            sf = state.get("last_superframe")
            if arrival is None or sf is None:
                continue
            found.append({"dev_id": dev_id, "sf": sf, "arrival_us": arrival,
                          "slot": state.get("slot"), "ts": ev.get("ts"),
                          "rssi": state.get("rssi_up_sync_dbm"),
                          "gain": state.get("lna_gain")})
        return found


def device_reader(port, baud, out, stop):
    """The VCP stream in its own thread; the parser is telemetry.py's."""
    stream = Stream()
    buf = ""
    with serial.Serial(port, baud, timeout=0.2) as ser:
        while not stop.is_set():
            chunk = ser.read(ser.in_waiting or 1)
            if chunk:
                buf += chunk.decode("ascii", "replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                if not line.startswith("!"):
                    continue
                rec = stream.feed(line)
                if rec is not None:
                    out.append((rec, stream.lost))


def ladder(before, after, sent):
    """Where the frames went, hub-wide. Not per device - see the note it prints."""
    if not before or not after:
        return
    d = dict((k, after.get(k, 0) - v) for k, v in before.items())
    print("\n%-16s %8s   %s" % ("hub receive ladder", "count", "of this device's sent"))
    for name, key in (("sync matched", "sync_match"), ("passed CRC", "frames"),
                      ("CRC failed", "crc_err")):
        n = d.get(key)
        if n is None:
            continue
        print("%-16s %8d   %s" % (name, n,
              ("%.1f%%" % (100.0 * n / sent)) if sent else "-"))
    print("""
  These counters are hub-wide and count every device on the air, while the sent
  column above is this device alone. With another node transmitting, the ladder
  is an upper bound on what was heard from this one. Quiesce the other node, or
  read the percentages as bounds.""")


def whole_cycle_histogram(rows, g):
    """How many of a cycle's opportunities land together.

    Both halves are this side's, so the question needs nothing from the hub. If
    the three were lost independently the counts follow the binomial; a split
    into all-or-nothing means the channel holds its state for the 1.2 s the
    three opportunities span.
    """
    cycles = {}
    for r in rows:
        cycles.setdefault(r["sf"], []).append(r["arrival_us"] is not None)
    full = [v for v in cycles.values() if len(v) == g["SLOT_OPPS"]]
    if len(full) < 2:
        return
    opps = sum(len(v) for v in cycles.values())
    q = sum(sum(v) for v in cycles.values()) / float(opps)
    hist = [0] * (g["SLOT_OPPS"] + 1)
    for v in full:
        hist[sum(v)] += 1
    n = len(full)
    print("\n%-8s %9s %9s %12s   over %d complete cycles"
          % ("of %d" % g["SLOT_OPPS"], "observed", "if indep", "expected n", n))
    for j in range(g["SLOT_OPPS"] + 1):
        pj = (math.comb(g["SLOT_OPPS"], j) * q ** j *
              (1 - q) ** (g["SLOT_OPPS"] - j))
        print("%-8d %9d %8.1f%% %12.1f" % (j, hist[j], 100 * pj, n * pj))
    print("cycle delivery: observed %.1f%%, independent loss predicts %.1f%%"
          % (100.0 * (n - hist[0]) / n, 100.0 * (1 - (1 - q) ** g["SLOT_OPPS"])))


def summarise(rows, lost, g, hub_only, substituted):
    """Per opportunity, and never a delivery figure without both of its halves."""
    print("\n%-3s %6s %6s %9s   %-22s   %-22s"
          % ("k", "tx", "rx", "delivery", "device off (us)", "hub - nominal (us)"))
    for k in range(g["SLOT_OPPS"]):
        sent = [r for r in rows if r["k"] == k]
        got = [r for r in sent if r["arrival_us"] is not None]
        if not sent:
            continue
        offs = [r["off"] for r in sent]
        res = [r["arrival_us"] - nominal_us(g, r["slot"], k) for r in got]

        def stat(xs):
            if not xs:
                return "%-22s" % "-"
            sd = statistics.stdev(xs) if len(xs) > 1 else 0.0
            return "%7.0f sd %-5.0f n %-4d" % (statistics.mean(xs), sd, len(xs))

        print("%-3d %6d %6d %8.1f%%   %s   %s"
              % (k, len(sent), len(got), 100.0 * len(got) / len(sent),
                 stat(offs), stat(res)))

    whole_cycle_histogram(rows, g)

    sent = len(rows)
    got = len([r for r in rows if r["arrival_us"] is not None])
    if sent:
        print("\nall opportunities: %d sent, %d accepted, %.1f%%"
              % (sent, got, 100.0 * got / sent))
    by_sf = {}
    for r in rows:
        by_sf.setdefault(r["sf"], []).append(r)
    whole = [s for s in by_sf.values()
             if any(r["arrival_us"] is not None for r in s)]
    if by_sf:
        print("superframes with at least one of %d accepted: %d of %d, %.1f%%"
              % (g["SLOT_OPPS"], len(whole), len(by_sf),
                 100.0 * len(whole) / len(by_sf)))
    if lost:
        print("! %d device record(s) lost on the VCP: the tx count is a floor" % lost)
    if hub_only:
        share = 100.0 * hub_only / max(1, hub_only + got)
        print("! %d hub arrival(s) had no device record in the window, %.0f%% of"
              " the joined population" % (hub_only, share))
        if share >= 25.0:
            print("! NOT GRADEABLE: 06-regression.md 6.2 refuses a window whose"
                  " non-vacuity figure is a large fraction. Do not quote the"
                  " delivery figure above.")
    if substituted:
        print("! %d arrival(s) carried the previous frame's value, not their own:"
              " the server's diff omitted the field" % substituted)

    print("""
Read before quoting:
  The MEAN of hub - nominal is not a residual. The hub stamps arrival_us after
  the frame ended and after its own decode, so the column carries the air time
  and the hub's work as a constant offset of roughly 9.1 ms. Only the SD is a
  measurement of this device's placement, and it is what ROADMAP item 41
  predicts. Item 26 is what the column exists for.
  Delivery counts frames the hub ACCEPTED. A frame that arrived and failed CRC
  is a loss here and is visible as one in /api/devices/<id>/frames.""")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="the device's VCP, by serial id")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-s", "--server", default="http://localhost:8080")
    ap.add_argument("-n", "--seconds", type=float, default=0.0,
                    help="stop after this long; 0 follows until interrupted")
    ap.add_argument("-g", "--grace", type=float, default=6.0,
                    help="seconds to wait for an arrival before calling it lost")
    ap.add_argument("--header-dir", default=HEADER_DIR)
    ap.add_argument("--dev", help="hub device id; the default matches by slot")
    args = ap.parse_args()

    g = geometry(args.header_dir)
    print("geometry from %s: uplink at %d us, slot %d us, %d opportunities, stride %d"
          % (args.header_dir, g["UPLINK_OFFSET_US"], g["SLOT_US"],
             g["SLOT_OPPS"], g["SLOT_STRIDE"]))

    hub = Hub(args.server)
    hub.seed()
    diag_before = hub.rxdiag()
    boot_before = hub.boot_id
    print("hub boot %s" % (boot_before if boot_before is not None else "not reported"))

    records, stop = [], threading.Event()
    reader = threading.Thread(target=device_reader,
                              args=(args.port, args.baud, records, stop),
                              daemon=True)
    reader.start()

    print("%-9s %-3s %-5s %8s %10s %10s %9s %5s  %s"
          % ("sf", "k", "slot", "off", "arrival", "nominal", "hub-nom", "grid", ""))

    waiting, arrived, rows = {}, {}, []
    dev_id, hub_only, seen, lost, started = args.dev, 0, 0, 0, time.time()

    def emit(row):
        rows.append(row)
        nom = nominal_us(g, row["slot"], row["k"])
        if row["arrival_us"] is None:
            print("%-9d %-3d %-5d %8d %10s %10d %9s %5d  lost"
                  % (row["sf"], row["k"], row["slot"], row["off"], "-", nom, "-",
                     row["grid"]))
        else:
            print("%-9d %-3d %-5d %8d %10d %10d %9d %5d  rssi %s gain %s"
                  % (row["sf"], row["k"], row["slot"], row["off"], row["arrival_us"],
                     nom, row["arrival_us"] - nom, row["grid"], row["rssi"],
                     row["gain"]))
        sys.stdout.flush()

    def flush(deadline):
        """A transmit with no arrival by the deadline is a loss, not a wait."""
        nonlocal hub_only
        for key in [k for k, v in waiting.items() if v["at"] < deadline]:
            emit(waiting.pop(key))
        for key in [k for k, v in arrived.items() if v["at"] < deadline]:
            arrived.pop(key)
            if dev_id is not None and key[0] == dev_id:
                hub_only += 1

    try:
        while True:
            if args.seconds and time.time() - started > args.seconds:
                break
            while seen < len(records):
                rec, lost = records[seen]
                seen += 1
                if rec["kind"] != "tx.up":
                    continue
                f = rec["fields"]
                slot_n = f["slot"]
                slot = slot_n % g["SLOT_STRIDE"]
                k = slot_n // g["SLOT_STRIDE"]
                if dev_id is None:
                    dev_id = hub.find_by_slot(slot)
                row = {"sf": f["sf"], "k": k, "slot": slot, "off": f["off"],
                       "grid": f["grid"], "arrival_us": None, "rssi": None,
                       "gain": None, "at": time.time()}
                # The hub pushes an arrival before the device drains its
                # record: neither side is reliably first.
                hit = arrived.pop((dev_id, f["sf"], k), None)
                if hit is not None:
                    row.update(arrival_us=hit["arrival_us"], rssi=hit["rssi"],
                               gain=hit["gain"])
                    emit(row)
                else:
                    waiting[(f["sf"], k)] = row

            for a in hub.poll():
                if a["slot"] is None:
                    continue
                if dev_id is not None and a["dev_id"] != dev_id:
                    continue
                k = opportunity(g, a["arrival_us"], a["slot"])
                row = waiting.pop((a["sf"], k), None)
                if row is not None:
                    row.update(arrival_us=a["arrival_us"], rssi=a["rssi"],
                               gain=a["gain"])
                    emit(row)
                else:
                    a["at"] = time.time()
                    arrived[(a["dev_id"], a["sf"], k)] = a

            flush(time.time() - args.grace)
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        flush(time.time() + 1.0)

    diag_after = hub.rxdiag()
    if boot_before is not None and hub.boot_id != boot_before:
        print("\n! the hub reset during this window: boot %s -> %s"
              % (boot_before, hub.boot_id))
        print("! superframes restart and the ladder counters zero, so the rows above"
              " are two populations. No summary is printed: re-run inside one boot.")
        return 2
    summarise(rows, lost, g, hub_only, hub.substituted)
    ladder(diag_before, diag_after, len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
