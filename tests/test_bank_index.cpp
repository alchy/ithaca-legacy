// tests/test_bank_index.cpp
// Testuje ciste parsovaci funkce nazvu a detekci formatu.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "sample/bank_index.h"

#include <string>

using namespace ithaca;

TEST_CASE("parseLegacyName rozparsuje mNNN-velV-fSS.wav") {
    ParsedName p = parseLegacyName("m060-vel3-f48.wav");
    REQUIRE(p.ok);
    CHECK(p.midi == 60);
    CHECK(p.vel == 3);
    CHECK(p.sr_tag == 48);
}

TEST_CASE("parseLegacyName akceptuje hranice (m021-vel0, m108-vel7)") {
    ParsedName a = parseLegacyName("m021-vel0-f48.wav");
    REQUIRE(a.ok); CHECK(a.midi == 21); CHECK(a.vel == 0);
    ParsedName b = parseLegacyName("m108-vel7-f96.wav");
    REQUIRE(b.ok); CHECK(b.midi == 108); CHECK(b.vel == 7); CHECK(b.sr_tag == 96);
}

TEST_CASE("parseLegacyName akceptuje vsechny tri SR tagy (44/48/96)") {
    // Hard pozadavek: nove samply prijdou ve 44100/48000/96000 Hz → f44/f48/f96.
    // Zamyka, ze bound \d{2,3} v legacy regexu zadny z nich neodrizne.
    ParsedName a = parseLegacyName("m060-vel3-f44.wav");
    REQUIRE(a.ok); CHECK(a.sr_tag == 44);
    ParsedName b = parseLegacyName("m060-vel3-f48.wav");
    REQUIRE(b.ok); CHECK(b.sr_tag == 48);
    ParsedName c = parseLegacyName("m060-vel3-f96.wav");
    REQUIRE(c.ok); CHECK(c.sr_tag == 96);
}

TEST_CASE("parseLegacyName odmitne nelegacy nazvy") {
    CHECK_FALSE(parseLegacyName("m60-front-abc123.wav").ok);   // extended
    CHECK_FALSE(parseLegacyName("nahodny.wav").ok);
    CHECK_FALSE(parseLegacyName("m060-v003-f48.wav").ok);      // 16-vrstva varianta nepodporovana
}

TEST_CASE("detectFormatFromName rozpozna legacy vs extended") {
    CHECK(detectFormatFromName("m060-vel3-f48.wav") == BankFormat::Legacy);
    CHECK(detectFormatFromName("m60-front-abc123.wav") == BankFormat::Extended);
    CHECK(detectFormatFromName("m60-soundboard-abc123.wav") == BankFormat::Extended);
    CHECK(detectFormatFromName("readme.txt") == BankFormat::Unknown);
}

TEST_CASE("parseLegacyName nespadne na prehnane dlouhych cislech") {
    CHECK_FALSE(parseLegacyName("m060-vel3-f99999999999.wav").ok);  // SR tag moc dlouhy → nematchne
    CHECK_FALSE(parseLegacyName("m9999-vel3-f48.wav").ok);          // nota moc dlouha
}
TEST_CASE("parseExtendedName nespadne na prehnane dlouhe note") {
    CHECK_FALSE(parseExtendedName("m999999999999-front-abc.wav").ok);  // nota moc dlouha → nematchne
}
TEST_CASE("scanBank prezije adresar nelze — vrati Unknown bez vyjimky") {
    BankScan s = scanBank("/tmp/ithaca_nope_zzz_xyz");
    CHECK(s.format == BankFormat::Unknown);
}
