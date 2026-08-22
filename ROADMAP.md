# Roadmap

The single list of open work on the device: debts, defects, and design that was
agreed and never built. **Nothing here is reasoning** — every item names the page
in `../radio_devices_docs` that holds the why, exactly as source comments do. If
an item needs a paragraph to justify it, that paragraph belongs on its page and
the line here shrinks to a pointer.

An item leaves this file when it is done, not when it is understood.

**Cite a tag or a commit message, never a bare hash.** The history here was
rebuilt into eleven themed commits on 2026-08-21, and every hash written before
that stopped being reachable from `master` the moment it was. Three items in this
file cited one; all three survived only through `pre-squash-2026-08-21`, and none
of them failed loudly. A tag survives a rebuild and a hash does not.

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

### 1. Nothing on this device runs without a typed command — `blocking`

Every autonomous-looking behaviour is armed from the console and none of it
survives a reset. `report_armed` is a RAM flag set by `report on`, and an
invitation is only heard inside the window `join invited` opens. `main()` is
`CLI_Poll(); report_service();` and nothing else.

**The downlink clause is done**: `report_service` opens the region every cycle
between the beacon and the first uplink slot, applies the command and echoes its
`cmd_seq`. Measured end to end - the hub commanded rate 5, the cadence moved
from every 8 superframes to every 5, and the hub's `acked` counter left zero for
the first time.

What is missing is a state machine that boots into: restore the grant, camp on
the beacon, listen for an invitation while unpaired, report while paired. The
pieces all exist and are verified separately — `join_restore` already runs from
`CLI_Init`, so a reset does not demote a paired device.

**One clause of this is now done.** Re-acquisition after a lost counter runs
from the superloop and needs nothing typed: measured on 2026-08-21 from a cold
reset, `transmit gate: open` and a measured period about 60 s later. What is
still missing is everything else on the list above — camping between recoveries,
hearing an invitation while unpaired, and reporting across a reset. `report_armed`
is still a RAM flag, so a reset still silences the loop.

The gap between recoveries is the visible remainder: recovery only starts after
`RECOVER_LOST_US`, so with the reporting loop disarmed the device idles, goes
stale, and waits 30 s to notice. Camping is what closes that, not more recovery.

`Core/Src/main.c:149`, `Core/Src/cli.c` → `recover_service`, `report_service`,
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
Item 34 is confirmed with a control that closed: dropping the hub's LNA from AGC
to G6 lifts per-frame delivery 23 % → 34 %, which is 54 % → 71 % for at least one
of three opportunities. **The requirement needs 78 % per frame for 99 % in one
superframe.** A real gain, a third of the way, and this item stays blocking.

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

### 6. Neither uplink path gates on the reserved counter window — `defect`

**Fixed, pending a flash to node B.** The window now lives in `reserve_ceiling`
outside `frame_ctx`, so no console command gates it. `cmd_uplink` goes through
`tx_gate`; `report_service` asks `reserve_covers` before it seals anything and
records `tx.deny why=8` when the answer is no. The extend is a flash write that
can stall 22 ms — longer than a slot — so it runs **in the gap after the last
opportunity**, and once from `report on`, where a stall costs nothing.

`store counter` was extending the stored mark without updating the cached
ceiling, so the two disagreed by a thousand counters. It goes through
`reserve_extend` now: one door, one value, nothing to keep in step.

Both directions exercised on node A off-air: `no counter reservation - nothing
may be sealed yet`, then `counter reserved below 4006` matching what the store
reports.

**The defect was exercised on the bench while the fix sat unflashed.** Node B's
sixth unattended recovery, 2026-08-21:

    rx.miss   sf=635920   the hub leaving the air for its own flash
    rec.enter sf=635926   tier 1
    rec.park  sf=635929   grid 18, 866900000 Hz
    rec.hit   sf=639087   after 48.4 s parked, 24.1 superframes
    sync.jump sf=639087   was=635954  d=3133
    tx.up     sf=639088   three frames, immediately

`git grep reserve_covers pre-squash-2026-08-21` finds nothing and finds two uses at HEAD, so
the running board **has no gate**: it followed a 3133-superframe jump and sealed
three frames at a counter thousands past anything it had durably reserved. A
power loss there restores a lower counter and the nonce space is reused - which
is the entire reason this entry exists.

Outage to schedulable was 68.5 s, in family with the 54 s and 98 s draws; the
park is a geometric wait with a mean of 28 superframes and 24.1 is one draw.

**This is the argument for flashing sooner rather than at a convenient moment.**

`radio_devices_docs/radio/crypto/wire-crypto.md`.

