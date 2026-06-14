#!/usr/bin/env python3
# tests/test_ithaca_crypto.py
# Python krypto zrcadlo: HMAC/KDF/keystream proti TYMZ vektorum jako C++
# (tests/test_ithaca_crypto.cpp) -> garantuje cross-language paritu.
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import ithaca_crypto as ic


class TestCrypto(unittest.TestCase):
    def test_hmac_rfc4231_tc2(self):
        m = ic.hmac_sha256(b"Jefe", b"what do ya want for nothing?")
        self.assertEqual(
            m.hex(),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")

    def test_derive_key(self):
        secret = b"\x42" * 32
        ref = ic.hmac_sha256(secret, b"enc" + b"LIC")
        got = ic.derive_key(secret, "enc", b"LIC")
        self.assertEqual(got, ref)

    def test_keystream_involution_and_offset(self):
        key = b"\x11" * 32
        nonce = b"\x22" * 32
        orig = bytes((i * 7 + 3) & 0xFF for i in range(200))
        enc = ic.keystream_xor(key, nonce, 0, orig)
        self.assertNotEqual(enc, orig)
        self.assertEqual(ic.keystream_xor(key, nonce, 0, enc), orig)
        full = ic.keystream_xor(key, nonce, 0, orig)
        part = ic.keystream_xor(key, nonce, 50, orig[50:110])
        self.assertEqual(part, full[50:110])


if __name__ == "__main__":
    unittest.main()
