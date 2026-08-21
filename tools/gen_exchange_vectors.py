#!/usr/bin/env python3
"""Host reference for the pairing exchange, written from the hub session's spec
text alone - not from its generator, and not by calling any firmware.

That independence is the point. A checker checks arithmetic; a second
implementation checks the specification. The hop sequence found three divergence
points and one wrong documented property exactly because it was reimplemented
from the description rather than verified against the other side's output.

Self-validating: the same HKDF that produces the new values first has to
reproduce wire_v3's session and hop keys, which are already agreed. A generator
that cannot hit a known answer is not evidence about an unknown one."""
import hashlib
import hmac

# --- from wire_v3, already agreed by both sides ---
HUB_ID = 0x33442211
DEV_ID = 0x0000002A
SHARED_X = bytes.fromhex("39eec3e897d3c11e42681481f592e733eb699f8d54f8917e82222883dcfbd73b")
DEV_PUB_COMPRESSED = bytes.fromhex("028f54a4a6e161c89987304f89fc0d8f59387924c2cfa959699650857a33d219bf")
HUB_PUB_COMPRESSED = bytes.fromhex("036dd84cda7f25d7e0f61097a62565bb30950425cbcd0f93a26fd1a26e93915920")
KEY_SESSION_GEN0 = bytes.fromhex("060e9f74f3aff28470483e4f7d6562f8")
KEY_HOP_GEN0 = bytes.fromhex("0b9d2943fe2fa389beae0257367d9008")


def hkdf(salt, ikm, info, length):
    """RFC 5869. `info` is the string's bytes with no NUL terminator - the
    firmware passes sizeof(literal) - 1, and an off-by-one here changes every
    key while still producing something that looks like a key."""
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, block, counter = b"", b"", 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        out += block
        counter += 1
    return out[:length]


def be32(v):
    return v.to_bytes(4, "big")


def main():
    # Salt is hub then dev, big-endian, as wire_v3 pins it.
    salt = be32(HUB_ID) + be32(DEV_ID)
    assert salt.hex() == "334422110000002a", "salt does not match wire_v3"

    # Self-check before trusting anything new.
    sess = hkdf(salt, SHARED_X, b"openhub/v1/session", 16)
    hop = hkdf(salt, SHARED_X, b"openhub/v1/hop", 16)
    assert sess == KEY_SESSION_GEN0, f"session key mismatch: {sess.hex()}"
    assert hop == KEY_HOP_GEN0, f"hop key mismatch: {hop.hex()}"
    print("self-check: HKDF reproduces wire_v3 session and hop keys")
    print()

    # 2. Fingerprint: SHA-256 over the 33 compressed bytes that travel.
    fp = hashlib.sha256(DEV_PUB_COMPRESSED).digest()
    print(f"fingerprint            = {fp.hex()}")
    print(f"fingerprint_displayed  = {fp[:6].hex().upper()}")
    print()

    # 3. Transcript: four named byte strings, dev id first.
    # Ids in the same order as the HKDF salt. The salt is pinned in wire_v3 and
    # cannot move, so the transcript is the side that yields - two orderings of
    # the same two fields in one exchange is an invitation to a confirmation
    # mismatch with no diagnosable cause.
    t_items = [
        ("t_hub_id_be", be32(HUB_ID)),
        ("t_dev_id_be", be32(DEV_ID)),
        ("t_dev_pub_compressed", DEV_PUB_COMPRESSED),
        ("t_hub_pub_compressed", HUB_PUB_COMPRESSED),
    ]
    for name, b in t_items:
        print(f"{name:22s} = {b.hex()}")
    T = b"".join(b for _, b in t_items)
    assert len(T) == 74, f"transcript is {len(T)} bytes, spec says 74"
    print(f"{'transcript':22s} = {T.hex()}")
    print(f"{'transcript_len':22s} = {len(T)}")
    print(f"{'transcript_sha256':22s} = {hashlib.sha256(T).hexdigest()}")
    print()

    # 4. Confirmations. HMAC is over T itself, per the spec's formula - not over
    # SHA-256(T), which the phrase "transcript hash" elsewhere could be read as.
    k_hub = hkdf(salt, SHARED_X, b"openhub/v1/confirm/hub", 32)
    k_dev = hkdf(salt, SHARED_X, b"openhub/v1/confirm/dev", 32)
    print(f"k_confirm_hub          = {k_hub.hex()}")
    print(f"k_confirm_dev          = {k_dev.hex()}")
    hub_c = hmac.new(k_hub, T, hashlib.sha256).digest()[:16]
    dev_c = hmac.new(k_dev, T, hashlib.sha256).digest()[:16]
    print(f"hub_confirm            = {hub_c.hex()}")
    print(f"dev_confirm            = {dev_c.hex()}")
    assert hub_c != dev_c, "separate info strings must give different values"
    print()
    print(f"digest = {hashlib.sha256((fp + T + k_hub + k_dev + hub_c + dev_c)).hexdigest()[:16]}")


if __name__ == "__main__":
    main()
