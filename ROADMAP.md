# Roadmap

The single list of open work on the device: debts, defects, and design that was
agreed and never built. **Nothing here is reasoning** — every item names the page
in `../radio_devices_docs` that holds the why, exactly as source comments do. If
an item needs a paragraph to justify it, that paragraph belongs on its page and
the line here shrinks to a pointer.

An item leaves this file when it is done, not when it is understood.

**Cite a tag or a commit message, never a bare hash.** The history here was
rebuilt into eleven themed commits on 2026-08-21, and every hash written before
that stopped being reachable from `master` the moment it was — silently. A tag
survives a rebuild and a hash does not.

**Cite this repository's own evidence, not another queue's item number.** A number
in a foreign queue moves when that queue is cleaned, and a number is not a
sentence, so nothing anywhere disagrees when it goes stale. Name the file, the
symbol, the commit or the ADR instead; those fail loudly when they move. The `hub`
items below are the exception and are hints rather than identifiers.

**Cleaned four times.** The first pass retired eight closed entries and moved the
reasoning worth keeping from each to its page in `../radio_devices_docs` — see
`radio/hopping.md`, `radio/beacon.md`, `radio/timebase.md`, `testing/telemetry.md`
and `testing/bench-harness.md` under `wl55_device/`. The second retired item 34,
the hub's front end, which had been closed as an investigation and kept only as a
pointer; its measurement is on `open_hub/radio/configuration.md` and item 4 now
carries the one line about it that this queue needs. **A pointer is not an open
item**, and keeping one here is how a closed investigation gets re-opened. The
third retired item 83, the hop deck KAT: it was verified on node B with its own
control on 2026-08-24 and the reasoning worth keeping is on
`radio/hopping.md` and `wl55_device/security/self-tests.md`. **A closed item that
stays because its story is good is the same failure in a nicer costume.**

**The fourth pass retired the four entries phase 9 closed**, each of which had
been left here carrying a `closed`, `fixed` or `moved` tag — which is the same
failure again, in the costume of a status column. Item 85, `release` refusing to
reopen the window only it could reopen, its reasoning now on
`wl55_device/testing/console.md`; item 76, the library-to-be's include guard,
whose per-file rule and empty-population arm are on `radio/phy-seam.md` and whose
duplication across two consumers is `../radio_stack/ROADMAP.md` item 3; item 84,
this tree compiling the hub's `hop.c`, closed in the commit that made the
library the only one, reasoning on `radio/hopping.md`; and item 82, `exchange.h`
reaching an implementation, reasoning on `radio/phy-seam.md` § the crypto seam.
**An item that records its own closure is a queue entry that will be re-read as
open**, because the tag is the last thing scanned and the title is the first.

**Status words**

| | |
|---|---|
| `blocking` | the acceptance criteria cannot be met without it |
| `defect` | confirmed wrong, in the tree today |
| `debt` | agreed, costed, deliberately not built |
| `hub` | the work is on the hub side; listed so this side does not assume it |
| `contract` | changing it binds both firmwares — agree with the hub session first |

The hub keeps the same list for its half in `../OpenHub/ROADMAP.md`, and neither
file is visible to the other repository. Contract items appear in both, with the
reasoning in [`radio/known-issues.md`](../radio_devices_docs/radio/known-issues.md).

---

## The acceptance criteria

> An unpaired device listens. The hub sends it a pairing invitation. The device
> pairs with no side commands from a CLI, the two exchange keys, bring up an
> encrypted channel, and start exchanging regular messages. Both the device and
> the hub survive a reboot and restore the link. An event at a sensor reaches the
> hub and is processed there within 1 s.

**"No side commands from a CLI" means none typed on the device**, which is this
repository's clause of it — settled with the hub session 2026-08-21.

**Irregular messages are out and a deadline is in — settled 2026-08-21.** No
contention windows and no transmit outside the grid; the latency is bought with
revisit interval alone. The requirement is settled and the mechanism is not.

---

## Blocking

### 1. The state machine runs; nothing has driven it through a reset — `blocking`

**Rewritten 2026-08-23 — the construction half is done.** `main()` runs
`console_poll(); timebase_service(); device_service(); telemetry_service();`, and
`device_service()` listens through `invite_service()` while unpaired and recovers
and reports while paired. `report_armed` is gone: nothing on this node is armed
from a console any more, and ten enrolments completed with nothing typed on it.

Three things this entry still owns:

- **No reset has been run through the machine.** Every trial so far erased the
  store first, which exercises the draw path and never the restore path. That is
  item 5's sequence and this item's evidence.
- **Listening is bounded by design and the bound is not a defect**:
  `DEVICE_ENROL_WINDOW_MS` is 600 000, so the node stops listening ten minutes
  after power-up ([ADR-0024](../radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md)).
  It has to be stated wherever *listens unattended* is claimed.

  **Measured 2026-08-24, because it had been mistaken for a growing back-off.**
  There is no back-off: `INVITE_SLICE_MS` is 8500 and `INVITE_RETRY_GAP_MS` is
  2000, so a slice every 8.5 s until the window shuts and none after. Node B at
  34 872 s of uptime held `listening slices 71`; 71 x 8.5 s is 603 s against a
  window of 600. Averaged over the whole uptime that is one slice per 491 s,
  which is what the wrong reading was made of.

  The bound stays; **saying nothing about it was the defect** and is fixed.
  `state` now prints the seconds left or `WINDOW SHUT`, and the close emits one
  record. What is still owed is the length being a policy number rather than a
  compile-time constant, and the reopen route on a console-less unit — item 58.
- **Camping is still missing between recoveries.** Recovery starts only after
  `RECOVER_LOST_US`, so a device that has lost the beacon idles, goes stale and
  waits 30 s to notice. More recovery does not close that; camping does.

`Core/Src/main.c`, `Core/Src/device.c` -> `device_service`, `invite_service`.
`radio_devices_docs/wl55_device/radio/pairing.md`.
### 4. The grid cannot meet the 1 s event deadline — `blocking` `contract`

One slot per superframe makes the worst case 2 s, and the arithmetic is
`G + t_latch + t_air + t_hub <= 1000`, where G is the gap between a device's
opportunities. **Two opportunities per superframe cannot meet it**: the max gap
is minimised at exactly 1000 ms. Three can, and 64 devices x 3 is 192 slots
against 96.

Capacity is bounded by the uplink region's span rather than by the slot count —
the k slots spread over 2 s but may only be placed inside 1.824 s, so the device
count is `(R - 2*667ms) / w + 1`.

**Settled at k=3** and written: `RADIO_SLOT_OPPS 3`, `RADIO_SLOT_STRIDE 65`, and
`radio_slots.h:175` asserts `EVENT_GAP + latch + air < 1000000`, which holds at
789250 us.

**Part of the floor is the hub's front end, measured, and it is not enough.**
Confirmed with a control that closed: dropping the hub's LNA from AGC to G6 lifts
per-frame delivery 23 % → 34 %, which is 54 % → 71 % for at least one of three
opportunities. **The requirement needs 78 % per frame for 99 % in one
superframe.** A real gain, a third of the way, and this item stays blocking. The
arms, the withdrawn PA exoneration and the three AGC registers still to look at
are on `radio_devices_docs/open_hub/radio/configuration.md` § the gain step
confirmed.

**A floor of roughly 59 % loss is slot-independent and is not timing.** Per-slot
delivery measured by the hub on 2026-08-22 against 8124 frames sent per slot:
~41 / 24 / 14 %. Item 41's variance explains the *gradient* between those and
nothing about why slot 1, with 13 µs of scatter against a 1400 µs guard and 56 dB
of margin, loses three frames in five. Whatever that is, it is this item's.

**The arithmetic closes and the link does not.** The agreed-window measurement is
**23 % detected, 20 % accepted** at -17 dBm, per slot 39/15/6 %. At a 20 % first-
try acceptance rate the deadline is not met in practice whatever the geometry
allows, and three opportunities at 20 % is not three chances at the deadline —
it is 49 % of one. The hub's `radio/tdma.md` now states this plainly rather than
quoting the old 876-of-876 at -24 dBm.

