#!/usr/bin/env python3
"""Generate the known-answer vectors the device checks its hardware against.

The point is independence: these come from a host library and the public NIST
GCM test set, never from the STM32 HAL, so a passing test on the target proves
the silicon agrees with the specification rather than with itself."""
import textwrap

from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

# GCM case 4: the one with AAD and a payload that is not a whole number of blocks.
# radio_devices_docs/wl55_device/testing/host-tests.md
GCM_KEY = bytes.fromhex("feffe9928665731c6d6a8f9467308308")
GCM_IV = bytes.fromhex("cafebabefacedbaddecaf888")
GCM_AAD = bytes.fromhex("feedfacedeadbeeffeedfacedeadbeefabaddad2")
GCM_PT = bytes.fromhex(
    "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
    "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39")
GCM_CT = bytes.fromhex(
    "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
    "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091")
GCM_TAG = bytes.fromhex("5bc94fbc3221a5db94fae95ae7121a47")

# A fixed P-256 scalar: it only has to be constant across target and host.
# radio_devices_docs/wl55_device/testing/host-tests.md
P256_D = 0x519b423d715f8b581f4fa8ee59f4771a5b44c8130b4e3eacca54a56dda72b464

# NIST P-256 parameters, big-endian as both the PKA and SEC1 want them.
# radio_devices_docs/wl55_device/testing/host-tests.md
P256_P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
P256_B = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
P256_GX = 0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296
P256_GY = 0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5
P256_N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
# p = 3 mod 4: y = +/- t^((p+1)/4) mod p, one exponentiation, not Tonelli-Shanks.
# radio_devices_docs/wl55_device/security/self-tests.md
P256_SQRT_EXP = (P256_P + 1) // 4


def check_gcm():
    got = AESGCM(GCM_KEY).encrypt(GCM_IV, GCM_PT, GCM_AAD)
    assert got == GCM_CT + GCM_TAG, "GCM vector is wrong"


def on_curve(x, y):
    return (y * y - (x * x * x - 3 * x + P256_B)) % P256_P == 0


def scalar_mul(k, x, y):
    """Plain double-and-add. Not constant time, and does not need to be - it
    runs on the host to cross-check the constants the firmware will use."""
    rx, ry = None, None
    while k:
        if k & 1:
            rx, ry = point_add(rx, ry, x, y)
        x, y = point_add(x, y, x, y)
        k >>= 1
    return rx, ry


def point_add(x1, y1, x2, y2):
    if x1 is None:
        return x2, y2
    if x2 is None:
        return x1, y1
    if x1 == x2 and (y1 + y2) % P256_P == 0:
        return None, None
    if x1 == x2:
        lam = (3 * x1 * x1 - 3) * pow(2 * y1, -1, P256_P)
    else:
        lam = (y2 - y1) * pow(x2 - x1, -1, P256_P)
    x3 = (lam * lam - x1 - x2) % P256_P
    return x3, (lam * (x1 - x3) - y1) % P256_P


def check_decompress(qx, qy):
    """Recover Y from X and a parity bit, the way the firmware will."""
    x = int.from_bytes(qx, "big")
    y = int.from_bytes(qy, "big")
    t = (x * x * x - 3 * x + P256_B) % P256_P
    root = pow(t, P256_SQRT_EXP, P256_P)
    assert (root * root) % P256_P == t, "p is not 3 mod 4 after all"
    got = root if (root & 1) == (y & 1) else P256_P - root
    assert got == y, "decompression does not recover Y"
    return 0x03 if (y & 1) else 0x02


def find_x_without_root():
    """An X for which x^3-3x+b is a non-residue, so no point has that X.

    Flipping a bit of a valid X is not a test: about half of all field elements
    are valid x-coordinates, so it passes as often as it fails."""
    for x in range(2, 2000):
        t = (x * x * x - 3 * x + P256_B) % P256_P
        if pow(t, (P256_P - 1) // 2, P256_P) != 1:
            return x
    raise AssertionError("no non-residue found, which cannot happen")


def check_curve(qx, qy):
    assert on_curve(P256_GX, P256_GY), "generator is not on the curve"
    assert on_curve(int.from_bytes(qx, "big"), int.from_bytes(qy, "big"))
    own = scalar_mul(P256_D, P256_GX, P256_GY)
    assert own == (int.from_bytes(qx, "big"), int.from_bytes(qy, "big")), \
        "curve constants disagree with the library's public key"


def p256_pub(d):
    key = ec.derive_private_key(d, ec.SECP256R1())
    nums = key.public_key().public_numbers()
    return key, nums.x.to_bytes(32, "big"), nums.y.to_bytes(32, "big")


def carray(name, data):
    body = "\n".join(textwrap.wrap(", ".join("0x%02x" % b for b in data), 88))
    body = textwrap.indent(body, "    ")
    return "static const uint8_t %s[%d] = {\n%s\n};\n" % (name, len(data), body)


# RFC 7748 6.1, copied from the RFC and checked against the library below.
# radio_devices_docs/wl55_device/security/self-tests.md
X_A_PRIV = bytes.fromhex(
    "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
X_A_PUB = bytes.fromhex(
    "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
X_B_PRIV = bytes.fromhex(
    "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
X_B_PUB = bytes.fromhex(
    "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
X_SHARED = bytes.fromhex(
    "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")
# u = 0 has order 1: every scalar sends it to zero, and zero must be refused.
X_LOW_ORDER = bytes(32)


def check_x25519():
    from cryptography.hazmat.primitives.asymmetric import x25519
    a = x25519.X25519PrivateKey.from_private_bytes(X_A_PRIV)
    b = x25519.X25519PrivateKey.from_private_bytes(X_B_PRIV)
    assert a.public_key().public_bytes_raw() == X_A_PUB, "RFC 7748 alice public"
    assert b.public_key().public_bytes_raw() == X_B_PUB, "RFC 7748 bob public"
    assert a.exchange(b.public_key()) == X_SHARED, "RFC 7748 shared secret"
    assert b.exchange(a.public_key()) == X_SHARED, "RFC 7748 shared, other way"


def main():
    check_gcm()
    check_x25519()

    lines = [
        "/* Generated by tools/gen_vectors.py - do not edit by hand. */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        carray("vec_gcm_key", GCM_KEY),
        carray("vec_gcm_iv", GCM_IV),
        carray("vec_gcm_aad", GCM_AAD),
        carray("vec_gcm_pt", GCM_PT),
        carray("vec_gcm_ct", GCM_CT),
        carray("vec_gcm_tag", GCM_TAG),
        carray("vec_x25519_a_priv", X_A_PRIV),
        carray("vec_x25519_a_pub", X_A_PUB),
        carray("vec_x25519_b_priv", X_B_PRIV),
        carray("vec_x25519_b_pub", X_B_PUB),
        carray("vec_x25519_shared", X_SHARED),
        carray("vec_x25519_low_order", X_LOW_ORDER),
    ]
    with open("Core/Inc/crypto_vectors.h", "w") as f:
        f.write("\n".join(lines))
    print("x25519 rfc7748 6.1 shared =", X_SHARED.hex())


if __name__ == "__main__":
    main()
