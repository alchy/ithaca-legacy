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
    # Keystream je byte-identicky s naivni verzi (block_i = p//32, byte p%32 z
    # HMAC(key, nonce||u64le(block_i))), ale XOR delame bignum operaci v C misto
    # python loopu bajt-po-bajtu → radove rychlejsi pri bake velkych bank.
    n = len(data)
    if n == 0:
        return b""
    first_block = p0 // 32
    last_block = (p0 + n - 1) // 32
    ks = bytearray()
    for i in range(first_block, last_block + 1):
        ks += hmac_sha256(key, nonce + struct.pack("<Q", i))
    start = p0 % 32
    ks_slice = bytes(ks[start:start + n])   # presne n bajtu keystreamu
    x = int.from_bytes(data, "big") ^ int.from_bytes(ks_slice, "big")
    return x.to_bytes(n, "big")