### 38. A reset makes the device deaf for up to 33 minutes — `blocking` `defect`

**Measured 2026-08-21.** After a reset node B rejected every beacon the hub sent,
having heard and parsed them correctly:

    rec.deny sf=644026 grid=9 why=2      hub's claimed superframe
    rec.deny sf=644181 grid=9 why=2      still refusing, 12 minutes later

`time_start()` seeds the superframe floor from `st.counter_mark`, and
`superframe_align_at` refuses any counter below that floor - "a counter below the
durable floor would reuse a GCM nonce, so following it is refused". `report on`
had run `reserve_extend` while synced at sf 643552, pushing the mark to ~644552.
The reset then seeded the floor from it. The hub was at 644181, so **every
genuine beacon looked like a replay**. `tx.arm sf=644549` was that number and was
read at the time as a healthy restored counter.

Two separable faults:

**The floor gates the wrong question.** The durable mark answers "which counters
may this device *seal* with". It is also being used to answer "what time is it".
Aligning a clock to a beacon is timing, not crypto, so a device that cannot seal
should still sync. `reserve_covers` already gates sealing and needs a lower bound;
the align path should stop consulting the floor.

**But the separation does not fix the downtime.** Even done correctly the device
cannot seal below the reserved mark, because everything under it may have been
used. `STORE_COUNTER_STEP` is 1000, so a reset costs **up to 1000 superframes -
2000 s, 33 minutes - of no transmit**, silently, in a project whose stated
requirement is a sensor event delivered within one second (item 4).

**Not fixed on the night it was found, deliberately.** The align guard is
load-bearing against beacon replay: an attacker dragging the device backwards is
what it stops. The right change splits the two guards - beacon replay against the
last *accepted beacon* counter, nonce safety against the durable mark - and that
is a security-relevant redesign, not a 3 a.m. patch to flash and call verified.

