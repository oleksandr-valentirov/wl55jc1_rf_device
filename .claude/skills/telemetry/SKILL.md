---
name: telemetry
description: The record format the boards emit on their own over the VCP, and how to read it - the sentinel line, the sequence-gap rule that makes loss visible, wrap stitching, and the traps that come from sharing one UART with a console. Use when adding an event, reading a stream, joining two boards' logs, or when a poll is about to be used to answer a question about when something happened.
---

# Telemetry over the VCP

The boards emit records without being asked, on the same UART as the console.
This is not logging: it exists because three specific questions cannot be
answered by polling, and each one cost this bench a session.

**A wedge stops the answer and the core together.** A silent console is what a
dead console looks like, what a busy device looks like, and what a device told to
be quiet looks like. Only a heartbeat separates them.

**A poll reads a value, not the moment it moved.** `no-beacon 103` said a hundred
cycles found nothing and said nothing about when, so it could not be lined up
against the other board.

**A counter can be right about the wrong thing.** A forged-frame control read
`hits 2 → 3` and looked like a pass; the accepted superframe was a real beacon
that won the race. The counter was correct — about something else. Records name
the artifact, so the case is decided by reading rather than by re-running.

If you are about to answer "when did X happen" with a counter, that is the
signal to reach for this instead.

## The format

```
!<seq> <us> <kind> <key>=<value>...\r\n
```

`!` at column zero, one line, under 96 bytes, decimal everywhere. The device
side is `Core/Src/telemetry.c`; the reader is `tools/telemetry.py`; the full
table lives in
[`telemetry.md`](../../../radio_devices_docs/wl55_device/testing/telemetry.md).

Three rules, and they are the whole contract:

**Field names travel with the record.** Positional saves two bytes and moves
silently under a reader written against an older build. Adding a field is
additive; an unknown name is ignored.

**Every record with a position carries `sf`.** A wall-clock timestamp on two
boards is two clocks; the superframe is one number both compute, and it is the
only thing the two logs can be joined on. The exception is `boot`, which carries
no `sf` **at all** rather than a zero — nothing has aligned yet, and a zero would
be a real number describing something other than what a reader takes it for.

**Values are integers.** Signedness is declared per field in the kind table, not
inferred. A key that needs a string is a key that needs a code and a line in the
table.

## The property that makes absence visible

`tlm_emit` takes its sequence number **before** it looks at the ring:

```c
uint32_t mine = seq++;
if (next == tail) { dropped++; return; }
```

A record that does not fit still spends a number, so a full ring leaves a gap in
the stream. **Loss does not depend on a counter surviving the event that caused
it**, and it does not depend on anyone asking. The `dropped` counter exists and
is the cross-check, never the evidence.

Keep this if you port the format. A drop counter alone is the failure mode where
the instrument is lost together with the thing it was measuring.

### A gap is loss unless a `boot` sits inside it

A reboot renumbers the stream, and that is the one case where loss and evidence
look identical from the host. The rule belongs in the format, not in whoever is
reading: `boot` resets sequence continuity and the wrap epoch and counts as no
loss. Without it every reflash reports thousands of losses, and an instrument
that cries that loudly stops being read.

A reader joining mid-stream takes its first record as the origin, for the same
reason.

### Wrap stitching only works because beats bound the gap

`micros()` turns over every 71.6 minutes. A timestamp going backwards between
consecutive sequence numbers is a wrap — **and that inference is only valid
because a beat every five seconds guarantees no gap is near the wrap period.**
Lengthen the beat interval and the inference silently stops holding.

## Sharing the UART with a console

The console accumulates a response and flushes it in one blocking write;
telemetry drains one record per superloop pass through the same function. Both
run on the main thread, so **lines cannot interleave** — atomicity is a
consequence of the threading, not of a lock. If a drain ever moves into an
interrupt, that guarantee is gone and the format's line discipline goes with it.

**A telemetry line landing after the prompt breaks every command.** The host
waited for the buffer to *end* with `">>> "`, and a record arriving after the
prompt left it ending in something else — so a working device timed out every
command. `tools/console.py` splits records out before the check now, and `-r`
prints them to stderr rather than dropping them. Any new host tool that waits for
the prompt has this bug until it does the same.

**Do not silence the device to make a tool work.** A tool that needs the port
quiet cannot be used while the thing it is debugging is running, which is the
only time it matters. `tlm off` is for a measurement that genuinely needs the
console idle, and for nothing else.

