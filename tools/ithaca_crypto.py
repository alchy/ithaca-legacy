#!/usr/bin/env python3
"""ithaca_crypto.py — symetricka krypto pro pakovanou banku v2.

Zrcadli engine/util/ithaca_crypto.cpp (stejne vektory v testech garantuji
cross-language paritu). HMAC-SHA256 ze stdlib, CTR keystream, KDF.
"""
import hashlib
import hmac
import struct

NONCE_LEN = 32
KEY_LEN = 32


def hmac_sha256(key: bytes, msg: bytes) -> bytes:
    return hmac.new(key, msg, hashlib.sha256).digest()


def derive_key(secret: bytes, domain: str, msg: bytes) -> bytes:
    # HMAC(secret, domain || msg) — zrcadlo C++ deriveKey.
    return hmac_sha256(secret, domain.encode("ascii") + msg)


def keystream_xor(key: bytes, nonce: bytes, p0: int, data: bytes) -> bytes:
    # CTR keystream XOR. p0 = blob-relativni pozice prvniho bajtu data.
    out = bytearray(len(data))
    p = p0
    done = 0
    n = len(data)
    while done < n:
        block_i = p // 32
        ks = hmac_sha256(key, nonce + struct.pack("<Q", block_i))
        off = p % 32
        while off < 32 and done < n:
            out[done] = data[done] ^ ks[off]
            off += 1
            done += 1
            p += 1
    return bytes(out)
