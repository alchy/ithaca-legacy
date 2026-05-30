// tests/test_patch_manager.cpp
// Testuje vyber hlasu: velocity→slot, chybejici nota → ticho (zadny resampling),
// round-robin bez opakovani. Banka se stavi v pameti (zadny disk).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "voice/patch_manager.h"
#include "sample/sample_types.h"

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
            MicLayer m;
            m.mic_name         = "stereo";
            m.file.frames      = 100;
            m.file.sample_rate = 48000;
            m.file.valid       = true;
            m.mode             = MicLayerMode::FullyLoaded;
            m.head_frames      = 100;
            m.preload_head.assign(100 * 2, 0.1f);
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

TEST_CASE("selectVoice: chybejici nota → asset nullptr (ticho, bez resamplingu)") {
    // Rozhodnuti 2026-05-30: zadny pitch-shift z nejblizsi noty.
    // Chybejici nota se NEodvozuje — vrati se prazdny VoiceSpec (player = ticho).
    Bank b; addNote(b, 60, 4);                          // jen nota 60
    RoundRobinState rr;
    CHECK(selectVoice(b, 62, 100, rr).asset == nullptr);   // soused 60 nepouziti
    CHECK(selectVoice(b, 57, 100, rr).asset == nullptr);
    // Presne nota 60 ale stale hraje.
    VoiceSpec exact = selectVoice(b, 60, 100, rr);
    REQUIRE(exact.asset != nullptr);
    CHECK(exact.pitch_ratio == doctest::Approx(1.0).epsilon(0.0001));
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
