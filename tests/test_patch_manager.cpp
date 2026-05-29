// tests/test_patch_manager.cpp
// Testuje vyber hlasu: velocity→slot, pitch-shift chybejicich not (2 osy),
// round-robin bez opakovani. Banka se staví v pameti (zadny disk).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "voice/patch_manager.h"
#include "sample/sample_types.h"

#include <cmath>

using namespace ithaca;

namespace {
// Pomocna: pridej notu s `nslots` sloty (kazdy 1 variant, rostouci RMS),
// volitelne `rr` round-robin variant v prvnim slotu.
void addNote(Bank& b, int midi, int nslots, int rr_in_slot0 = 1) {
    for (int s = 0; s < nslots; ++s) {
        VelocitySlot slot;
        slot.rms_db = -40.f + (float)s * 5.f;          // vzestupne
        int variants = (s == 0) ? rr_in_slot0 : 1;
        for (int v = 0; v < variants; ++v) {
            SampleAsset a;
            a.peak_rms_db = slot.rms_db;
            MicLayer m; m.mic_name = "stereo"; m.frames = 100; m.sample_rate = 48000;
            m.data.assign(100 * 2, 0.1f);
            a.mics.push_back(std::move(m));
            slot.variants.push_back(std::move(a));
        }
        b.notes[midi].slots.push_back(std::move(slot));
        b.notes[midi].recorded = true;
    }
}
} // namespace

TEST_CASE("selectVoice: presna nota → pitch_ratio 1.0") {
    Bank b; addNote(b, 60, 8);
    RoundRobinState rr;
    VoiceSpec vs = selectVoice(b, 60, 100, rr);
    REQUIRE(vs.asset != nullptr);
    CHECK(vs.pitch_ratio == doctest::Approx(1.0).epsilon(0.0001));
    CHECK(vs.vel_gain > 0.f);
}

TEST_CASE("selectVoice: chybejici nota → transpozice z nejblizsi") {
    Bank b; addNote(b, 60, 4);                          // jen nota 60
    RoundRobinState rr;
    VoiceSpec up = selectVoice(b, 62, 100, rr);         // o 2 pultony vys
    REQUIRE(up.asset != nullptr);
    CHECK(up.pitch_ratio == doctest::Approx(std::pow(2.0, 2.0/12.0)).epsilon(0.001));
    VoiceSpec dn = selectVoice(b, 57, 100, rr);         // o 3 pultony niz
    REQUIRE(dn.asset != nullptr);
    CHECK(dn.pitch_ratio == doctest::Approx(std::pow(2.0, -3.0/12.0)).epsilon(0.001));
}

TEST_CASE("selectVoice: vyssi velocity → vyssi slot (hlasitejsi)") {
    Bank b; addNote(b, 60, 8);
    RoundRobinState rr;
    VoiceSpec soft = selectVoice(b, 60, 1, rr);
    VoiceSpec loud = selectVoice(b, 60, 127, rr);
    REQUIRE(soft.asset != nullptr); REQUIRE(loud.asset != nullptr);
    // loud asset ma vyssi peak_rms_db nez soft (vybral hlasitejsi slot).
    CHECK(loud.asset->peak_rms_db > soft.asset->peak_rms_db);
}

TEST_CASE("selectVoice: round-robin neopakuje naposledy hranou variantu") {
    Bank b; addNote(b, 60, 1, /*rr_in_slot0=*/3);      // 1 slot, 3 RR varianty
    RoundRobinState rr;
    const SampleAsset* prev = nullptr;
    // Pri 3 variantach se zadne dve po sobe nesmi shodovat.
    for (int i = 0; i < 20; ++i) {
        VoiceSpec vs = selectVoice(b, 60, 64, rr);
        REQUIRE(vs.asset != nullptr);
        CHECK(vs.asset != prev);                        // nikdy stejny jako minuly
        prev = vs.asset;
    }
}

TEST_CASE("selectVoice: prazdna banka → asset nullptr") {
    Bank b;
    RoundRobinState rr;
    VoiceSpec vs = selectVoice(b, 60, 100, rr);
    CHECK(vs.asset == nullptr);
}