**Measured again 2026-08-22, both nodes, and this time the cost was priced.** A
flash at 15:33:40 and 15:33:50 left both boards refusing every beacon. The hub was
at superframe 693059; node A's durable mark was 693329 and node B's 693353, so the
wait was **270 and 294 superframes - 9.0 and 9.8 minutes** before either could
accept a beacon. Node A's console said it in one line: `sync: stale - a beacon
would reuse a counter; re-pair (3 refused since the last good one; last was
693041, -382 from here)`.

Two things this instance adds. The cost is not the 33-minute worst case unless the
reserve is fresh - here the block was partly spent, so it was a third of that; the
figure to quote is `mark - hub counter`, which the console prints. And **every
flash on this bench costs that silence**, which is why a before/after across a
flash has a hole in it that is not the treatment: the hub was told to cut its
series at 693329 / 693353 rather than at the flash timestamp.

Item 5 (reboot-and-recover as one sequence) cannot pass while this stands.

`radio_devices_docs/radio/crypto/wire-crypto.md`.

### 8. `report_service` blocks the superloop while it waits — `debt`

Two spin waits per cycle hold the core until an absolute instant, so the console
is unresponsive across the beacon window and the slot, and the widened window of
a stale cycle makes that longer. Harmless while the device does one thing;
the blocker for item 1, which needs the same loop to do several.

`Core/Src/cli.c` → `report_service`.

### 21. `frame.c`'s replay rule guards a loopback, not the production path — `debt`

**The transmit half is fixed** (in `feat(link)`, `pre-squash-2026-08-21`):
`seal_claim` compares the pair `(superframe, slot)` lexicographically, mirroring
the hub at `CM4/Core/Src/radio.c:1463`. Measured before: three cycles emitting
`tx.up slot=1` then two `tx.deny why=3`. After: three frames on one channel
611 ms apart.

**The receive half is fixed, and this item had the defect in the wrong place.**
It named `Core/Src/frame.c` as this side's receive rule. That is the rule, but
`frame_open` has one caller — the `frame` console command, invoked with
`FRAME_DIR_UPLINK` and a fixed slot 7. It is a loopback instrument. The path
that receives real commands is `downlink_open` -> `downlink_apply`, and it
consulted **no floor at all**; `downlink_apply`'s `cmd_seq` match is idempotence,
not a replay guard. Closed in `fix(downlink): a replay floor on the path that
receives real commands`.

**The tuple this item prescribed is the wrong shape for this direction**, and the
reason is two facts on the hub: `downlink_service` returns early on
`dl_served == frame_counter`, so one downlink per superframe, and a downlink's
`slot` is the addressed device's granted slot rather than an opportunity index.
For one device it is a constant, so a tuple would be a mechanism with no varying
input — and would read as "this side has the k=3 floor" when this direction has
no k=3 in it.

**What is left is `frame.c` itself**, whose scalar rule is correct for the
loopback it guards but is one grep away from being read as the production rule
again. Either give it a caller that varies the slot or say on it what it guards.

Bounding the transmit side, which has not changed: three opportunities are
nonce-safe **only because the slot is in the nonce**. Carrying one slot number
for all three would be same-key, same-nonce, three plaintexts.

`Core/Src/frame.c`, `radio_devices_docs/wl55_device/security/replay.md`.

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

### 43. The live hop was sized by a test vector's constant — `closed 2026-08-22`

`hop_init_live()` passes **`HOP_VECTORS_COUNT`** as the channel count for the
hopping the radio actually uses. That is the hop_v1 fixture's number. The
contract's number is `RADIO_HOP_COUNT`, in the shared header both firmwares
compile.

Both are 28 today, so nothing disagrees and nothing will until the grid changes
size. Then this device keeps 28 while the hub takes the new value, and **every
channel diverges at once** — different cycle length, different deck, different
index into it. Not a degradation: a total loss of the link, from a constant that
was never about the air.

Same shape as the `hop` command answering with the test-vector key, one level
over: a live path parameterised by a fixture.

**Fixed 2026-08-22, and it was not one identifier — it was four sites, three of
them live.** `hop_init_live` was the one this entry named; the other two live
ones were the recovery scan's window (`2 * N - 1` superframes and its timeout)
and **the park grid a lost device chooses**, `r % HOP_VECTORS_COUNT`. That last
one is the path node A sat in all morning. Only the `hop vectors` self-test still
takes the fixture's count, which is what a fixture is for.

The class, not the instance: an entry that says "the fix is one identifier" is
a claim nobody checked, and grepping the symbol took less time than writing this
sentence. `test/test_hop.c` now carries **"hop_v1 still describes the live deck"**
— `HOP_VECTORS_COUNT == RADIO_HOP_COUNT` — so a grid that changes size fails the
suite instead of leaving a fixture quietly validating a cycle nothing transmits
on. Verified non-vacuous by setting the fixture to 27 and watching it fail.

`Core/Src/cli.c` → `hop_init_live`. `radio_devices_docs/radio/hopping.md`.

### 42. The hop channel was computed from the guess and never recomputed — `closed 2026-08-22`

`report_service` tunes before it listens, because it has to:

    sf = superframe_now(&sframe) + 1;                     /* predicted */
    hop_channel_live(sf, &hop); radio_configure(hop_to_grid(hop));
    ...
    sf = aligned;                                         /* the hub's counter */

Between those two lines the counter can change and `grid` does not. So the
channel belongs to this node's guess while the nonce, the slot timing and the
telemetry label belong to the hub's truth.

**Unreachable today, and that is the whole point of the entry.** A beacon that
fails to arrive or fails `beacon_apply` returns before any transmit, so nothing
goes out unless the guess was right — measured over 9 h 25 m: `rx.miss` 0,
`rx.beacon` 8899, `tx.up` 26694 = 8898 × 3, and zero transmitting superframes
without a beacon in the same one. The guard is real and it is doing the work.

It becomes reachable the moment a beacon can arrive while the guess is wrong: a
hop function whose period is shorter than the counter error, two superframes
sharing a channel, or a beacon accepted after a jump. **A bug made unreachable by
a guard elsewhere is still the bug**, and this one is one edit to `hop.c` away
from being live, in a file that has no reason to know it is load-bearing for
`cli.c`.

**Fixed 2026-08-22** with the second option: after `sf = aligned`, a counter that
differs from `report_attempt_sf` — the one the radio was tuned for — is refused
with `TLM_TX_DENY ... TLM_WHY_CHANNEL`, carrying both counters so the telemetry
says which way they diverged.

**The refusal has never fired, so it cannot be read in either direction yet.**
Its own entry's argument says why: the beacon guard makes it unreachable today.
A zero here is not evidence the branch works — it is evidence the guard in front
of it still holds. What would exercise it is a beacon accepted after a jump, and
node A does exactly that on every cycle it cold-starts (item 29), so the first
reading may come from A rather than from a deliberate test.

Found 2026-08-22 while answering the hub's channel-disagreement question, which
this side's data excludes as a cause.

`Core/Src/cli.c` → `report_service`. `radio_devices_docs/radio/hopping.md`.

### 34. Front-end overload confirmed, and it does not close item 4 — `hub`

**Confirmed on 2026-08-22 with a control that closed.** The hub stepped its LNA
by hand across two runs whose block orders were reverses of each other, so gain
and elapsed time pointed the same way in one and opposite ways in the other:

    gain   run 1 (gain down)  run 2 (gain up)
    AGC          10.29             10.32     arrivals/min
    G2           11.03             11.18
    G4           17.06             15.29
    G6           21.76             15.15
                                   G6-return 14.12 against opening 15.15

**AGC measured at the start of one run and near the end of the other, ninety
minutes apart, agrees to 0.3 %; G2 to 1 %.** The baseline is steady, so run 1's
apparent drift was two high blocks and not a moving reference. Run 2's monotone
fall as gain rises cannot be drift, because drift would have lifted it.

The denominator is this side's and was measured off the air rather than taken
from these counters: 45 frames a minute, flat across every block, and this
device's own transmit rate held at 224-225 per five minutes with the beacon
window flat to 6 µs in 47 810 for the whole of run 1. The precondition was on the
part all along — `RegLna = 0x08`, AGC enabled, selecting **maximum gain on every
frame at −42 dBm**, 107 of 107.

**Best estimate: +47 % delivery, 23 % to 34 % per frame.**

**And that is the number this item exists to put next to the requirement.** Item
4 needs an event delivered inside 1 s, which is one superframe, which is three
opportunities:

    per-frame 22.9 %  ->  at least one of three  54.2 %
    per-frame 33.7 %  ->  at least one of three  70.8 %

    for 99 % in one superframe, per-frame must be 78 %

**A real +17 pp on the thing that matters, and still 28 pp short of the
requirement.** The `blocking` tag moves to item 4, which owns the floor; this
item stops being a blocker and becomes a measured contribution with a mechanism.

Two things it does not explain and that are not modelled away: two of run 1's
blocks read 10 % and 30 % high at the two largest attenuations with nothing in
run 2 reproducing it, and the remaining ~66 % per-frame loss at G6.

The original A-B-C that opened this item, kept because the arms are still the
evidence that low power turned the link on at all:

### 34a. The original power experiment

Dropping this device from +14 to -17 dBm turned the link on. A-B-C on matched
counts, 36 frames a side, brackets agreed with the hub session in advance:

    A  -17  57 frames   8 syncs   8 accepted   0 CRC failures
    B  +14  36 frames   1 sync    0 accepted   1 CRC failure
    C  -17  36 frames   9 syncs   8 accepted   1 CRC failure

    accepted  C 8/36 vs B 0/36              Fisher one-sided  0.00253
    synced    C 9/36 vs B 1/36                                0.00679
    pooled    low 16/17 syncs vs high 2/20                     1.8e-07

C reproducing A on fresh channels killed the drift arm. Sync moved too, so
"sync is fine, CRC is dead" was a description of the high-power era and could
not have been contradicted by the only data that existed.

**The mechanism was confounded and is now resolved.** The experiment varied
*this device's transmit power*, which moves two things at once: the hub's input
level, and this PA's operating point. The hub's per-frame level-and-gain column,
taken during the +14 window from sf 635080, separated them:

    n  grid      afc     lvl  gain  crc
   16    26    15747   -25    G1  CRC FAIL
   20    11    11108   -26    G1  CRC FAIL
   21    11      610   -13    G6  ok

**A compressed front end under-reports the level of the signal compressing it.**
-25 dBm at G1 is 18 dB of a 31 dB step and reads as a failed instrument; -13 at
G6 is the predicted level and the frame is clean. The under-reading and the
corruption are one event, and the gain column is non-vacuous because the same
population holds G1 and G6.

**The exoneration of this PA is WITHDRAWN, 2026-08-21.** It read: *this PA is
exonerated by a positive result, not by elimination — frame 21 is +14 dBm from
here arriving at -13 dBm with a clean CRC.* The whole weight of it sits on that
one level reading being a trustworthy measurement of what left this antenna, and
the hub has since reported that the level column moves **24 dB the wrong way for
a 30 dB gain reduction**, and that the run's log carries neither the pin state
nor this device's transmit power — so it cannot be re-read and cannot exclude
this PA. One frame, one instrument, and the instrument has failed a control it
was said to have passed.

Withdrawing it is not evidence against the PA; it returns the PA to untested.
The conclusions that rested on it — no PA config per power step, the
sixty-four-device link budget not at stake — go back to open with it.

**The AFC scatter goes with it.** Five G1 failures carry 15747, -1587, 3540,
4638 and 11108 Hz; the one G6 success carries 610. The estimator was sound and
its input was distorted - the reverse of the caveat carried here for a week.

**Confirmed by intervention on the other end.** The hub pinned its LNA at G6,
bracketed at sf 635289, this device unchanged at +14 throughout:

    pinned G6, sf 635322..635457     15 accepted of 18
    free G1,   pooled over the run   28 accepted of 50
    Fisher one-sided 0.034, two-sided 0.049; before/after check p = 0.517

**Not 7 of 7.** That was the first seven frames, read out while the population was
still growing, and the hub withdrew it on its own having gone back to tag every
frame by superframe for an unrelated reason. The conclusion does not move and the
p-value improves, but the claims differ: 7/7 says pinning fixes it, 15/17 says
pinning fixes **most** of it and something else costs about 12% of frames. That
residual was invisible in the favourable prefix.

Both pinned failures carry the largest negative corrections in the set, -10865
and -10010 Hz, against accepted frames spanning -9522 to +18066. **Two points,
found right after a stopping-rule error: a thing to look at, not a finding.**

**Those numbers replace 15/17 against 2/11 at p = 3.6e-04, corrected by the hub
2026-08-21.** Both p-values are arithmetically right - recomputed here, 3.57e-04
and 0.0345 - and the entire difference is the control. 2/11 was a hand-picked
adjacent window; 28/50 is every free-AGC frame the same run recorded, with a
before/after check licensing the pooling. **A result that moves by three orders
of magnitude when the control stops being chosen was a result about the choice.**

At p = 0.034 one-sided the pin still points the same way, and it is now one
weak result rather than the decisive one this entry was built around.

Two one-sided interventions on opposite ends - this device's power, the hub's
gain - both recover acceptance, and neither touches the wire. **What does not
survive** is the sentence that followed: the pinned frames reading -12/-13 dBm
against the free-AGC -25/-26 was quoted as the level column passing its 31 dB
control outright, and the same column is what moved 24 dB the wrong way. The
compression mechanism is not disproven; it is unsupported by the instrument that
was supposed to support it, and that instrument is the hub's.

**The AFC scatter survives the pin**: accepted pinned frames carry -9522 to
+18066 Hz. Correction magnitude does not predict survival in a population where
survival is high, which is the arm the frequency story never had. Carrier error,
RX bandwidth, deviation, DAGC and `RADIO_CARRIER_ERR_HZ` are closed as causes of
frame loss here; re-opening one needs new evidence, not a re-reading of this.

**Settling time is dead, and the arm had the power to say so.** This device ran
20 cycles at an 8-byte preamble, sf 635488..635640, at unchanged +14 - doubling
the AGC's budget from 640 us to the 1280 us it had at 25 kbps, without changing
the rate:

    4 B free AGC  sf 635080..635232   60 tx, 11 delivered, 2 accepted
    8 B free AGC  sf 635488..635640   63 tx,  4 delivered, 0 accepted

    accepted  2/60 vs 0/63                         p = 0.236, no difference
    P(0 of 63 | the pinned rate, now 15/18)        9.5e-50
    smallest recovery callable at p<0.05 vs 2/60   9 of 63, 14.3%

**This one does not move.** Its primary test is like-for-like - free AGC against
free AGC, the only thing varied being the preamble - so the control correction
above does not reach it, and the power floor is computed against 2/60 rather
than against the pinned arm. The pinned rate appears only as a reference and
9.5e-50 carries the same conclusion 2.80e-59 did.

The last line is what makes this a negative rather than an absence: the arm could
have called a recovery six times smaller than the pinned one. **G1 on every
delivered frame, -25/-26 dBm on every one** - the AGC did not back off and the
level stayed compressed by the same 13 dB.

Corroborating, and quoted as adjacent rather than as a test: nine frames at 4
bytes and +14 immediately before the change, **zero syncs**.

**One column in that arm was never tested and does move.** `delivered` is 11/60
against 4/63, Fisher one-sided 0.039, two-sided 0.055 - found by sweeping this
entry's own arithmetic after the hub corrected its control, not by pre-
registration. It is post-hoc, borderline, n = 60 and 63, and points the *wrong*
way for the settling-time hypothesis, which predicts a longer preamble helps.
**It is a column to watch, not a finding**, and the reason to write it down is
that it is the *detection* column - the hub has independently found detection and
corruption moving per slot while acceptance was the only thing either of us was
testing.

**The authoritative delivery rate, both sides counting one agreed window.**
sf 647400..647694, this device at -17 dBm, three opportunities per cycle:

    slot   1   59 accepted of 151   39 %      64 detected, 42 %
    slot  66   22 accepted of 151   15 %      24 detected, 16 %
    slot 131    9 accepted of 151    6 %      15 detected, 10 %
    overall    90 accepted of 453   20 %     103 detected, 23 %

**20%, not the 31-36% carried here before.** Every earlier figure divided a
count from one side by a window the other side had bounded differently; this is
the first one where both sides agreed the superframe range before counting.
Numerator and denominator are now from one population, which is the whole
difference.

The monotonic fall across the three slots is **not** read as the offset
hypothesis confirmed. All three opportunities sit in one superframe here, so
offset-into-superframe and history-inside-the-hub's-receive-window are
confounded by construction. `report opp 2` is the pre-registered falsifier and
is unrun. CRC-per-opportunity is the hub's to report from now on: every frame
leaves this side identically, so **"never detected" and "detected and broken"
are a distinction only that end can make.**

Open, and the hub's: *why* its AGC barely engages. Across two preamble lengths,
three power conditions and a whole night the part has produced **exactly two of
its six gain steps, G1 and G6, nothing between.** A slow AGC undershoots through
the intermediate ones; a threshold either fires or does not. Three registers, all
the hub's: `RegTestDagc` continuous mode, `RegRssiThresh` gating the restart, and
whether the AGC thresholds are configured to move at all.

**A cheap test offered from here, on data the hub already holds:** this device's
three opportunities sit at +59400, +670400 and +1281400 us, so how long the hub
has been in RX before each frame differs by over a second, every cycle, by
construction. If its two free-AGC G6 frames share a slot, that points at the
restart gate. Two frames confirm nothing but can aim the next window.

**The preamble arm is testable from here without a contract change**, see item
37. `radio_devices_docs/radio/phy.md`.

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
preamble hypothesis in item 34. Side effect the hub must be told before it
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

### 36. A commanded `report_every` does not survive a device reset — `defect` `contract`

**Written, not yet exercised on air.** `RADIO_CMD_SET_RATE` now marks the rate
unsaved and `store_save_report_every` appends it from the gap after the last
opportunity, not from the downlink slot - an append can erase for 22 ms, which is
longer than a slot. The flag is the retry: a refused write stays owed to the next
cycle. An unchanged rate does not write, so a repeated command costs no record.
`join.c` already restores the stored rate at boot, so nothing was needed there.

The defect it closes, measured: the hub commanded rate 5, this device applied it
and its cycles moved from every 8 superframes to every 5; after the next flash
the cadence was 8 again with the hub still believing 5.

**CLOSED, verified on target 2026-08-21.** The hub sent `rate 2`; `rx.cmd
sf=643744 cmd=1 seq=1 rpt=0` recorded it applied, and the transmit cadence stepped
8 -> 2 in that exact superframe and held. The board was then power-cycled and
`report`, issued as the first command after boot while still disarmed and having
received nothing on the air, read:

    disarmed  every 2 superframes  seal refused 0

The grant from pairing is 8, so that 2 came from the flash store and nowhere else.
`tx.arm sf=644549 dbm=-17 opp=255 every=2` confirms it at arming through a second,
independent path.

**A window that spans a regime change fools both halves of a fraction.** The hub
measured 0.402 frames per superframe after its own ack came back and read it as
rate 8 - the window ended before 643744. Numerator and denominator both correct,
both from one population, and the population straddling the event. Its fix is an
observed arrival interval **with a horizon**; a cumulative one would have printed
the pre-command rate beside a grant of 2 for another six minutes.

`radio_devices_docs/radio/tdma.md`.

### 41. Item 35's fix injects the period estimate's noise, amplified by slot offset — `defect`

**Item 35 is closed.** `hub_us_to_local` scales every hub microsecond by
`measured_us / SUPERFRAME_US`, and the hub measured this side's arrival residual
from its own grid on 2026-08-22 as **+614 / +1240 / +790 µs** for slots 1 / 66 /
131 — flat means, and the sign is wrong for the defect, which predicted
−141 / −1596 / −3051. That is the post-fix confirmation the item owed, taken
from the other side of the antenna on current firmware with no flash.

**What replaced it is the same lever arm applied to variance.** `measured_us` is
a per-superframe measurement — `elapsed / frames` between *consecutive* beacons,
so `frames` is 1 and one timestamp's noise lands on the estimate undivided — and
the filter is `measured_us = (measured_us + per) / 2`, which is α = 0.5 and
reduces sd only by 0.577. Item 35's own recorded population of `per` (57 samples,
2003459..2006079) is sd ≈ 539 µs ≈ 270 ppm, so the estimate carries ≈ 156 ppm.

Multiplying an offset by a scale that noisy scatters the slot time in proportion
to the offset:

    slot 1     59400 us  ->  sd    9 us
    slot 66   670400 us  ->  sd  104 us
    slot 131 1281400 us  ->  sd  199 us

`RADIO_SLOT_GUARD_US` is 1400 and a frame starting at residual R ends at R + 8000
in a 9400 µs slot, so R > 1400 overruns into the next slot. **The hub measured a
1657 µs residual at slot 131** in its first eleven samples, already over.

**This is invisible from this side by construction.** `off = micros() - air -
slot_at` is measured against this device's own `slot_at`, so an error in the
belief moves the frame and leaves `off` reading 600. Measured over 8124 samples
per slot: ranges 56 / 56 / 54 µs, identical and tight, and evidence about the
scheduler's precision rather than about where the frame landed. Only the hub
measures against the grid.

**The fix is a longer baseline, not a smaller α.** Holding a reference beacon N
superframes back makes `per = (now - ref_us) / (counter - ref_counter)` and
divides the timestamp noise by N. The hub's period is a constant; only this
clock's rate drifts, and it drifts over minutes, so a long baseline costs two
fields and buys the accuracy directly. A heavier filter lengthens the response to
a real rate change as well, which a longer baseline does not.

**Pre-registration held.** The hub's sampler at n = 23 measured sd **13 / 296 /
412 µs** against predicted 9 / 104 / 199. sd scales with slot offset rather than
being flat, which was the stated falsifier; slot 1 landed within 4 µs of a
prediction made from a population measured months ago for another purpose. The
far slots run ~2× the prediction, which is the fat tail also stated in advance —
one residual of 1657 µs in n = 7 is 4 sd at a Gaussian 200 µs and should appear
once in 30 000.

**Fixed in source, unconfirmed on air.** `SUPERFRAME_PERIOD_BASELINE` is 64: the
period is measured across a span rather than between consecutive beacons, so one
timestamp's noise divides by the superframes it covers. `test_period` measures
the estimate's worst error under ±600 µs of injected timestamp jitter and it is
graded rather than pass/fail — baseline 1 gives 1166 µs, 8 gives 128, 32 gives
30, 64 gives 18. Predicted post-fix scatter: **0.2 / 2.0 / 3.8 µs**, from 9 / 104
/ 199.

**This explains the gradient and not the floor**, and the hub is right to insist
on the distinction: slot 1 delivers ~41 % while carrying 13 µs of scatter against
a 1400 µs guard, so something roughly slot-independent is losing ~59 % of frames
and this variance rides on top of it. Recording item 41 as the cause of
67 / 38 / 27 would be wrong. The floor is item 4's, and it is not timing.

The flash is coupled to the hub's: `RADIO_LINK_VERSION` went 4 → 5, both sides
refuse on it before the tag, so node B cannot carry this fix until the hub moves
too. When it does, the treatment has a predicted direction and the hub cuts its
series at the superframe of the flash.

`Core/Src/superframe.c`, `Core/Src/cli.c` → `hub_us_to_local`.
`radio_devices_docs/wl55_device/radio/timebase.md`.

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

`Core/Src/cli.c` → `downlink_open`.

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

`Core/Inc/superframe.h` → `SUPERFRAME_FRESH_US`, `Core/Src/cli.c` →
`report_service`, `radio_devices_docs/wl55_device/testing/telemetry.md`.

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

`Core/Src/cli.c` → `recover_service`, `recover_search`,
`radio_devices_docs/wl55_device/testing/telemetry.md`.

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

`Core/Src/cli.c` → `report_service`, `Core/Src/radio.c:559`.


---

### 46. `rssi_down` is only ever measured by a typed command — `defect`

`beacon_rssi_valid` is set at three sites, and all three are console commands:
`cmd_time` twice and `cmd_downlink` once. It is never cleared, so it latches —
but on a device nobody types at, it never latches at all. `report_service`
receives the beacon into `info`, emits `TLM_RX_BEACON` from it, and does not
store `info.rssi_dbm`; five lines later it reports
`rep.rssi_down = beacon_rssi_valid ? beacon_rssi_dbm : 0`.

So an autonomous device reports **a literal zero, for ever**, while hearing a
beacon every cycle with the level in hand. Confirmed on air 2026-08-22: both
nodes read `report_flags=3`, labels `rssi_stale` and `supply_stale`, and
`rssi_down_dbm` 0 on the hub's API, through a whole day of accepted beacons.

`RADIO_REPORT_FLAG_RSSI_STALE` is documented as "rssi_down is a last value, not
a sentinel". That is false in this state: it is not a stale reading, it is a
reading that was never taken. The hub reasonably reads the flag's contract and
serves the zero rather than hiding it, which is how a never-measured number
reaches an API as data. **Hiding it on the hub would paper over this**; the fix
is on this side, and is one assignment in the path that already has the value.

This is item 1's shape again — a value that exists only if an operator typed
something — and it is the term the downlink budget would be sized from.

**Fixed in source 2026-08-22, unflashed.** The pointed-at fix was one path; the
sweep found **six** that accept a beacon and **three** that recorded nothing —
`report_service`, `recover_service`, and the sweep inside `cmd_time`. All six now
go through one `beacon_rssi_note`, so a new beacon path cannot forget. Recovery
already carried the level into its `rec.hit` record and dropped it afterwards.
Flashing waited on the hub's item 45 measurement, because this tree also carries
item 41, which moves the arrival scatter the hub is measuring.

`Core/Src/cli.c` → `beacon_rssi_note`, `report_service`, `recover_take`.
`radio_devices_docs/radio/tdma.md`.


### 48. The firmware cannot say which build it is — `closed 2026-08-22`

`boot` carries `up`, `reset`, `kbps`; `status` prints uptime, clock and reset
cause; nothing anywhere names the build. So "what is on this board" is not
answerable from the board, and the only way to know is to remember flashing it.

**It blocked a decision on 2026-08-22.** Item 46's fix was built and could not
be taken to the air, because flashing might have delivered the whole tail since
the last flash — including item 41's `SUPERFRAME_PERIOD_BASELINE`, which moves
the arrival scatter the hub was measuring in the same window. The question was
not answerable and the flash had to be ordered around it instead.

`RADIO_LINK_VERSION` narrows it and does not close it: the link refuses on a
mismatch, so a live link proves both sides compile the same **contract**. A
contract is not a build, and every fix that changes no wire byte is invisible to
it — which is most of them.

`git describe --always --dirty` at configure time, into a generated header: the
string on `status`, and the abbreviated commit as an integer field on `boot`, so
a stream says which build produced it and two boards' logs stay joinable across
a reflash. The telemetry format takes integers only, so the string belongs on
the console and the number in the record.

The hub has the same hole and the same cause — its `hello` carries `fw` as a
name, not a version — and tracks it as its own item 47. Two items, one shape;
neither inherits the other's fix.

**Closed on air 2026-08-22.** `cmake/build_id.cmake` runs at build
time rather than at configure time, because a configure-time value goes stale on
the next commit without anything saying so; it rewrites the header only on a
change, so nothing rebuilds without cause. `status` prints `git describe
--always --dirty`, and `boot` carries the abbreviated commit with bit 31 set for
a dirty tree — the packed form the `sync` field already uses. Verified against a
clean configure, and non-vacuous: the same generator pointed at the hub's tree
returns that tree's id, so it reads git rather than emitting a constant.

Confirmed from the boards rather than from the build: after the 15:33 flash both
answer `build    4e04682 (record 81806978)`, which is HEAD, and the dirty bit is
clear because the tree was committed before the build. The question "what is on
this board" is now answered by the board.

`cmake/build_id.cmake`, `CMakeLists.txt`, `Core/Src/telemetry.c`,
`Core/Src/cli.c` → `cmd_status`.
`radio_devices_docs/wl55_device/testing/telemetry.md`.


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

---

## Bench debts

### 18. Every air number here came from one node — `closed 2026-08-22`

Node B had done all the transmitting, so nothing on this bench distinguished a
property of the protocol from a property of one board.

**Node A transmitted for the first time on 2026-08-22**, on slot 0, in the same
superframes as node B — TDMA separates them, so one window measures both. The
first thing two nodes bought was a cross-check no single node could give: the
hub hears A at -72 dBm and B at -43, and the devices hear the hub at -46 and -15.
**Two receivers at opposite ends of the link agree on the 30 dB split to within
2 dB**, neither calibrated against the other. What was left of this entry is the
grant asymmetry, which is item 29's.

**Node A is not the blank board this entry assumed.** `join show` on it reads
`dev C4D444AA, paired: slot 0, report every 8, key gen 2` — restored from its
store at boot. So pairing has run on two devices, not one, and node A's session
is generation 2 against node B's.

Two consequences. Whether the hub still holds `C4D444AA` is unknown from here
and decides whether node A is a live second device or the first real instance of
the stale-session case `RADIO_CMD_REJOIN` exists for — untested either way.

And **flashing a test build to the idle board does not make it inert**: it boots
paired, and one `report on` puts it on the air on slot 0. That happened during
this cleanup and was caught by reading the arm message. Off-air validation on
node A must leave it disarmed.

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