**Item 21 does not block measuring it, and the claim that it did was wrong in
both directions.** A k=3 PER run is this device transmitting three uplinks and
the hub receiving them. This side's transmit half has compared `(superframe,
slot)` since `feat(link)`, and the hub's receive half has carried `rx_floor`
beside `rx_floor_slot` since before the question was asked — `CM4/Core/Src/radio.c`,
`age < 0 || (age == 0 && f.slot <= d->rx_floor_slot)`, checked in HEAD rather
than assumed. **Nothing in the k=3 path refuses two of every three.** What item 21
was actually about is this side's *receive* direction, which has no k=3 in it at
all. The run is available and was never gated.

**That assert was green on a capability this device did not have.** It computes
`RADIO_EVENT_GAP_US` from `RADIO_SLOT_OPPS`, and until the seal-guard fix (`pre-squash-2026-08-21`) the guard
refused two of every three opportunities, so the number this side could use was
one:

    OPPS 3   gap 778000   total  789250   passes
    OPPS 2   gap 1389000  total 1400250   fails
    OPPS 1   gap 2000000  total 2011250   fails

The deadline was missed by a factor of two with nothing disagreeing, because the
assert pins the design constant rather than what either firmware can reach. The
transmit half is fixed; **the header wants a comment naming what the guarantee
depends on**, agreed with the hub session and pending its next change.

The event itself needs no wire change: `report_every` is device-side policy, the
slot is already in the nonce, and `RADIO_SLOT_TO_DEVICE(n) = n % 65` maps slots
66 and 131 back to this device. It costs one bit of `flags`, of which seven are
free.

Two device-side consequences whichever option wins: `t_latch` is 3.25 ms awake
and on channel (2882 ramp + 268 SetTx + 82 seal) and **unmeasured from sleep**,
which is the figure the budget actually needs; and `report_service` refuses to
transmit in a cycle where it heard no beacon, so every opportunity but the first
has to fly on the free run with the window sized by staleness.

`radio_devices_docs/radio/tdma.md` § the event deadline.

### 5. The reboot-and-recover clause has never been run as one sequence — `blocking`

The store holds identity, session key, hop key, slot, rate, the replay floor and
the counter mark, and each has been exercised on its own. Clean device, invited,
paired, reporting, reset, recovered, still on the grid has never run end to end,
and pairing has run one device, once.

Sweep the success-only paths while doing it: the paths that can only execute
after something finally goes right are the cheapest to find at exactly that
moment (`verification` skill).

`radio_devices_docs/wl55_device/testing/bench-harness.md`.

---

## Defects

### 6. ADR-0023's transmit floor has never been exercised across a reset — `defect`

**Replaces the two entries this file carried for the counter reservation.**
`store_reserve_counter()` is gone, `reserve_covers` and `reserve_extend` with it,
and `time_start()` now seeds the clock from zero — *a booted device has no opinion
and the first beacon gives it one*. Sealing is gated by `tx_allowed()` on the
`tx_floor` an opened downlink supplies, plus `TX_FLOOR_MARGIN`.

So the 33-minute post-reset silence is removed **by design**, and the durable mark
no longer answers *what time is it*. `counter_mark` survives in the record layout
as a retired field and in the torn-write fixture, and nothing else reads it.

**Exercised 2026-08-24, and it failed.** `downlink_open()` raised the floor on
every downlink rather than latching the first, and the downlink region is in the
same cycle as the report — so the floor was always the superframe about to be
used and every report was refused `why=8`. Hub `uplink windows 589, sync 0`
against a node that believed it was reporting. Fixed by latching once, and both
sides then agree: node `tx.up sf=21832 slot=2`, hub `sync 2, 2/0`, and an SDR
reading `UPLINK v6 slot=2` off the air at 68.6 ms against a grid offset of 68.8.

**What is still owed is the reset half.** The evidence above is a device that
booted, paired earlier, and transmitted — not one that transmitted, reset, and
transmitted again above its own previous nonces. That is item 5's sequence and it
is what the floor exists for.

`Core/Src/device.c` -> `tx_allowed`, `time_start`, `downlink_open`.
`radio_devices_docs/radio/decisions/0023-the-hub-supplies-the-transmit-floor.md`.

### 40. A reset reopens up to a thousand superframes of replay window — `defect`

The downlink floor is durable through `store_note_received` / `rx_floor`, and
that write is amortised to `STORE_COUNTER_STEP`, which is 1000. So the stored
floor trails the live one by up to a thousand superframes — over half an hour at
`SUPERFRAME_US` — and after a reset every downlink inside that trail is accepted
again. The flag closed the unbounded case, not this one.

The amortisation exists because the alternative is a flash append per accepted
frame, which the store's endurance does not have and which cannot happen inside
a slot in any case. So the fix is not a smaller step: it is either a second,
cheaper durable location for the tail, or an argument that the trail is
acceptable because a downlink carries `cmd_seq` and `downlink_apply` refuses a
repeat of the command it holds — which is an argument, not a guard, and this file
is not the place to make it.

Found 2026-08-22 while closing item 21. Nothing on the wire changes either way.

`Core/Src/store.c` → `store_note_received`,
`radio_devices_docs/wl55_device/security/replay.md`.

### 37. A runtime preamble walks past the assert that sizes the slot — `defect`

`radio preamble <2..32>` writes `preamble_bits` and `radio_tx_air_time_us`
follows it, but `_Static_assert(RADIO_AIR_START_TO_END_US(RADIO_UPLINK_BYTES) <=
RADIO_SLOT_US)` is written against the compile-time `RADIO_PREAMBLE_BYTES`, so a
runtime change walks straight past the only check that the frame still fits its
slot. An assert pinning two constants the same side owns pins nothing about the
value on the air (`verification`).

**Half closed.** `radio_set_preamble` costs the value before setting it and
refuses anything whose uplink frame will not fit `RADIO_SLOT_US`, which removes
the catastrophic case - 32 bytes is 12480 µs, three slots wide, and was accepted.

**What it still does not encode is the placement offset.** The check allows up to
12 bytes and 12 overruns once the measured ~590 µs of start offset is added.
There is no constant for that offset to check against - that is item 11, which
has never sized `RADIO_SLOT_GUARD_US` from a measured ramp. Until it does, 8 is a
hand-checked number and not one the firmware enforces:

    frame air at 4 B preamble    8000 µs
    slot                         9400 µs  (8000 air + 1400 guard)
    measured start offset         ~590 µs  (582..600)
    headroom                      ~810 µs  = 5 bytes, so 8 is the maximum

**The second consumer this item used to record was recorded backwards, and the
prescription would have caused the error it named.** It read `start_us`
subtracting the compile-time pre-sync as contamination and asked for the runtime
value. But `preamble_bits` reaches the chip in `SetPacketParams`' `PreambleLength`,
which on the SX126x in GFSK is **what this radio transmits**; reception is armed
by `PREAMBLE_DETECT_16BIT`. The preamble on a received frame is the sender's,
every frame this device receives is the hub's, and the hub's is fixed in the
shared header — so for a received frame the compile-time constant is correct.
The real defect was the mirror image and is closed: `cmd_downlink` estimated a
**received** frame's start with the transmit-side helper. Fixed in
`fix(radio): air time has a direction`, which split the general verb into
`radio_tx_air_time_us` and `radio_rx_air_time_us`.

**Nothing but the names enforces that split**, so `radio preamble <n>` prints both
air times: a sweep must move the transmit figure and leave the receive figure
where it was. That is the check, and it is only visible at the moment of a sweep.

**Why the sweep matters:** 4 -> 8 bytes restores the 1280 µs AGC settling budget
of the 25 kbps era at unchanged rate, which is the one-sided test of the hub's
preamble hypothesis about the hub's front end. Side effect the hub must be told before it
attributes frames: key-up is unchanged, so **the sync word moves 640 µs later
inside the slot**.

This item claimed the hub's receive-side arithmetic was correct only while this
device stays at four bytes. **Checked on their side rather than accepted, and it
is false**: their two consumers measure from the sync edge and subtract
`RADIO_AIR_SYNC_TO_END_US`, which is the 42 bytes after the sync word and contains
no preamble term. A sweep moves no hub figure. The announcement is still owed,
for attribution rather than for arithmetic.

`Core/Src/radio.c` → `radio_set_preamble`. `radio_devices_docs/radio/phy.md`,
`radio_devices_docs/wl55_device/radio/driver.md`.

### 41. The period estimate's noise is fixed in source and unconfirmed on air — `defect`

**Item 35 is closed** — `hub_us_to_local` scales every hub microsecond by
`measured_us / SUPERFRAME_US`, and the hub confirmed it from its own grid on
2026-08-22: residuals +614 / +1240 / +790 us for slots 1 / 66 / 131, means flat and
the sign wrong for the old defect.

**What replaced it is the same lever arm applied to variance**, and the fix is
written: `SUPERFRAME_PERIOD_BASELINE` is 64, so the period is measured across a
span rather than between consecutive beacons and one timestamp's noise divides by
the superframes it covers. `test_period` grades the estimate's worst error under
+/-600 us of injected jitter rather than passing or failing it — baseline 1 gives
1166 us, 8 gives 128, 32 gives 30, 64 gives 18. Predicted post-fix scatter
**0.2 / 2.0 / 3.8 us**, from 9 / 104 / 199.

**Flashed 2026-08-22 and the pre-registration's own falsifier fired**, so no
scatter result is kept from that run. Board B's delivery fell by a factor of six
(p = 0.0016) while board A's rose, one flash, two boards, opposite directions —
and the flash carried a second treatment nobody had registered (item 52).

**The next attempt is a design, not a rerun**, and all three of its rules are on
the page: separate treatment by board rather than by time, grade the *spread* of
within-cycle slopes rather than a pooled slope, and compute the power on that
statistic in units of cycles rather than frames. Producing an untreated board costs
a rollback flash.

**This explains the gradient and not the floor.** Slot 1 delivers ~41 % carrying
13 us of scatter against a 1400 us guard, so something roughly slot-independent
loses ~59 % of frames. The floor is item 4's and it is not timing.

`Core/Src/superframe.c`, `Core/Src/device.c` -> `hub_us_to_local`.
`radio_devices_docs/wl55_device/radio/timebase.md` § correcting a bias, § the
statistic to grade.

### 29. The device is schedulable for 4 s out of every 16 — `defect`

`SUPERFRAME_FRESH_US` is two superframes and the report cadence is eight, so the
clock expires between cycles and is re-earned by the beacon at the head of the
next one. Measured on 2026-08-21 from the telemetry stream, repeating exactly:

    sync.ok    sf=557448  per=2005264
    sync.lost  sf=557449  since=4000021
    sync.ok    sf=557456  per=2005102

Nothing is wrong with either constant. The consequence is: an event arriving
while the gate is shut cannot use any of its three opportunities until a beacon
has been heard, so **the revisit interval is not the only term between an event
and the air** — and item 4's deadline is computed as though it were.

Only visible because the device emits rather than answers: a poll lands in one of
the two states and reads it as the steady one.

**It is also a treatment, not only a latency term** — found on 2026-08-22 when
node A and node B were put on the air together to compare distance. A holds the
`every 8` grant and B holds `every 2`, so A cold-starts on every cycle and
searches with `win=71934` where B tracks with `win=47806`, and A's period
estimate comes out at 2010388 us against B's 2004216 us — 0.3 % apart on the same
hub, because a sparse and churning baseline is a worse baseline. Both effects
push a far device's delivery down for reasons that are not distance, which is the
comparison the experiment exists to make. **Two devices being compared must hold
the same grant**, and the grant is the hub's to set.

`Core/Inc/superframe.h` → `SUPERFRAME_FRESH_US`, `Core/Src/device.c` →
`report_service`, `radio_devices_docs/wl55_device/testing/telemetry.md`.

### 26. The loop's slot residual cannot see an alignment error — `defect`

`report_service` computes `slot_at` from `sframe.last_beacon_us` and then
measures the transmit against `slot_at`. **The reference moves with the frame**,
so an error of X in this node's estimate of the hub's boundary shifts the
transmit by X and the residual not at all. The 56 µs spread over 838 frames
bounds the transmit chain alone and says nothing about alignment — it is the
round-trip trap with a different shape, and only the hub sits outside the loop.

The hub's first measurement of the quantity the guard actually pays for is a
529 µs arrival range on slot 1, ten times this node's transmit spread.

Both instruments now exist and neither is the fix: `polled fallback` counts the
cycles aligned from `done_us`, and the residual carries a standard deviation
beside its extremes — sd 5 µs at n = 11, against 8.7 µs implied by the 56 µs
range at n = 838. **The blindness is unchanged**: only the hub can see this
node's arrival against the hub's grid.

Two runs of that residual were once held for pooling on a 273 ppm calibration
difference. **Pooling two blind measurements licenses nothing** — the question
was retired with the instrument, not answered.

**A live instance with a magnitude, 2026-08-22.** Two nodes on the same hub, same
grant, disagree about the superframe: node A measures `per=2010490`, node B
`per=2003781`. Every slot is placed through
`hub_us_to_local(x) = x * measured_us / SUPERFRAME_US`, so a scale differing by
0.335 % multiplies the slot offset:

    slot   0   offset   50000 us  ->    168 us
    slot  65   offset  661000 us  ->   2217 us
    slot 130   offset 1272000 us  ->   4266 us

**`RADIO_SLOT_GUARD_US` is 1400 µs**, so A's later slots are outside the guard
for a reason the guard was never sized against. And A's own `off` reads
643/637/638, flat, because it is measured against the `slot_at` the same wrong
scale produced — the blindness above, now costing milliseconds rather than
bounding microseconds.

The cause is not the grant: B was moved to A's cadence and its period estimate
did not move. What is left is level, A being 30 dB down, which would make
distance damage the clock and not only the SNR.

`Core/Src/device.c` → `report_service`, `Core/Src/radio.c:559`.

### 53. A 23 dB power command arrived as 14, and the cause is not yet known — `defect`

Asked for 23 dB down, from +14 to -9 dBm, the hub's ring measured **14 to 15 dB**
at its input on 2026-08-22. The command took: the console reads back `-9 dBm`,
the level moved, at the announced superframe 694233 and on that board only.

**The mechanism this item first named is wrong, and the correction is the useful
part.** It said the driver holds the datasheet's +14 dBm PA row while sweeping
`SetTxParams` to the bottom. It does hold that row - and so does ST's own
reference. `SUBGRF_SetTxParams` in the Cube FW takes `RBI_RFO_LP_MAXPOWER`, which
is 14 on this board, calls `SetPaConfig(0x04, 0x00, 0x01, 0x01)`, and computes
`power = 0x0E - (max_power - power)`, **which is the identity at max_power 14**.
Every value this driver sends is the value ST would send. Reading the vendor
driver before writing the fix is what stopped it.

**Two candidates remain and the bench does not yet separate them.**

The hub's LNA ladder is often quoted as clearing its front end: 30 dB of receiver
gain moved its printed level about 2 dB, an input-referred RSSI behaving
correctly. But **that ladder ran at -40 dBm**, after this board had already been
turned down. The reading in question was taken at **-25**, and nothing has tested
that point. A compressed reading at -25 reads *low*, so the true step would be
larger than 14 and closer to the commanded 23 - which is the direction that would
explain everything without any device-side fault at all.

The other candidate is the SX126x low-power PA's own transfer curve near the
bottom of its range, which no instrument on this bench has measured. The RTL-SDR
cannot: its bursts clip at 52 % of samples on the rails, and the gain that clears
the clipping is below the burst detector's threshold.

**One real gap was found while checking, and it is fixed.** `REG_OCP` at 0x08E7
is never written here, where ST writes 0x18 - 80 mA - on every `SetTxParams` for
the low-power PA. `SetPaConfig` does not set it on this part. It limits current
rather than gain, so it is not an explanation for a compressed step, and it is
corrected because the driver should not differ from the reference in a way nobody
chose.

**And the instrument half stands unchanged.** `radio power` reported
`radio_power_dbm()`, which returns the byte that was sent, with no reading
anywhere in this firmware able to disagree - the unfalsifiable instrument the
verification skill names, and why the shortfall had to be found from the other
side of the antenna. It now prints `commanded, not measured`.

**No longer held back by a window.** This was parked because item 41's window was
open on both boards and a flash would have ended it; both have been reflashed
since, so what remains is the measurement, not the wait for a slot to take it in.

`Core/Src/radio.c` -> `radio_set_power`, `set_tx_params`, `radio_power_dbm`.
`verification` skill § instruments.

### 52. A reset puts both boards on maximum transmit power, silently — `defect`

`tx_dbm` is `static int8_t tx_dbm = 14;` in `Core/Src/radio.c`, and
`radio_set_power` has exactly one caller: the `radio power` console command.
Nothing persists it and nothing restores it, so **every reset returns the board to
the compiled maximum of +14 dBm** with no record that a level was ever chosen.

It confounded the item 41 window on 2026-08-22 and was found from the other side of
the antenna, because the hub's ring timed the step at each board's own first
post-flash transmission rather than at a shared moment - which is what makes it
firmware and not the room:

    0xc4d444aa   -72 dBm before 15:48:25   ->  -42 / -40 after
    0xdcbac6f5   -48 dBm before 15:52:06   ->  -25 after

**It is the worst member of a family this queue has three of** - a value that
exists only if an operator typed it - because here the default is the *maximum*
and the direction of the surprise is upward. Duty cycle, item 25's governor and
the hub's front-end work are all reasoned about at a power nobody re-asserts after
a reboot.

**The fix is not only persistence: an instrument has to say which value it used.**
`report` and `status` print neither the level nor whether it was chosen or
defaulted. `tx.arm` carries `dbm`, which is the only reason the number was
recoverable at all.

The accident's dividend - the hub's level column passing the 31 dB control it had
been blocked on for a week - is recorded on the hub's page rather than here.

`Core/Src/radio.c` -> `radio_set_power`, `radio_power_dbm`.
`radio_devices_docs/open_hub/radio/configuration.md` § the level column passed its
control. `verification` skill § windows, brackets and the arming.

---

## Debts

### 8. `report_service` blocks the superloop while it waits — `debt`

Two spin waits per cycle hold the core until an absolute instant, so the console
is unresponsive across the beacon window and the slot, and the widened window of
a stale cycle makes that longer. Harmless while the device does one thing;
the blocker for item 1, which needs the same loop to do several.

`Core/Src/device.c` → `report_service`.

### 21. `frame.c`'s replay rule now guards nothing at all — `debt`

The transmit half and the receive half are both closed. `seal_claim` compares the
pair `(superframe, slot)` lexicographically, mirroring the hub; the path that
receives real commands is `downlink_open` -> `downlink_apply` and it consulted no
floor at all until `fix(downlink): a replay floor on the path that receives real
commands`.

**What is left is smaller and worse than when this was written.** `frame_open` had
one caller — the `frame` console command, a loopback instrument — and now has
**none**: it is declared in `frame.h`, defined in `frame.c`, and called by
nothing in `Core/` or `test/`. A correct-looking replay rule with no caller is one
grep away from being read as the production rule again. Either give it a caller
that varies the slot, or delete it and let the production path be the only rule in
the tree.

Bounding the transmit side, which has not changed: three opportunities are
nonce-safe **only because the slot is in the nonce**. Carrying one slot number for
all three would be same-key, same-nonce, three plaintexts.

`Core/Src/frame.c`, `radio_devices_docs/wl55_device/security/replay.md`.

### 32. The RX filter is short of the budget and loses nothing — `debt`

`RX_BW_117300` in `Core/Src/radio.c` is 7358 Hz short of
`2 * (fdev + bitrate/2 + err)` at the hub's measured maximum carrier error of
12329 Hz. **Measured before changing it: 0 beacons missed in 19 cycles.** So the
shortfall is on paper and not yet in the data, at a metre and on a warm board,
and widening costs about a decibel of noise bandwidth for nothing currently
being spent.

The part cannot be set to the hub's 125000 in any case - the SX126x table steps
117300 -> 156200. A second assert in `radio_phy.h` would name 156200, which is
31 kHz past the requirement rather than matched to it.

The cross-direction reading is the argument for leaving it: the receiver short of
the budget loses nothing while the receiver clearing it by 342 Hz loses nearly
every frame to CRC, so filter width is not the deciding term in either direction.

`radio_devices_docs/radio/phy.md`.

### 28. Recovery's predicted tier is unusable cold, not useless — `debt`

Re-acquisition has two tiers: with a measured period it predicts the next
beacon's channel and opens a window, with no usable counter it parks. Across the
first three runs the predicted tier opened **seven windows and aligned on none**,
and the conclusion drawn here was that it had never earned its place.

That diagnosis was wrong and the telemetry stream corrected it. Given a measured
period it aligns on the first window:

    252.018  rec.enter sf=561256 tier=1
    252.120  rec.hit   sf=561271 grid=24 rssi=-17

**102 ms**, twice in one capture. Every earlier attempt started from the stub,
where there is nothing to extrapolate from — so the tier is not code that buys
nothing, it is code entered in a state it cannot work in.

What remains is the entry condition: `recover_service` picks the tier from
`measured_us != 0`, which is true of a period measured before a long outage and
therefore stale. The tier should be chosen by whether the prediction can still be
trusted, not by whether a measurement was ever taken.

Only visible because the records are timestamped; a counter shows the same
"seven windows, zero hits" either way.

`Core/Src/device.c` → `recover_service`, `recover_search`,
`radio_devices_docs/wl55_device/testing/telemetry.md`.

---

## Contract debts

Each of these binds the hub. Agree with the hub session before starting, and
re-measure on air afterwards.

### 9. `UPLINK_AIM_US` is a contract number that lives on one side — `contract`

Where a frame sits inside its slot is the same kind of number as where the slot
sits inside the superframe, and it belongs in `Common/inc/radio_slots.h`. It is
700 µs here — the frame centred in the slack — and appears nowhere in the hub,
which assumed flush at slot start. Both sides stayed internally consistent while
computing about different geometry, and it has already cost one wrong guard
analysis.

**Agreed with the hub session 2026-08-22: it goes into the shared header, once
the anchor is trusted.** That waits on item 12 — writing an aim into the contract
while the boundary it is measured from is in dispute would pin the wrong geometry
in the one place both sides compile.

**"No experiment is needed for the aim itself" was written here and is wrong.**
It was measured on 2026-08-22 from both sides of the antenna at once, and 700 is
not what leaves:

    node        this side, `off`      hub, from arrival_sync_us
    0xc4d444aa  634 µs  sd 4  n 114   659 / 616,  n 2
    0xdcbac6f5  586 µs  sd 5  n 114   574,        n 1

Two instruments sharing no code, no clock and no side of the antenna agree per
node to 3 and 12 µs, and both put the frame **65 to 115 µs early** against the
compiled 700. They also reproduce the difference between the boards - 48 µs by
one instrument, 64 by the other - so the boards genuinely differ and it is not
one instrument's bias.

`off` is an upper bound on the aim, not an estimate of it: it is measured as
`micros() - air - slot_at + ramp`, and `ramp` is `air - on_air`, which carries
the TxDone interrupt latency. The true landing is earlier than the table says,
which widens the gap rather than closing it.

So the correction is not to the reasoning but to its remedy: writing 700 into the
shared header would pin a number **neither firmware implements**, which is this
item's own hazard reappearing inside its own fix. Whatever goes in has to be the
measured aim or a lead that produces it, and `UPLINK_LEAD_US` is where the error
is - `2882 - UPLINK_AIM_US + 268` models a ramp and a SetTx lag that together
overstate the real path by about 100 µs. Landing early eats the guard on the safe
side, which is why nothing has failed and why nothing found it either.

`verification` skill § know which artifact each assert pins.

### 10. No constant for radiated energy — `contract` `debt`

`RADIO_AIR_START_TO_END_US` counts modulated bits, and two consumers roll back
from the first bit using it, so it cannot be widened to include the PA ramp. The duty
cycle needs a third constant that does. **Model only, never a roll-back** — it
must not be wired to anything that suppresses a transmit.

`radio_devices_docs/radio/phy.md` § duty cycle.

### 11. `RADIO_SLOT_GUARD_US` has never been sized from a measured ramp — `contract`

1400 µs is an assumption. The configured TX ramp is 200 µs and the ramp-down is
unmeasured from outside; the SDR capture that would measure it is a bench item
below. Sizing the guard from a number nobody has taken is how the slack ends up
in the wrong place.

**Item 4 made this decision-relevant**: at the slot widths that meet the
deadline, 400 µs of guard is worth about five devices, and the measured transmit
spread is 56 µs over 758 frames — on slot 1, 69 ms after a beacon, which is the
easy case. A late slot free-runs 1.8 s and has never been measured.

A hand-assigned slot was refused — the operator's rule is that slots are never
handed out by hand — and it is not needed: aligning on every Nth beacon buys the
same free-run time in slot 1, and item 4 needs that machinery anyway. It bounds
rather than reproduces the k=3 case, because skipping beacons ages the period
estimate as well as lengthening the extrapolation, and k=3's later opportunities
have a fresh estimate. **Only the hub can take the number** — the device's own
+636 µs is measured against its own prediction.

`radio_devices_docs/radio/tdma.md` § lead time and guard band.

### 12. `BEACON_BOUNDARY_LAG_US` and the hub's number cannot both be right — `defect` `contract`

260 µs ± 5 here, from the hub's pooled n = 61 over two runs. The hub measures
its own boundary-to-first-bit directly as **358..366 µs over 529 beacons**.

**This is no longer "take the better number".** `start_us` already subtracts
`RADIO_PRE_SYNC_US`, so what this constant covers is the hub's boundary-to-first
-bit **plus this receiver's detect-to-timestamp residual**. The hub's figure is
the first term alone. Mine must therefore be *larger* than the hub's, and it is
100 µs smaller. A positive residual cannot subtract.

So one of three things is wrong: the hub's 358..366, this side's 260, or the
pre-sync subtraction over-correcting. Adopting the hub's number would move this
anchor 100 µs and might land correctly for a false reason, which is worse than
leaving it. **Neither number is to be written into the shared header until the
discriminator below has run.**

The pre-sync term is **not** the explanation, and it is now less able to be one
than when this was written. On 2026-08-22 it was two definitions of one quantity —
`RADIO_PRE_SYNC_US` here beside `RADIO_PRE_SYNC_AIR_US` in the shared header,
both derived from `RADIO_US_PER_BYTE`, both 1280 µs. The local one is deleted and
the shared one is `RADIO_AIR_START_TO_SYNC_US`, so there is no longer a second
value that could have differed. Checked 2026-08-22, and the hypothesis it ruled
out can no longer be reintroduced.

**The discriminator**: node B forges a beacon on its own boundary while node A
reports the computed lag. Node B's boundary-to-first-bit is its own transmit
path, measurable locally with `radio slot`, so the difference isolates this
receiver's residual with nothing else in it. Same boot as `report opp 2`, so it
does not become another two-window fraction. Hub side is item 31.

It also bundles this receiver's demod pipeline with the hub's transmit path, so
a change to either side's PHY invalidates it and nothing in either tree checks
that it is still true.

`radio_devices_docs/wl55_device/radio/timebase.md`.

### 13. The data beacon is unauthenticated — `contract`

So the quiesce flag is a denial-of-service primitive. This side's mitigation is a
per-beacon clamp and a minimum gap, which bound one forgery and a rate — neither
is a cryptographic guarantee. The fix is a network broadcast key and it changes
both firmwares at once.

`Core/Src/beacon.c:93`, `radio_devices_docs/radio/known-issues.md`.

### 14. GCM partial final word — `contract`

`HAL_CRYP_Decrypt` does not mask the unused bytes of a partial final word while
encrypt does, so every length not a multiple of four fails with byte-perfect
ciphertext. **Not fixed — contained** by zeroing the input buffer before the
copy, which a new code path can forget. Found on this side and present in the
hub's HAL family too.

`radio_devices_docs/radio/known-issues.md`.

### 15. Key rotation: generation 0 only — `contract`

`R(0) = Z`, so generation 0 is `pair_v2`'s pinned session key and no vector moves
when the ratchet is written. The store already carries `key_gen` and clears the
replay floor with each new session key; nothing rotates one. The hop key has no
rotation path at all, and that is chosen — `PAIR_ACCEPT` is the only sealed
downlink that exists.

`radio_devices_docs/radio/crypto/key-lifecycle.md`.

### 22. Three transmits a superframe is over the 1% duty cycle — `defect` `contract`

`radio/phy.md` calls 1% the most binding constraint in the project. A device
using all three of item 4's opportunities every superframe is over it, and the
number moved when the wire did:

    wire            frame     k=3      k=2
    42 B, old      6720 us   1.008%   0.672%
    50 B, link_v4  8000 us   1.200%   0.800%
    54 B, 8 B pre  8640 us   1.296%   0.864%

**The 1.008% first written here was computed on the 42-byte wire.** link_v4 took
the frame to 50 bytes and the sustained k=3 figure to **1.200%** - twenty percent
worse, and it moved silently because nothing recomputes it. At 25 kbps it is
2.400%, so the deadline retires that rate for event-capable devices outright.

**This is now the only duty-cycle constraint still binding anything.** The hub
found that `RADIO_DOWNLINK_EVERY 2` was justified by a 1.42% figure that had been
computed on a 31-byte downlink; at 39 bytes and 50 kbps the real total is 0.800%,
and three copies of the stale 0.74% survived the change - including a comment
sitting beside the assert that recomputes it correctly on every build. Half rate
is a choice there now, not a requirement (their item 33). **The uplink k=3 figure
above is the one that still refuses.**

k=2 is 0.800% and still safe, but its margin fell from 0.33 to 0.20 points.

The 8-byte row is tonight's experiment, not a proposal: it is the cost if the
preamble arm turns into a default. The bench cadence is one cycle in eight, so
this board is at 0.162% and never approached the bound.

Nothing on either side would refuse: every individual frame is legal and no
governor exists. The bound has to be stated in the contract, not inferred.

**At this margin the SDR cannot answer the question either.** Attribution
depends on selecting bursts by air time, and 50 kbps shrinks the frames while
k=3 multiplies them. The method needs re-validating before it gates anything.
Item 10 stops being tidiness here: at 1.008% the ramp decides the verdict.

`radio_devices_docs/radio/phy.md` § duty cycle.

### 23. The downlink's `slot` field has three candidate meanings under k=3 — `contract`

`downlink_open` refuses on `f[2] != join_res.slot` and feeds the same byte to
the nonce. With three slot numbers meaning one device, the contract must say the
downlink always carries the **base** slot. A one-line predicate on both sides,
and picking differently reads as a tag failure rather than as an addressing
disagreement.

`Core/Src/device.c` → `downlink_open`.

**The `dl_served` guard is load-bearing for the crypto, not only for the
schedule.** The downlink nonce is `(superframe, dev_id, direction, slot)`, and
for one device in one superframe every one of those is fixed. A second downlink
to the same device in the same superframe would be the same key and the same
nonce over a different body. Whatever the contract ends up saying the `slot`
field means, it must not make that reachable.

### 25. No duty-cycle governor, and it has to be durable — `defect` `contract`

1% is 36 s of air an hour. **The 2.976 transmits a superframe first written here
was the 42-byte wire too**; on link_v4's 50 bytes the same budget is 4500 frames
an hour, **2.500 a superframe**. So the bound on item 22 is **a budget over the
hour, not an integer per superframe**: a per-superframe rule forbids bursts the
regulation allows, which is the wrong shape for event-driven traffic, and an
integer rule would now have to be 2 rather than 3 in any case. Neither firmware
has either.

**A budget in bytes, not in frames**, or the next wire change moves it again
without anyone noticing - which is exactly what happened to both numbers here.

**A bucket held in RAM is refilled by every reset**, so it governs sustained
load and not a device in a reset loop, which is the case most likely to need it.
The store's counter reservation already amortises a periodic write against
wear; this needs the same treatment.

`radio_devices_docs/radio/phy.md` § duty cycle.

---

## Bench debts

### 19. Node B's PA ramp has never been captured from outside — `hub`

Offered by the hub session, which owns the SDR. It is the measurement item 11
needs, and the only evidence available about what leaves the antenna before the
first modulated bit.

`radio_devices_docs/open_hub/testing/sdr.md`.

### 20. Cold start is untested

Oscillator settling is temperature dependent and every sweep was at room
temperature — including the 2.4 ms TCXO wait that the whole RX lead time is built
on. If devices are specified below freezing this needs re-measuring, not
assuming.

`radio_devices_docs/radio/tdma.md` § lead time and guard band.

### 74. The convention checker reads a dereference as a comment — `debt`

`check_conventions.py` treats any line whose first character is `*` as a comment
continuation, so a wrapped expression beginning `*out = ...` is measured as part
of the block above it and reported as an over-length comment at a line holding no
comment at all. It cost one function a rewrite on 2026-08-24 to satisfy a rule it
was never breaking.

The predicate wants to be `* ` or `*/` rather than `*`. **Both trees carry a copy
and `tools/test_check_docs.py` asserts they agree**, so this is two edits and a
shoulder in `tools/test_check_conventions.py`, not one edit.

`tools/check_conventions.py`.

### 54. `radio init` reports bench mode that a service silently takes back — `debt`

`radio_configure(slot)` passes `want_bench = 0`, so every service-path retune —
`report_service`, `recover_park`, the join rendezvous — clears `bench_mode` and
rewrites the protocol sync word. On an armed board `radio init` prints
`bench 869500000 Hz sync "benc"` and the next read of `0x06C0..0x06C3` returns
`68 65 6C 6C`, the protocol word. Measured on node A 2026-08-22, four registers
read one command after the retune.

Nothing is wrong with reverting: an armed board belongs on the protocol air. What
is missing is that the console said bench and nothing said it stopped, so a bench
measurement taken while `report` is armed is on the protocol carrier and sync
word under the bench's name.

`radio_devices_docs/wl55_device/radio/driver.md`.

### 57. The console is starved while the enrolment window is open — `debt`

`invite_service()` holds a receive slice, and the console is not serviced inside
it. Measured on node A 2026-08-23: a command written to the VCP answers in more
than 2.5 s and less than 5 s while the node is unpaired and listening, against
well under 1 s once paired.

Nothing is wrong with the pacing. What is wrong is that **a read that times out
is indistinguishable from a dead board**, and the board goes on emitting its
telemetry beat the whole time, so the one instrument that says "alive" is the one
a console read does not use. It produced twelve false negatives in one session:
two harnesses reported zero pairing attempts by a node that was listening
correctly.

`radio_devices_docs/wl55_device/testing/bench-harness.md`.

### 61. The WL55 hub role never got ADR-0026, and the fix is a host test — `blocking` `hub`

Eleven exchanges on 2026-08-23, node B built `-DWL55_ROLE=HUB`, node A on
`632b261`:

```
tx      invites 11 (err 0)  beacons 33
rx      sync 11  crc err 0  frames 11
req     seen 11  rsp sent 11
conf    seen 0
accept  sent 0  paired 0  timeouts 11
```

**Eleven of eleven, deterministic.** `sync == frames` exactly, so the hub heard the
eleven requests and never a sync word for a confirmation.

The cause is known and it is not a radio: `hublogic.c` arms
`HUB_EX_TIMEOUT_US = 2 * SUPERFRAME_US` from the response and knows nothing of
`RADIO_PAIR_CONF_REGION`. This is the hazard the aggregate roadmap's phase 8 named
and mis-scoped — *with step 4 alone the device holds a confirmation the hub is no
longer waiting for* — and step 3 landed on the H755's CM4 while step 4 landed
here. **The WL55 hub role is a third image nobody counted.**

**The cost reaches past this bench.** The WL55-to-WL55 control produced the **4 of
4** baseline the pairing page quotes against the H755's 5 of 10, and that fixture
cannot now complete a pairing at all. Every comparison against that baseline is
suspended. It also removes the deterministic fixture: the H755 completes an
enrolment about half the time, so a control that has to *fail* eleven times out of
eleven to mean anything cannot be run against it.

#### Re-scoped 2026-08-24: not a hand-port, a host test

**This entry used to say build ADR-0026 into `hublogic.c`, and that is the wrong
remedy for the right problem.** `hublogic.c` is a second copy of the hub's
exchange logic, and it went stale in exactly the way ADR-0028 was written about:
*a contract each side owns a copy of is a contract one side can revise alone.*
Hand-porting the region model into it now buys a fixture that will go stale again
at the next ADR reaching the wire, and adds a third implementation to keep in
step until phase 9b subsumes it.

**What to build instead is a host test for `hublogic.c` against a fake PHY.** It
is the one piece of exchange logic in either tree that compiles for the host
today — *the same protocol with no chip and no mailbox in it* — and the seam it
reaches the radio through is the same eight calls
`OpenHub/CM4/test/test_phy.c` already fakes on the other side. Making it
ADR-0026-correct **under test** buys three things where the hand-port bought one:

- the exchange scheduler becomes deterministic on a PC in seconds, where today
  every one of its states is an air test;
- the WL55 hub role comes back **as a by-product** of the same change, built for
  the board rather than maintained for it;
- it is phase 9a's third step
  ([phy-seam.md](../radio_devices_docs/radio/phy-seam.md) § order of work) and the
  bridge into 9b, so the work counts twice instead of being spent on a fixture.

#### What the fixture is actually for, and what it is not

Three values, and only the third is unique to it. **The second PHY as a control**
is now partly delivered otherwise: the host suite separates logic from driver by
removing the driver, which is stronger for a deterministic logic fault and cannot
see anything that depends on time or air. **A deterministic fixture** — 11 of 11,
4 of 4 — against the H755's coin flip is real and is worth having. And
**localising K2 to the RFM69 side**: a WL55-to-WL55 link takes that part out of
the question entirely, so pairing that works there and fails against the H755 puts
the fault on the RFM69's side of the antenna. Nothing else on this bench does that.

**It is not on the path to hub item 60.** The H755 already implements ADR-0026's
region model — `ex_due_frame = ex_req_frame + 1`, `RADIO_EX_RSP_DUE`,
`join_window_holds()`, and a `join_rx_deadline` set afresh per region — so its
confirm leg is chased in its own source and its own counters. Item 60 carries that
and does not wait on this.

#### Built 2026-08-24, and the stated cause does not reproduce

`test/test_hublogic.c`, 53 checks, `hublogic.c` driven through a fake PHY with a
device on the other side that keeps ADR-0026's timing. **The exchange completes.**
Request seen, response sent, confirmation heard, grant sent, paired, and no
timeout - against a device that holds its confirmation to
`RADIO_PAIR_CONF_REGION` past the invitation, which is the thing this entry said
`hublogic.c` knows nothing about.

**The negative is a real negative and not an untested path.** Mutating
`HUB_EX_TIMEOUT_US` from two superframes to one turns the accept, the pairing and
the held-time check red, so the suite does catch this class of defect. It is not
present in this code: the deadline is armed from the *response*, and the response
is only ~41 ms after the invitation, so two superframes from there still covers a
confirmation due two superframes after the invitation.

**Two numbers replace the argument, and the second is the binding one:**

| | measured |
|---|---|
| deadline slack - how late the confirmation may be before the *timeout* refuses it | **41 040 us**, which is exactly this hub's invitation-to-response turnaround |
| guard band - how late before the *window* has already closed under it | **106 660 us** past the region |

The window is the constraint, not the timeout. A confirmation later than ~107 ms
past its region point lands where the hub has put the part in standby, and is
**never heard at all** - which is the only shape that reproduces this entry's
bench signature: request seen, response sent, `conf seen 0`, timed out. Later
still is heard again, in the next superframe's window, so *later* is not a
diagnosis on its own; it is a band and not a cliff.

**So the eleven-of-eleven is bounded rather than explained.** Either the device's
confirmation was more than ~107 ms past its nominal region point, or it was never
transmitted. That run recorded the hub's counters only; `stats.conf_sent` and
`stats.invite_to_conf_us` on the node settle it in one reading, and until it is
re-run this entry has a symptom and no cause.

**A control that says why ADR-0026 exists, now reproducible in a second.**
Pre-ADR-0026 timing - the confirmation 105 ms after the response - is *also* lost,
in the dead zone before the next window opens. That is hub item 60's original
mechanism, on a PC, without a board.

**No change to `hublogic.c`.** Re-anchoring the deadline to the invitation would
be principled and would buy nothing: the window closes first either way, and it
would put an untested change on the path that is about to be re-measured.

#### Closed 2026-08-24 on the bench: the hub was talking over it

Run `2026-08-24-3`. **Both of the previous section's possibilities are wrong.**
The device transmits the confirmation - `conf sent 38` against the hub's
`conf seen 0` - and it is not late: `invite -> confirm` read 4 008 911, 4 008 923
and 4 008 930 us against a 4 000 000 us schedule, **under 20 us of variation over
twenty minutes**, and 8.9 ms inside a guard band the host test measured at
106 660 us.

**The third instrument found it.** `decode.py` over a 60 s bladeRF capture of the
join channel decodes the invitation, the request, the response **and the
`PAIR_CONF`** - so the confirmation was radiating the whole time. Beside each one,
4.13-4.31 ms ahead of it, sits the hub's own `JOIN_BEACON`, whose air time is
4.38 ms. The two transmissions overlap, and `phy_sx126x.c:30` holds the receiver
off for the whole of a blocking send.

**Even lands on even.** `hublogic.c` invites on `frame_counter % 8 == 0` and
otherwise beacons on `% 2 == 0`; the device answers `RADIO_PAIR_CONF_REGION`
(**2**) superframes later at the same phase, and `8k + 2` is even. That is why the
failure was 11 of 11 and then 38 of 38 rather than a rate.

**The clause was already implemented on the H755** - `pair_region_owned()` at
`OpenHub/CM4/Core/Src/radio.c:1906` suppresses the beacon while the exchange owns
the region. This role had no such guard. **The title was right and the cause was
wrong**: the host test refuted the timeout anchor, and the bench found the missing
ADR-0026 clause.

`hublogic.c` now carries the same guard, bounded to `RADIO_PAIR_CONF_REGION`
regions from the invitation. **One beacon in eight is deliberately not sent** -
a real cost to a node still searching, which is why it is bounded rather than
"silent while busy". First exchange after the reflash: `conf seen 1`,
`accept sent 1`, `paired 1`, `level confirm -43 dBm` where it read `0`, and node A
`paired yes hub 574C3535 net 0001 slot 0`.

**The host test caught it before the board confirmed it**, once the fake stopped
being generous in two ways this run exposed: `phy_transmit` left the hub
listening through its own transmission, and nothing watched what the hub put into
the region. `case_hub_is_silent_in_the_region_it_listens_in` was **red against the
real defect and green after the fix** - a control nobody had to inject.

**What is still owed:** the WL55-to-WL55 baseline the pairing page quotes was
taken on a fixture that has now changed, so re-measure it before quoting 4 of 4
again. Item 81 carries the anchor discrepancy this run found and did not settle.

`radio_devices_docs/radio/decisions/0026-one-turn-per-join-region.md`,
`bench/runs/2026-08-23-3/RESULTS.md`, `bench/runs/2026-08-24-3/RESULTS.md`. Related: item 80, the backend under this
seam has no host test either.

### 58. A device with no console still cannot be released — `blocking`

**Narrowed 2026-08-23.** The half that needed a debug probe is built:
`store_release_pairing()` appends a record that drops the session, the hop key,
the hub's static key, the slot, the rate, the replay floor **and the network
binding**, and keeps `priv`, `dev_id` and `counter_mark`. So a released unit comes
back with the **same id its label carries** — which an erase of pages 126/127
could never do, and which cost this bench six identity changes in one day.

`device_release_pairing()` clears the RAM copy too and re-anchors the enrolment
window on the release, because [ADR-0024](../radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md)
bounds listening to a physical act and boot was being treated as the only one.
The console command is `release`, and it is the first on this node that writes.

**What is left is the product route, and it is a decision rather than a defect.**
Under `-DWL55_CONSOLE=OFF`, which RG-T-2 requires to link and which a shipped
sensor is, there is still no way to release. The candidates are a button held at
power-up, a sealed downlink from the hub, or an accepted answer that a sensor is
released only at a bench — and the second cannot cover the case this item exists
for, which is a hub that is *gone*. Nothing should be built here until that is
chosen.

**Not verified on hardware, and it is no longer blocked on anything.** The claim
here was that no route to a paired node existed: the WL55-to-WL55 fixture cannot
complete an exchange (item 61) and the H755's roster was full. **Both halves have
moved** — the hub's store was rebuilt on the ADR-0027 ring and node A completed a
full four-frame exchange into slot 2, so a real pairing is available to release.
What has run so far: both console arms link and flash, and the console-off
build's record stream is unchanged — the same identity, off flash. The control
still owed is unchanged and now runnable: release, read `ident`, confirm the id
did not move, then pair and check the id the hub sees.

`radio_devices_docs/radio/decisions/0024-the-device-id-is-the-whole-enrolment-anchor.md`.

### 77. The k = 3 geometry lost its only writer in the console move — `defect` `blocking`

`report_opp` and `report_opp_all` are declared at `Core/Src/device.c:380` and
`:382`, read at `:707`, `:708` and `:713`, and **assigned nowhere in this tree**.
Both are file-scope statics, so both are zero for the life of every image, and
`k_first = k_last = 0`: the report loop runs once, always on `k = 0`.

**They had a writer and it was the console.** `cli.c` carried
`report opp <k|all>` — `report_opp_all = 1u` under the comment *"Same grant, a
later slot: the k=3 geometry used as an instrument"*. `08e244e`, *"the console
stops being load-bearing"*, moved the reporting loop into `device.c` and **took
the three reads without the write**. Nothing since has replaced it, by console or
by grant.

That splits this into two facts that are easy to conflate:

- **k = 3 has never been autonomous.** It was always an operator typing a command.
  The 20 % figure on [`radio/tdma.md`](../radio_devices_docs/radio/tdma.md) — 151
  cycles, 453 frames, 39 / 15 / 6 % per opportunity — is a real measurement taken
  with that command held on.
- **Today it cannot be reached at all**, so even the instrument is gone, and every
  delivery figure measured since `08e244e` is a **single-opportunity** figure.

**Two instruments say so and they were not asked to agree.** Regression run
`2026-08-24-1`, check RG-A-5: measured duty cycle **0.043 %**, against 0.050 %
predicted for k=1 and 0.150 % for k=3 at `report every 8`. `airgrid`'s C3 reports
`[3, 0, 0]` on the same window — its `[3, 1, 0]` counts the 868.000 MHz neighbour
at 16 dB SNR as an uplink, against this system's own 66 dB.

`RADIO_SLOT_OPPS` is 3, carries a static assert, is the denominator of the
duty-cycle tripwire in `radio_slots.h`, and is the whole of **REQ-F-10**'s
argument that the event deadline is met by three opportunities inside one second.
**That argument has never had a mechanism under it.**

**The instrument is back and the item is not closed.** `opp <k|all>` is restored
under `-DWL55_DEV_COMMANDS=ON`, off by default, so the product console keeps the
read-only property `device.h` states and the developer build breaks it in one
named place. Verified on node B in both directions: the dev image answers
`opp all` with `opps all 3 of 3  slots 0/65/130` and refuses `opp 9`; the product
image's own help does not list the command and still says `release` is the only
one that writes.

**`state` now prints the regime in both builds** — `opps 1 of 3  k=0  slot 2` —
because run `2026-08-24-1` had to infer `k` from a duty cycle, and no run should
have to again.

**What is still owed is the autonomous grant, and it is a design question**: a hub
grant in the downlink — what the field name and `arm_opp`'s `0xFF` imply, and what
item 78 says cannot be delivered today — a local policy on a missed
acknowledgement, or a compile-time default. The duty cycle constrains the answer
rather than settling it: k = 3 is 1.200 % *every superframe* and 0.150 % at
`report every 8`, so the grant must carry the cadence with it.

**REQ-F-10 stays `not met` until that lands**, because an operator holding a
console command is not a mechanism a requirement can rest on.

**Nothing may quote three opportunities until this closes**, including
`radio/tdma.md`'s deadline argument and REQ-F-10's stated state.

### 78. The device hears almost nothing the hub sends — `defect` `blocking`

Regression run `2026-08-24-1`, checks RG-A-1 and RG-A-3, and it is the first
window that held both instruments at once.

| the air, over 31 superframes | this device |
|---|---|
| **31 beacons, one per superframe**, each on a grid channel | node A: `sync.lost since=4000027` repeatedly, `since beacon` 5.8–12.9 s |
| ~~**15 downlinks**~~ | ~~node A: **`downlinks 0 of 5`**~~ — **withdrawn, see below** |
| join-channel bursts on channel 14 | node B: **0 invitations across 68 listening slices**, `sync=0` for a whole 10-minute window |

**The middle row said the opposite of what it was read as, and regression
`2026-08-25-1` withdrew it.** `downlinks A of B` is `dl_applied` of `dl_opened`:
`dl_opened` counts every downlink that passed its tag and its replay floor, and
`dl_applied` only those carrying a **new command** — a keepalive returns before
the increment. So `0 of 5` reads *five downlinks arrived and none carried a
command*, and the hub agrees from the other side of the antenna with
`cmds 0 sent, 0 acked, 0 lost`. It was never evidence that the device heard
nothing; it is evidence that it heard five.

**This entry's conclusion is unchanged.** The other two rows are independent, and
the channel split below is stronger than any of them. What changes is that one of
the three witnesses was miscounted, and the console's heading is what misread —
both counters are incremented correctly, on the right paths.

**And the same question now has a far better answer.** In that run's window the
hub reported `downlink 6978 sent` while node A reported **1738 opened**, both
counters read within the hour over the same ~28 000 s: **24.9 % of hub→device
frames on the grid**, on a population four times the 31 superframes above.
`../bench/runs/2026-08-25-1/RESULT.md`.

The hub's transmitter is not the suspect: the capture holds its frames. The band
is not the suspect either — `bandscan.py` over the run's own window puts all 29
grid channels at 1.17–1.49 % with nothing foreign on any of them.

**Losing sync costs the period estimate, which costs the next beacon.**
`SUPERFRAME_FRESH_US` is `2 * SUPERFRAME_US`, so two missed beacons drop sync;
`superframe_align_at()` then clears `measured_us` after `SUPERFRAME_RESYNC_AFTER`
refusals and re-enters the two-beacon bootstrap. Node A reported
`per=2006987 us` against a nominal 2 000 000 — **6987 µs per superframe against a
1400 µs guard**. The estimator itself is sound: `test_period` holds ±18 µs over
401 estimates and its own two-beacon control sits at +563 µs. It is being starved,
never reaching the 64-superframe baseline.

So this is a loop rather than a defect in one place, and the entry point is not
yet known. What separates *the logic is wrong* from *this driver is wrong* is a
second PHY running the identical logic, which is exactly what
[ADR-0028](../radio_devices_docs/radio/decisions/0028-the-radio-is-a-library-and-the-region-is-a-compile-time-profile.md)
§9a sequences early and what `-DWL55_ROLE=HUB` already half-provides.

**The loss splits by channel, measured 2026-08-24 in run `2026-08-24-4`.** The
same two boards, minutes apart, in one window:

| hub -> device | channel | delivered |
|---|---|---|
| `PAIR_RSP`, `PAIR_ACCEPT` | 866.5 MHz, **fixed** | **7 of 7** |
| join and data beacons | the 29-channel **hopping grid** | ~1 in 8 |

Both nodes read `clock ... stale` with **15.6 s since the last beacon** against a
2 s superframe, while paired and delivering - and in the same run not one
hub->device enrolment frame was lost. **The downlink is not broken in general; it
is broken on the grid**, which points at the hop sequence or the retune rather
than at the transmitter or this receiver's sensitivity, and neither was a suspect
before.

It is an inference across two frame classes that differ in length and timing as
well as in channel, so it is a hypothesis with evidence rather than a settled
fact. **The arm that settles it is cheap**: hold a device on the join channel and
compare beacon reception there against the grid.

**And the period estimate does not order by level.** Node A is 11 dB stronger at
the hub and reads `period 2 007 280 us` - 3 640 ppm, the same figure this entry
already records - while node B reads `2 000 943 us`, 470 ppm. The weaker board has
the better estimate, which the starvation loop above does not predict.

### 79. `linkjoin.py`'s `hub - nominal` column compares two different origins — `defect`

It prints ~9200 µs on every delivered uplink, against a slot pitch of 9400 and a
guard of 1400. Read as written that says the device lands a full slot late, which
is precisely the mechanism K2 has been looking for. **It is not true.**

`arrival_us` is stamped **after the decrypt** — `../OpenHub/Common/inc/ipc.h:535`
says so, and `../OpenHub/CM4/Core/Src/radio.c:2036` computes it as
`rfm_micros() - superframe_start_tk` at that point. The column's `nominal` is a
first-bit figure. Subtracting the device's own in-slot offset and
`RADIO_UPLINK_AIR_US` leaves **656 µs and 532 µs** on the two delivered frames of
run `2026-08-24-1` — the decrypt, and the same both times.

The uplink placement is correct. Either subtract the air time and label the column
what it is, or have the hub stamp at the first bit — which is `hub` item 44 and
the better fix, because a timestamp taken after a variable-length operation is not
an arrival time.

**Nothing has been quoted from this column yet.** Fixing it before something is
is the whole point of writing it down.

### 80. `phy_sx126x.c` has no host test, and the hub's backend now does — `debt`

The hub cut `phy_poll` on 2026-08-24 and got `OpenHub/CM4/test/test_phy.c` with
it: 95 checks against a fake part, three mutation controls, and three deliberate
defects it was pointed at to prove it can refuse
([host-tests.md](../radio_devices_docs/open_hub/testing/host-tests.md)). This
tree's backend has none, and the same afternoon showed why that matters.

**The concrete miss.** `phy_ev_t` gained `lna_gain`, where `0xFF` means *unknown*.
This backend zeroes the event and returned 0 — which is **G1**, a gain a receiver
really does choose. Nothing here would have said so; it was found by reading the
hub's new field, not by a check. The sentinel is now `PHY_LNA_UNKNOWN` with a
`_Static_assert` in `phy.h` so both builds hold the constant, but **nothing tests
that this backend uses it**, and the next field the contract gains has the same
shape of trap waiting.

What it needs is what the hub's suite needed: shims for the board and the clock,
and a fake part under `radio_listen_poll`. The SX126x's is heavier than the
RFM69's, which is the reason this is an entry rather than a commit — the driver
here reaches the part through the SUBGHZ HAL rather than through injected
callbacks, so the seam to fake is further down.

**It is also the missing half of the control the seam exists for.** Identical
logic over two PHYs separates *the logic is wrong* from *this driver is wrong*
only when both backends are known to honour the same contract. One of them is now
checked on a PC and the other is checked by reading it.

### 81. The device's `t_beacon` is 11 ms from where the source says it is — `defect`

`join.c:292` takes `t_beacon = micros()` **after** `receive_until` returns, which
reads as the moment the invitation finished arriving. Two independent
measurements in run `2026-08-24-3` put it within a millisecond of the
invitation's **start**, about one whole invitation air time earlier:

| | end-anchor predicts | start-anchor predicts | measured on air |
|---|---|---|---|
| invitation -> request burst | 10 824.4 ms | 10 812.5 ms | **10 813.3 ms** |
| invitation -> confirm burst | 14 780.5 ms | 14 768.6 ms | **14 767.9 ms** |

The node's own counter agrees with the air and not with the source:
`beacon_to_req_us` is 43 864 us, while the end-anchor requires about 32 000.

**It is not academic.** Everything the region schedule promises is measured from
this instant, `invite_to_conf_us` is quoted against it, and the host test's fake
anchors the way the source reads - which is why `test_hublogic` could not
reproduce item 61's collision until the invariant was stated in terms of the
hub's schedule instead of millisecond arithmetic.

Same family as item 79: two origins compared as though they were one. What
settles it is a `micros()` read on both sides of `receive_until` printed once,
against the capture.

`radio_devices_docs/wl55_device/radio/pairing.md`,
`bench/runs/2026-08-24-3/RESULTS.md`.


### 82. `TLM_TX_DENY` names one gate whatever shut the other — `defect`

The record that repeats every cycle is emitted with a **hard-coded**
`TLM_WHY_FLOOR`:

```c
if (!may_send) {
    tlm_emit(TLM_TX_DENY, sf, TLM_WHY_FLOOR, 0u, 0u);
```

`tx_allowed()` has two gates - `tx_floor_known`, and the floor comparison itself -
and `TLM_TX_HOLD` distinguishes them correctly, `tx_floor_known ? TLM_WHY_FLOOR :
TLM_WHY_NODOWNLINK`. But `TX_HOLD` fires **once**, behind `tx_hold_said`, so it is
gone by the time anyone attaches to the VCP, while `TX_DENY` repeats forever with
a constant in the reason column.

**Cost, 2026-08-25.** A board that had opened no downlink since boot - so
`tx_floor_known` was 0 and the reason was `NODOWNLINK` - emitted `tx.deny why=8`
after every beacon it heard. `8` is `TLM_WHY_FLOOR`. Read at face value it says a
floor exists and the superframe has not passed it, which is the opposite of the
truth and points a reader at the hub's downlink content rather than at its
absence. The console said the right thing in the same minute - `tx floor none yet
- no downlink opened since boot` - so the two instruments disagreed and only the
one nobody was watching was correct.

**It is the `verification` skill's category in its cheapest form**: not a wrong
number but a *category* in a reason column, where every value is a small integer
and nothing distinguishes a constant from a reading. The fix is one expression,
already written correctly eight lines above.

`Core/Src/device.c` -> the `may_send` block; `radio_devices_docs/wl55_device/`.

Related and **already fixed** in the same session: `dl_repeats` and `dl_replays`
were incremented and read nowhere - not on the console, not in the snapshot - so a
node refusing every downlink was indistinguishable from a hub sending none. They
now print with the durable floor beside them as
`dl <n> repeat, <n> replay, floor <n>`, which is what separated the two candidate
causes of hub item 98 in one read.
