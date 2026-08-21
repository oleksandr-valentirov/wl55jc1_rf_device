#!/usr/bin/env python3
"""Host reference for the pair_v2 exchange, written from the hub session's spec
text alone and not from its generator.

v2 adds the device's own freshness. In v1 every input was fixed by the two
identities plus hub_eph, so a recorded PAIR_RSP re-derived the same keys
forever; binding the request's superframe and an 8-byte device nonce into both
the salt and the transcript is what removes that.

The curve code proves itself against two values wire_v3 already pins before it
computes anything new. A subtly wrong implementation still produces something
that looks like a key."""
import hashlib
import hmac

P  = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
A  = P - 3
GX = 0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296
GY = 0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5

HUB_PRIV = int("1f2e3d4c5b6a798897a6b5c4d3e2f10012233445566778899aabbccddeeff001", 16)
DEV_PRIV = int("0a1b2c3d4e5f60718293a4b5c6d7e8f900112233445566778899aabbccddeeff", 16)
HUB_EPH_PRIV = int("4b3a291807f6e5d4c3b2a1907e6d5c4b3a2918070605040302010f0e0d0c0b0a", 16)

HUB_PUB_X = int("6dd84cda7f25d7e0f61097a62565bb30950425cbcd0f93a26fd1a26e93915920", 16)
ECDH_SHARED_X = "39eec3e897d3c11e42681481f592e733eb699f8d54f8917e82222883dcfbd73b"
HUB_ID, DEV_ID = 0x33442211, 0x0000002A

# v2 inputs. Proposed by the device side; both generators must use the same
# values or the diff compares nothing.
REQ_SUPERFRAME = 123456
DEV_NONCE = bytes.fromhex("a1b2c3d4e5f60718")

# v1 values, so a v2 run also proves it did not silently reproduce v1.
V1_SESSION = "0db428d17a5354013d744e5d8ada5e8b"
V1_TRANSCRIPT_LEN = 107


def add(p1, p2):
    if p1 is None: return p2
    if p2 is None: return p1
    (x1, y1), (x2, y2) = p1, p2
    if x1 == x2 and (y1 + y2) % P == 0: return None
    if p1 == p2:
        lam = (3 * x1 * x1 + A) * pow(2 * y1, P - 2, P) % P
    else:
        lam = (y2 - y1) * pow(x2 - x1, P - 2, P) % P
    x3 = (lam * lam - x1 - x2) % P
    return (x3, (lam * (x1 - x3) - y1) % P)


def mul(k, pt):
    r = None
    while k:
        if k & 1: r = add(r, pt)
        pt = add(pt, pt); k >>= 1
    return r


def compress(pt):
    x, y = pt
    return bytes([0x02 | (y & 1)]) + x.to_bytes(32, "big")


def hkdf(salt, ikm, info, length):
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, block, ctr = b"", b"", 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([ctr]), hashlib.sha256).digest()
        out += block; ctr += 1
    return out[:length]


def main():
    G = (GX, GY)
    hub_pub, dev_pub = mul(HUB_PRIV, G), mul(DEV_PRIV, G)
    assert hub_pub[0] == HUB_PUB_X, "curve code does not reproduce hub_public"
    z1 = mul(HUB_PRIV, dev_pub)[0].to_bytes(32, "big")
    assert z1.hex() == ECDH_SHARED_X, "ECDH does not reproduce wire_v3"
    print("selfcheck curve   hub_public + ecdh_shared_x reproduce wire_v3   ok")

    hub_eph = mul(HUB_EPH_PRIV, G)
    z2 = mul(HUB_EPH_PRIV, dev_pub)[0].to_bytes(32, "big")
    assert z1 != z2, "ephemeral must not equal static"
    Z = z1 + z2

    be4 = lambda v: v.to_bytes(4, "big")
    salt = be4(HUB_ID) + be4(DEV_ID) + be4(REQ_SUPERFRAME) + DEV_NONCE
    assert len(salt) == 20, f"salt is {len(salt)}, spec says 20"

    hub_static_c, hub_eph_c, dev_static_c = (compress(hub_pub), compress(hub_eph),
                                             compress(dev_pub))
    # Hub before device, everywhere; the two freshness fields sit with the ids
    # they qualify, ahead of any key.
    T = (be4(HUB_ID) + be4(DEV_ID) + be4(REQ_SUPERFRAME) + DEV_NONCE
         + hub_static_c + hub_eph_c + dev_static_c)
    assert len(T) == 119, f"transcript is {len(T)}, spec says 119"
    assert len(T) != V1_TRANSCRIPT_LEN

    k_session = hkdf(salt, Z, b"openhub/v1/session", 16)
    # The v1 salt was 8 bytes; a v2 run reproducing the v1 session key would
    # mean the new inputs never reached the KDF, which is the whole point.
    assert k_session.hex() != V1_SESSION, "v2 salt did not change the session key"
    print("selfcheck binding session key differs from pair_v1              ok")
    print()

    k_hub = hkdf(salt, Z, b"openhub/v1/confirm/hub", 32)
    k_dev = hkdf(salt, Z, b"openhub/v1/confirm/dev", 32)
    # No hop key: it is a network key delivered in PAIR_ACCEPT, so deriving one
    # here would publish a vector with no consumer - which is what pair_v1's
    # pair_key_hop turned out to be.
    out = [
        ("pair_req_superframe", f"{REQ_SUPERFRAME:08x}"),
        ("pair_dev_nonce", DEV_NONCE.hex()),
        ("pair_hub_eph_public_compressed", hub_eph_c.hex()),
        ("pair_z1", z1.hex()), ("pair_z2", z2.hex()), ("pair_z", Z.hex()),
        ("pair_salt_len", str(len(salt))), ("pair_salt", salt.hex()),
        ("pair_key_session", k_session.hex()),
        ("pair_confirm_key_hub", k_hub.hex()), ("pair_confirm_key_dev", k_dev.hex()),
        ("pair_transcript_len", str(len(T))), ("pair_transcript", T.hex()),
        ("pair_transcript_sha256", hashlib.sha256(T).hexdigest()),
        ("pair_confirm_hub", hmac.new(k_hub, T, hashlib.sha256).digest()[:16].hex()),
        ("pair_confirm_dev", hmac.new(k_dev, T, hashlib.sha256).digest()[:16].hex()),
    ]
    for k, v in out:
        print(f"{k:32s} = {v}")


if __name__ == "__main__":
    main()
