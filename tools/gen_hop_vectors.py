#!/usr/bin/env python3
"""Generate known-answer vectors for the hop sequence.

The hop sequence is a wire contract exactly like the frame format: both ends
must land on the same channel or they never hear each other. It has no vectors
today, and the hub's own unit tests inject a fake PRF, so nothing anywhere
checks the keyed byte-level sequence that the radio actually uses.

That matters because a disagreement here has no symptom. A frame on the wrong
channel is not a decode error, it is silence - indistinguishable from a dead
radio, a bad antenna or a device that never woke up.

These come from a host library, never from either MCU's HAL, for the same
reason the crypto vectors do: two implementations agreeing with each other
proves only that they were written by the same person."""
import hashlib
import textwrap

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

# The hop key from wire_v3 - the one HKDF derives with info "openhub/v1/hop".
# Using the published key ties this set to that one rather than inventing a
# second secret that nothing else references.
HOP_KEY = bytes.fromhex("0b9d2943fe2fa389beae0257367d9008")

GRID_COUNT = 29
JOIN_SLOT = 14
HOP_COUNT = GRID_COUNT - 1

CH_BASE_HZ = 865_100_000
CH_SPACING_HZ = 100_000

# Superframes worth pinning: the first cycle, the cycle boundary, a far jump a
# sleeping node would have to land on directly, and a wrap-adjacent value.
SUPERFRAMES = [0, 1, 27, 28, 29, 55, 56, 1000, 100_000, 0xFFFFFFFF]


def aes_ecb(block):
    e = Cipher(algorithms.AES(HOP_KEY), modes.ECB()).encryptor()
    return e.update(block) + e.finalize()


def swap32(b):
    """What a byte buffer becomes when a little-endian core hands it to CRYP as
    words with DATATYPE_32B: every group of four bytes reverses."""
    return b"".join(b[i:i + 4][::-1] for i in range(0, len(b), 4))


def deck(cycle, count, cycle_big_endian=True, word_swap=False):
    """The hub's Fisher-Yates, reimplemented from hop.c rather than called, so
    agreement means the algorithm is right and not that the code is shared."""
    block = bytearray(16)
    block[0:4] = cycle.to_bytes(4, "big" if cycle_big_endian else "little")

    def prf(b):
        if word_swap:
            return swap32(aes_ecb(swap32(bytes(b))))
        return aes_ecb(bytes(b))

    stream = bytearray(prf(block))
    block[15] = 1
    stream += prf(block)

    d = list(range(count))
    for i in range(count - 1, 0, -1):
        j = stream[i & 31] % (i + 1)
        d[i], d[j] = d[j], d[i]
    return d


