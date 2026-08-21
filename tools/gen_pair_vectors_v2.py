#!/usr/bin/env python3
"""Host reference for the option-4 pairing exchange, written from the hub
session's revised spec text alone.

Needs real P-256 arithmetic this time, so the curve code proves itself against
two values wire_v3 already pins before it computes anything new:
  1. hub_private * G must reproduce hub_public;
  2. ECDH(hub_private, dev_public) must reproduce ecdh_shared_x_only.
A curve implementation that cannot hit a known point is not evidence about an
unknown one, and a subtly wrong one still produces something that looks like a
key."""
import hashlib
import hmac

P  = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
A  = P - 3
B  = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
GX = 0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296
GY = 0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5

HUB_PRIV = int("1f2e3d4c5b6a798897a6b5c4d3e2f10012233445566778899aabbccddeeff001", 16)
DEV_PRIV = int("0a1b2c3d4e5f60718293a4b5c6d7e8f900112233445566778899aabbccddeeff", 16)
HUB_EPH_PRIV = int("4b3a291807f6e5d4c3b2a1907e6d5c4b3a2918070605040302010f0e0d0c0b0a", 16)

HUB_PUB_X = int("6dd84cda7f25d7e0f61097a62565bb30950425cbcd0f93a26fd1a26e93915920", 16)
DEV_PUB_X = int("8f54a4a6e161c89987304f89fc0d8f59387924c2cfa959699650857a33d219bf", 16)
DEV_PUB_Y = int("bba60951a78761d3eacad95ae1383faceb081f2a75e4b530b489dfde08961906", 16)
ECDH_SHARED_X = "39eec3e897d3c11e42681481f592e733eb699f8d54f8917e82222883dcfbd73b"
HUB_ID, DEV_ID = 0x33442211, 0x0000002A


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
    hub_pub = mul(HUB_PRIV, G)
    dev_pub = mul(DEV_PRIV, G)
    assert hub_pub[0] == HUB_PUB_X, "curve code does not reproduce hub_public"
    assert (dev_pub[0], dev_pub[1]) == (DEV_PUB_X, DEV_PUB_Y), "dev_public mismatch"
    z1 = mul(HUB_PRIV, dev_pub)[0].to_bytes(32, "big")
    assert z1.hex() == ECDH_SHARED_X, "ECDH does not reproduce wire_v3"
    print("selfcheck curve   hub_public + ecdh_shared_x reproduce wire_v3  ok")

    salt = HUB_ID.to_bytes(4, "big") + DEV_ID.to_bytes(4, "big")
    assert hkdf(salt, z1, b"openhub/v1/session", 16).hex() == "060e9f74f3aff28470483e4f7d6562f8"
    assert hkdf(salt, z1, b"openhub/v1/hop", 16).hex() == "0b9d2943fe2fa389beae0257367d9008"
    print("selfcheck hkdf    single-term session + hop reproduce wire_v3   ok")
    print()

    hub_eph = mul(HUB_EPH_PRIV, G)
    z2 = mul(HUB_EPH_PRIV, dev_pub)[0].to_bytes(32, "big")
    Z = z1 + z2
    assert z1 != z2, "ephemeral must not equal static"

    hub_static_c, hub_eph_c, dev_static_c = compress(hub_pub), compress(hub_eph), compress(dev_pub)
    # Hub before device, everywhere.
    T = (HUB_ID.to_bytes(4, "big") + DEV_ID.to_bytes(4, "big")
         + hub_static_c + hub_eph_c + dev_static_c)
    assert len(T) == 107, f"transcript is {len(T)}, spec says 107"

    k_hub = hkdf(salt, Z, b"openhub/v1/confirm/hub", 32)
    k_dev = hkdf(salt, Z, b"openhub/v1/confirm/dev", 32)
    out = [
        ("pair_hub_eph_public_compressed", hub_eph_c.hex()),
        ("pair_z1", z1.hex()), ("pair_z2", z2.hex()), ("pair_z", Z.hex()),
        ("pair_key_session", hkdf(salt, Z, b"openhub/v1/session", 16).hex()),
        ("pair_key_hop", hkdf(salt, Z, b"openhub/v1/hop", 16).hex()),
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
