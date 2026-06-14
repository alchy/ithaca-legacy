// tests/test_sha256.cpp
// SHA-256 proti znamym NIST vektorum (FIPS 180-4).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "util/sha256.h"

#include <cstdio>
#include <string>

using namespace ithaca;

namespace {
std::string hex(const std::array<uint8_t, 32>& d) {
    char out[65];
    for (int i = 0; i < 32; ++i) std::snprintf(out + i * 2, 3, "%02x", d[i]);
    return std::string(out, 64);
}
} // namespace

TEST_CASE("sha256 prazdny vstup") {
    CHECK(hex(Sha256::hash("", 0)) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("sha256 'abc'") {
    CHECK(hex(Sha256::hash("abc", 3)) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256 dvoublokova zprava") {
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hex(Sha256::hash(m, 56)) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256 streaming update == one-shot") {
    Sha256 s;
    s.update("ab", 2);
    s.update("c", 1);
    CHECK(hex(s.finish()) == hex(Sha256::hash("abc", 3)));
}