def channel(superframe, **kw):
    return deck(superframe // HOP_COUNT, HOP_COUNT, **kw)[superframe % HOP_COUNT]


def to_grid(hop_index):
    """The hop set and the join channel are disjoint by construction, not by
    probability: the reserved slot is skipped rather than avoided."""
    return hop_index if hop_index < JOIN_SLOT else hop_index + 1


def slot_hz(slot):
    return CH_BASE_HZ + (slot % GRID_COUNT) * CH_SPACING_HZ


def check_properties():
    """The properties the sequence exists for. Checked here as well as on the
    hub because a vector set that pins a broken sequence pins it forever."""
    cycles = 400
    seen = [0] * HOP_COUNT
    prev = None
    boundary_repeats = 0
    for sf in range(HOP_COUNT * cycles):
        ch = channel(sf)
        seen[ch] += 1
        if prev == ch:
            # Fisher-Yates per cycle guarantees no repeat *within* a cycle. It
            # guarantees nothing across a boundary: the last entry of one deck
            # equals the first of the next with probability 1/count. Measured
            # rather than asserted, because asserting zero would be asserting
            # something the design does not provide.
            assert sf % HOP_COUNT == 0, f"repeat inside a cycle at superframe {sf}"
            boundary_repeats += 1
        prev = ch
    assert set(seen) == {cycles}, f"occupancy not flat: {sorted(set(seen))}"
    print(f"{cycles} cycles: occupancy {cycles} exactly, "
          f"{boundary_repeats} repeats at cycle boundaries "
          f"(expected ~{cycles / HOP_COUNT:.1f}), "
          f"{100.0 * boundary_repeats / (HOP_COUNT * cycles):.3f}% of hops")
    assert to_grid(JOIN_SLOT - 1) == JOIN_SLOT - 1
    assert to_grid(JOIN_SLOT) == JOIN_SLOT + 1
    assert max(to_grid(i) for i in range(HOP_COUNT)) < GRID_COUNT
    assert JOIN_SLOT not in {to_grid(i) for i in range(HOP_COUNT)}


# The digest covers the *values*, not the file. Hashing the emitted text made a
# reworded comment move it, which is the worst of both: a set that cannot have
# changed reports that it did, everyone who pinned the old constant sees a false
# alarm, and a diff that keeps finding nothing is a diff people stop running.
EMITTED = []


def pin(name, values):
    EMITTED.append((name, list(values)))
    return values


def carr(name, data):
    pin(name, data)
    body = ", ".join(f"0x{b:02x}" for b in data)
    return (f"static const uint8_t {name}[{len(data)}] = {{\n" +
            textwrap.indent("\n".join(textwrap.wrap(body, 72)), "    ") + "\n};\n")


def value_digest():
    canon = "\n".join(f"{n}=" + ",".join(str(v) for v in vals)
                       for n, vals in sorted(EMITTED))
    return hashlib.sha256(canon.encode()).hexdigest()[:16]


# Cycle 1's first PRF input: a non-zero counter, so a byte order or width error
# cannot hide in a block of zeroes the way cycle 0's would.
PRF_IN = bytearray(16)
PRF_IN[0:4] = (1).to_bytes(4, "big")

FIPS_KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
FIPS_IN = bytes.fromhex("00112233445566778899aabbccddeeff")
FIPS_OUT = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")


def main():
    # A regression guard, not an anchor: this value was transcribed from the
    # hub session's message and both generators call the same OpenSSL, so the
    # circle is closed by the two CRYP peripherals reproducing it, not here.
    e = Cipher(algorithms.AES(FIPS_KEY), modes.ECB()).encryptor()
    assert e.update(FIPS_IN) + e.finalize() == FIPS_OUT, "host AES fails FIPS-197 C.1"
    print("selfcheck host AES reproduces FIPS-197 C.1  ok")
    check_properties()

    spec0 = deck(0, HOP_COUNT)
    spec1 = deck(1, HOP_COUNT)
    # Cycle 0 is the same under both endian conventions because the counter
    # bytes are all zero. A first-contact test that runs inside the hub's first
    # 56 seconds therefore passes under a convention mismatch and diverges
    # afterwards, which is the worst possible place for the difference to hide.
    assert deck(0, HOP_COUNT, cycle_big_endian=False) == spec0
    little1 = deck(1, HOP_COUNT, cycle_big_endian=False)
    assert little1 != spec1
    swapped1 = deck(1, HOP_COUNT, word_swap=True, cycle_big_endian=False)

    channels = [channel(sf) for sf in SUPERFRAMES]
    grid = [to_grid(c) for c in channels]

    lines = [
        "/* Generated by tools/gen_hop_vectors.py - do not edit. */",
        "/* The hop sequence is a wire contract: disagreement is silence. */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define HOP_VECTORS_COUNT   {HOP_COUNT}",
        f"#define HOP_VECTORS_GRID    {GRID_COUNT}",
        f"#define HOP_VECTORS_JOIN    {JOIN_SLOT}",
        f"#define HOP_VECTORS_SAMPLES {len(SUPERFRAMES)}",
        "",
        # The primitive itself, against an outside AES. The deck vectors catch
        # a wrong PRF too - a wrong block gives a different permutation, not an
        # invalid one - but they cannot say whether the PRF or the shuffle was
        # wrong, and they only run where a hop test runs. A pinned block is the
        # answer to "is this accelerator computing AES at all".
        # FIPS-197 appendix C.1, which comes from neither implementation. It is
        # the only block here that a shared mistake could not have produced.
        carr("vec_aes_fips_key", FIPS_KEY),
        carr("vec_aes_fips_in", FIPS_IN),
        carr("vec_aes_fips_out", FIPS_OUT),
        carr("vec_hop_prf_in", bytes(PRF_IN)),
        carr("vec_hop_prf_out", aes_ecb(bytes(PRF_IN))),
        carr("vec_hop_key", HOP_KEY),
        carr("vec_hop_deck_cycle0", spec0),
        carr("vec_hop_deck_cycle1", spec1),
        "/* The same cycle under the two conventions this could plausibly have",
        " * been written in. Kept so a device that hears the hub on an",
        " * unexpected channel can name which one the other end is using",
        " * instead of leaving it to be guessed. */",
        carr("vec_hop_deck_cycle1_le_counter", little1),
        carr("vec_hop_deck_cycle1_cryp_words", swapped1),
        "",
        "static const uint32_t vec_hop_superframe[HOP_VECTORS_SAMPLES] = {",
        "    " + ", ".join(f"{sf}u" for sf in pin("vec_hop_superframe", SUPERFRAMES)),
        "};",
        "",
        carr("vec_hop_channel", channels),
        carr("vec_hop_grid_slot", grid),
        "",
        "static const uint32_t vec_hop_hz[HOP_VECTORS_SAMPLES] = {",
        "    " + ", ".join(f"{hz}u" for hz in pin("vec_hop_hz", [slot_hz(s) for s in grid])),
        "};",
    ]
    text = "\n".join(lines) + "\n"
    pin("_geometry", [HOP_COUNT, GRID_COUNT, JOIN_SLOT, len(SUPERFRAMES)])
    digest = value_digest()
    text = text.replace("#include <stdint.h>\n",
                        f"#include <stdint.h>\n\n#define HOP_VECTORS_DIGEST  \"{digest}\"\n")

    with open("Core/Inc/hop_vectors.h", "w") as f:
        f.write(text)

    print(f"digest {digest}")
    print(f"cycle 0 deck: {spec0}")
    print(f"cycle 1 deck: {spec1}")
    print(f"cycle 1, little-endian counter: {little1}")
    print(f"cycle 1, CRYP 32-bit datatype:  {swapped1}")
    print("positions matching the spec deck: "
          f"le={sum(a == b for a, b in zip(spec1, little1))}/{HOP_COUNT} "
          f"cryp={sum(a == b for a, b in zip(spec1, swapped1))}/{HOP_COUNT}")
    for sf, ch, g in zip(SUPERFRAMES, channels, grid):
        print(f"  superframe {sf:>10} -> hop {ch:>2} -> grid {g:>2} -> {slot_hz(g)} Hz")


if __name__ == "__main__":
    main()