## The cost, and where it must not land

`tlm_emit` is a timestamp, a sequence number and three words into a ring — cheap
enough for a timing path. Formatting is deferred to drain time. The write is
blocking: 96 bytes at 115200 is **8.3 ms**.

That is safe today by construction: the drain and the transmit run on the same
thread, so a drain cannot occur inside the spin that places a frame, and the only
place it can land before one is ahead of a 100 ms boundary lead that absorbs it.
**Both halves of that are load-bearing.** An interrupt-driven drain, a shorter
lead, or a longer line turns a cost into a timing fault.

## Adding an event

One line in the kind table in `Core/Src/telemetry.c` — name, up to three field
names, a signedness mask — and one `tlm_emit` at the site. Then update
`test/test_telemetry.c`, which pins the rendered lines as **written-out strings**
rather than round-tripping them: a round trip agrees with any format, including a
wrong one. The format is a contract with a host tool and with the other board, so
it is checked against text, not against itself.

Emit at the **decision**, not after it. `rec.deny` fires where the refusal is
taken, so a refusal that never happens leaves no record and a refusal that fires
names its own reason — which is what let the counter-versus-channel check be
shown to discriminate rather than merely to refuse.

## An accumulator cannot be bracketed; a stream can

Both boards hit this in one evening. The hub's `synctime` carried `min` and `max`
since boot, so a timing fix could not show its collapse in the column meant to
show it — the pre-fix extremes survive forever, and the only way to read a
post-fix population was to reflash. This side's `report` carried a running mean,
sd, min and max of the slot residual over every frame since arming: it answered
*what is the spread* and could not answer *what was the spread between
superframes 610664 and 610752*, which is the only question three power windows
ever needed.

`tx.up ... off=` carries the same number once per frame with a superframe beside
it, so any statistic a host wants is computable **and attributable**. The
accumulator, its sums, the integer square root and their printing were deleted
and nothing was lost — the samples they summarised are all in the stream, and
windowing was gained.

**Delete an on-device statistic once the stream carries its samples.** Keep one
only for what cannot be streamed: a control that must read non-zero once, or a
rate too fast to emit a record per event.

The same argument retires counters. `sent`, `no-beacon`, `tx-err`, `held`,
`cycles` and `off-beat` were each a total with no dates in it; `tx.up`,
`rx.miss`, `tx.deny why=` and `rx.beacon` are the same events with a superframe
on each. **A counter is a stream with the timestamps thrown away** — so when the
record exists, the counter is not a second instrument, it is a lossy copy.

## Record where it went, not only when it happened

`tx.up sf slot off` said when a frame left, into which slot, and how well aimed
it was. It did not say **which channel**, and on a hopping link that is the field
that decides whether the frame could have been heard at all.

When 21 frames produced 10 sync matches at the other end, the answer came from
asking the device afterwards with `hop <sf>` — which worked only because the map
is deterministic and the key had not changed since. Neither would survive a key
rotation, a reboot, or a disagreement about the map itself, which is precisely
the case worth diagnosing.

**A record of an action carries the parameters that select where it acts.** Time
and quantity are the easy fields and they are not the ones that separate "did not
arrive" from "arrived corrupt".

## Reading a stream

```bash
tools/telemetry.py /dev/serial/by-id/usb-...-if02          # follow
tools/telemetry.py /dev/serial/by-id/... -n 120            # bounded
tools/console.py -r /dev/serial/by-id/... "recover"        # records to stderr
```

The reader reports **silence past twelve seconds**, because a device that has
stopped emitting has either been asked to be quiet or has wedged, and those are
different findings. It exits non-zero if anything was lost.

## What it found in itself on the first run

Both within two minutes of the first live stream, and neither would have appeared
in a poll:

- **The beat drifted** — 5.013, 30.227, 35.364 s. The next deadline came from
  `micros()` at the moment the beat fired instead of stepping the previous one,
  so the drain's own cost became permanent drift. The same mistake this project
  documents for the superframe boundary, repeated one file away.
- **`sync` read as fine while the device was hunting for a beacon.** The field
  carried a state that says `aligned` and knows nothing about staleness — a name
  broader than its coverage, straight out of the `verification` skill's list.

An instrument you have not yet watched run is an instrument you have not tested.
