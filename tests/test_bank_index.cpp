// tests/test_bank_index.cpp
// Testuje ciste parsovaci funkce nazvu a detekci formatu.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "sample/bank_index.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace ithaca;

TEST_CASE("scanBank detekuje dynamic-velocity dle m### podslozek") {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "ithaca_dynbank_scan_test";
    fs::remove_all(root);
    fs::create_directories(root / "m060");
    fs::create_directories(root / "m061");
    std::ofstream(root / "m060" / "aaaa.wav") << "x";
    std::ofstream(root / "m060" / "bbbb.wav") << "x";   // 2 vrstvy pro notu 60
    std::ofstream(root / "m061" / "cccc.wav") << "x";   // 1 vrstva pro notu 61
    std::ofstream(root / "m060" / "notes.txt") << "x";  // ne-wav → preskocit

    BankScan scan = scanBank(root.string());
    CHECK(scan.format == BankFormat::DynamicVelocity);
    CHECK(scan.files.size() == 3);                      // jen .wav
    fs::remove_all(root);
}

TEST_CASE("parseFixedVelocityName rozparsuje mNNN-velV-fSS.wav") {
    ParsedName p = parseFixedVelocityName("m060-vel3-f48.wav");
    REQUIRE(p.ok);
    CHECK(p.midi == 60);
    CHECK(p.vel == 3);
    CHECK(p.sr_tag == 48);
}

TEST_CASE("parseFixedVelocityName akceptuje hranice (m021-vel0, m108-vel7)") {
    ParsedName a = parseFixedVelocityName("m021-vel0-f48.wav");
    REQUIRE(a.ok); CHECK(a.midi == 21); CHECK(a.vel == 0);
    ParsedName b = parseFixedVelocityName("m108-vel7-f96.wav");
    REQUIRE(b.ok); CHECK(b.midi == 108); CHECK(b.vel == 7); CHECK(b.sr_tag == 96);
}

TEST_CASE("parseFixedVelocityName akceptuje vsechny tri SR tagy (44/48/96)") {
    // Hard pozadavek: nove samply prijdou ve 44100/48000/96000 Hz → f44/f48/f96.
    // Zamyka, ze bound \d{2,3} v legacy regexu zadny z nich neodrizne.
    ParsedName a = parseFixedVelocityName("m060-vel3-f44.wav");
    REQUIRE(a.ok); CHECK(a.sr_tag == 44);
    ParsedName b = parseFixedVelocityName("m060-vel3-f48.wav");
    REQUIRE(b.ok); CHECK(b.sr_tag == 48);
    ParsedName c = parseFixedVelocityName("m060-vel3-f96.wav");
    REQUIRE(c.ok); CHECK(c.sr_tag == 96);
}

TEST_CASE("parseFixedVelocityName odmitne nelegacy nazvy") {
    CHECK_FALSE(parseFixedVelocityName("m60-front-abc123.wav").ok);   // extended
    CHECK_FALSE(parseFixedVelocityName("nahodny.wav").ok);
    CHECK_FALSE(parseFixedVelocityName("m060-v003-f48.wav").ok);      // 16-vrstva varianta nepodporovana
}

TEST_CASE("detectFormatFromName rozpozna legacy vs extended") {
    CHECK(detectFormatFromName("m060-vel3-f48.wav") == BankFormat::FixedVelocity);
    CHECK(detectFormatFromName("m60-front-abc123.wav") == BankFormat::Extended);
    CHECK(detectFormatFromName("m60-soundboard-abc123.wav") == BankFormat::Extended);
    CHECK(detectFormatFromName("readme.txt") == BankFormat::Unknown);
}

TEST_CASE("parseFixedVelocityName nespadne na prehnane dlouhych cislech") {
    CHECK_FALSE(parseFixedVelocityName("m060-vel3-f99999999999.wav").ok);  // SR tag moc dlouhy → nematchne
    CHECK_FALSE(parseFixedVelocityName("m9999-vel3-f48.wav").ok);          // nota moc dlouha
}
TEST_CASE("parseExtendedName nespadne na prehnane dlouhe note") {
    CHECK_FALSE(parseExtendedName("m999999999999-front-abc.wav").ok);  // nota moc dlouha → nematchne
}
TEST_CASE("scanBank prezije adresar nelze — vrati Unknown bez vyjimky") {
    BankScan s = scanBank("/tmp/ithaca_nope_zzz_xyz");
    CHECK(s.format == BankFormat::Unknown);
}
