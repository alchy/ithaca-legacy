// tests/test_resonance_engine.cpp
// Testuje 5.5.1 invariant ResonanceEngine: eligibility, uniqueness, rule B,
// pedal UP fade. Fixture banka v pameti (4 noty: 60 64 67 72 — C major triad
// + oktava), zadny disk I/O.
//
// Pozor — tato testovaci sada VAZNE testuje invariant 5.5.1: NIKDY se nesmi
// soucasne hrat hlavni hlas a rezonancni hlas tehoz N. Kdyby tyto testy
// padly, je to BUG v eligibility filter / rule B / per-nota uniqueness.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "resonance/resonance_engine.h"
#include "voice/voice_pool.h"
#include "voice/patch_manager.h"
#include "pedal/pedal_state.h"
#include "sample/sample_types.h"

#include <array>
#include <vector>

using namespace ithaca;

namespace {

// FullyLoaded SampleAsset s konstantnim signalem (zadny streaming, zadny disk).
// resonance_start_frame = 0 → ResonanceVoice cte primo z preload_head.
SampleAsset makeAsset(float amp, int frames) {
    SampleAsset a;
    a.peak_rms_db     = 0.f;
    a.attack_end_frame = 0;
    MicLayer m;
    m.mic_name         = "stereo";
    m.file.frames      = frames;
    m.file.sample_rate = 48000;
    m.file.valid       = true;
    m.mode             = MicLayerMode::FullyLoaded;
    m.head_frames      = frames;
    m.preload_head.assign((size_t)frames * 2, amp);
    // resonance buffer: FullyLoaded => prazdny (ResonanceVoice cte z preload_head).
    m.resonance_start_frame = 0;
    m.resonance_frames      = 0;
    a.mics.push_back(std::move(m));
    return a;
}

// Banka pro test: noty 48, 60, 64, 67, 72 (oktava dolu + C dur trojzvuk +
// oktava nahoru), kazda 1 slot, 1 variant, 1 mic. Partial-coincidence model:
// silni partneri 60 jsou 72 (oktava nahoru), 48 (oktava dolu), 67 (kvinta);
// 64 (V.tercie) je pod prahem → nerezonuje. Sampl 5 s @ 48k.
struct Fixture {
    Bank bank;
    // SampleAsset musi prezit po dobu testu — drzime ho jako member.
    SampleAsset assets[128];

    Fixture() {
        const int frames = 48000 * 5;        // 5 sekund
        for (int n : {48, 60, 64, 67, 72}) {
            assets[n] = makeAsset(0.25f, frames);
            VelocitySlot slot;
            slot.rms_db = -12.f;
            slot.variants.push_back(assets[n]);   // copy do banky
            bank.notes[n].slots.push_back(std::move(slot));
            bank.notes[n].recorded = true;
        }
    }
};

// Renderuj N bloku po 256 frames a zahod vystup.
void renderNBlocks(ResonanceEngine& res, const PedalState& pedal,
                   int n_blocks, int block = 256) {
    std::vector<float> L((size_t)block, 0.f), R((size_t)block, 0.f);
    for (int i = 0; i < n_blocks; ++i) {
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        res.processBlock(L.data(), R.data(), block, pedal);
    }
}

} // namespace

TEST_CASE("ResonanceEngine: eligibility (1) — N s aktivni main voice neni eligible") {
    Fixture fx;
    PedalState pedal;
    VoicePool  pool(16);
    ResonanceEngine res;
    res.setEnabled(true); res.setGainDb(0.f);   // gain_lin = 1.0 (jako drive strength 1.0)
    const float sr = 48000.f;

    pedal.setSustainCC(127);  // pedal dolu — vsechny struny undamped

    // Plne sequence note-on (jako to dela Engine): pedal.noteOn → res.onSelfNoteOn
    // (rule B) → pool.noteOn → res.onPlayedNoteOn. Bez onSelfNoteOn by case,
    // kdy nota M budi rezonanci N a vzapeti se hraje N, neuvolnila rezonanci.
    VoiceSpec vs60;
    vs60.asset = &fx.bank.notes[60].slots[0].variants[0];
    vs60.pitch_ratio = 1.0; vs60.vel_gain = 1.f;
    pedal.noteOn(60);
    res.onSelfNoteOn(60, sr);
    pool.noteOn(60, vs60, sr);
    res.onPlayedNoteOn(60, 100, fx.bank, pool, pedal, sr);
    // 60 nesmi mit rezonanci (play-on-self filter v onPlayedNoteOn).
    CHECK_FALSE(res.isResonating(60));
    // 60 budi silne partnery: 48 (oktava dolu), 67 (kvinta), 72 (oktava nahoru).
    CHECK(res.isResonating(48));
    CHECK(res.isResonating(67));
    CHECK(res.isResonating(72));
    // 64 (V.tercie) je v partial-coincidence modelu pod prahem → nerezonuje.
    CHECK_FALSE(res.isResonating(64));

    // Ted hraj 48 (silny partner, ktery prave rezonoval) — rule B zafade jeho
    // rezonanci, pak alokuje hlavni hlas 48.
    VoiceSpec vs48;
    vs48.asset = &fx.bank.notes[48].slots[0].variants[0];
    vs48.pitch_ratio = 1.0; vs48.vel_gain = 1.f;
    pedal.noteOn(48);
    res.onSelfNoteOn(48, sr);                // rule B na rezonujici 48
    pool.noteOn(48, vs48, sr);
    res.onPlayedNoteOn(48, 100, fx.bank, pool, pedal, sr);

    // 48 budi 60 (oktava nahoru, silne), ale 60 MA main voice → eligibility (1)
    // tu rezonanci blokuje.
    CHECK_FALSE(res.isResonating(60));
    // 48 prave fade-uje (rule B); active() je stale true do dojeti rampy.
    CHECK(res.isResonating(48));
    // Render dost bloku, aby rule B fade dosel (5 ms = 240 fr; 30 bloku × 256 = 7680 fr).
    renderNBlocks(res, pedal, 30);
    CHECK_FALSE(res.isResonating(48));      // rule B fade dohrany, main voice 48 blokuje
    // 67 a 72 stale rezonuji od 60 (nemaji main voice).
    CHECK(res.isResonating(67));
    CHECK(res.isResonating(72));
}

TEST_CASE("ResonanceEngine: uniqueness (2) — multi-source jen aktualizuje amplitudu") {
    Fixture fx;
    PedalState pedal;
    VoicePool  pool(16);
    ResonanceEngine res;
    res.setEnabled(true); res.setGainDb(0.f);   // gain_lin = 1.0 (jako drive strength 1.0)
    const float sr = 48000.f;

    pedal.setSustainCC(127);

    // Note-on 60 BEZ pool.noteOn (cisty rezonancni test — testujeme jen budici
    // chovani, ne eligibility). 60 budi silne partnery 48 (oktava dolu), 67
    // (kvinta), 72 (oktava nahoru). 64 (V.tercie) je pod prahem.
    res.onPlayedNoteOn(60, 80, fx.bank, pool, pedal, sr);
    const int after1 = res.activeCount();
    CHECK(after1 == 3);                     // 48, 67, 72 rezonuji
    CHECK(res.isResonating(48));
    CHECK(res.isResonating(67));
    CHECK(res.isResonating(72));
    CHECK_FALSE(res.isResonating(64));
    // Necht 48 naramppuje k targetu (30 ms ramp = 1440 fr ~ 6 bloku po 256).
    renderNBlocks(res, pedal, 8);
    const float lvl_48_before = res.currentLevelFor(48);
    CHECK(lvl_48_before > 0.f);

    // Note-on 72 (taky bez pool.noteOn). 72 budi 60 (oktava dolu — NOVY slot,
    // 60 zatim nerezonuje) a 48 (dve oktavy dolu — UZ rezonuje od 60 → jen
    // update, uniqueness). 72 sama je play-on-self → jeji rezonance od 60 trva.
    res.onPlayedNoteOn(72, 80, fx.bank, pool, pedal, sr);
    const int after2 = res.activeCount();
    // Pribyl jen 1 novy hlas (60). Sloty 48, 67, 72 zustavaji — uniqueness.
    CHECK(after2 == after1 + 1);
    CHECK(res.isResonating(60));
    CHECK(res.isResonating(48));
    CHECK(res.isResonating(67));
    CHECK(res.isResonating(72));            // play-on-self ji neztlumi (trva od 60)

    // Uniqueness: gain 48 nesmi klesnout (72→48 addExcitation jen zvysuje target).
    renderNBlocks(res, pedal, 2);
    const float lvl_48_after = res.currentLevelFor(48);
    // Tolerance: max() pricte vyssi z (old, new excite) → target stoupne;
    // gain rampuje k targetu. Per-blok decay ~0.998 → za 2 bloky ~0.4% pokles.
    CHECK(lvl_48_after >= lvl_48_before * 0.99f);
}

TEST_CASE("ResonanceEngine: pravidlo B — note-on na rezonujici notu zafade rezonanci") {
    Fixture fx;
    PedalState pedal;
    VoicePool  pool(16);
    ResonanceEngine res;
    res.setEnabled(true); res.setGainDb(0.f);   // gain_lin = 1.0 (jako drive strength 1.0)
    const float sr = 48000.f;

    pedal.setSustainCC(127);

    // Note-on 60 (jen pedal, ne main voice) → budi rezonance na 67 + 72 + 64.
    res.onPlayedNoteOn(60, 100, fx.bank, pool, pedal, sr);
    REQUIRE(res.isResonating(67));
    // Necht rezonance 67 mela cas naramppovat k targetu.
    renderNBlocks(res, pedal, 4);
    const float lvl_67_pre = res.currentLevelFor(67);
    CHECK(lvl_67_pre > 0.f);

    // Uzivatel stiskne primo 67 (sebe). RULE B: rezonance 67 → fast fade.
    res.onSelfNoteOn(67, sr);
    // Voice ted fade-uje (5 ms ramp); rule B clearne last_excite na 0 →
    // eligibility filter zablokuje dalsi excitaci 67.
    CHECK(res.isResonating(67));            // jeste hraje (fade probiha)
    CHECK(res.currentLevelFor(67) > 0.f);

    // Render 50 bloku × 256 fr = 12800 fr @ 48k = ~267 ms. Fade je 5 ms = 240 fr
    // → davno za to. Rezonance 67 musi byt pryc.
    renderNBlocks(res, pedal, 50);
    CHECK_FALSE(res.isResonating(67));
}

TEST_CASE("ResonanceEngine: pedal UP -> rezonance ne-drzenych not fade") {
    Fixture fx;
    PedalState pedal;
    VoicePool  pool(16);
    ResonanceEngine res;
    res.setEnabled(true); res.setGainDb(0.f);   // gain_lin = 1.0 (jako drive strength 1.0)
    const float sr = 48000.f;
    // Per-blok decay nastav agresivnejsi (tau=100 ms), aby test bezel rychle.
    res.setExciteDecayTimeMs(100.f, 256, sr);

    pedal.setSustainCC(127);  // pedal dolu

    // Note-on 60 jen pres rezonance (zadne klavesy nedrzime).
    res.onPlayedNoteOn(60, 100, fx.bank, pool, pedal, sr);
    REQUIRE(res.isResonating(67));
    REQUIRE(res.isResonating(72));
    renderNBlocks(res, pedal, 4);
    REQUIRE(res.currentLevelFor(67) > 0.f);

    // Pedal nahoru — damping_[67] = 0 (held_[67] = false). processBlock pak
    // pretahne target_gain = last_excite * 0 = 0 → setTargetGain(0) → fade.
    pedal.setSustainCC(0);

    // Render dost bloku, aby fade dohajel. ResonanceVoice fade z target=0 je
    // rampovany (ramp_frames = 30 ms @ 48k = 1440 fr ~ 6 bloku). +decay
    // last_excite tau=100ms → po par blocich je target ~0.
    renderNBlocks(res, pedal, 50);
    CHECK_FALSE(res.isResonating(67));
    CHECK_FALSE(res.isResonating(72));
}

TEST_CASE("ResonanceVoice: sub-epsilon target -> deaktivace (zadny stuck voice)") {
    // Root cause regrese: setTargetGain s targetem mezi kResonanceGainEpsilon
    // (1e-5) a kResonanceTargetEpsilon (1e-4) drive oznacil hlas jako fading-out,
    // ale gain_ se ustalil na te male hodnote (> gain epsilon) → hlas se NIKDY
    // nedeaktivoval (stuck voice / leak). Po fixu musi dohajet na 0 a zhasnout.
    SampleAsset asset = makeAsset(0.5f, 48000);   // 1 s FullyLoaded (sample nedojede)
    const MicLayer* mic = &asset.mics[0];
    ResonanceVoice v;
    v.start(60, mic, 1.0f, 0.5f, 0.5f, 48000.f);
    float bl[256], br[256];
    auto render = [&]{ for (int i=0;i<256;++i){bl[i]=0.f;br[i]=0.f;} return v.process(bl, br, 256); };
    for (int b = 0; b < 8; ++b) render();        // gain vystoupa k 1.0
    REQUIRE(v.active());
    v.setTargetGain(5e-5f);                        // do "mrtveho pasma" mezi epsilony
    for (int b = 0; b < 30 && v.active(); ++b) render();  // ~30 bloku >> 30ms ramp
    CHECK_FALSE(v.active());                        // MUSI se deaktivovat
}

TEST_CASE("ResonanceEngine: setMaxVoices / maxVoices round-trip + clamp") {
    ithaca::ResonanceEngine re(32);
    CHECK(re.maxVoices() == 32);
    re.setMaxVoices(8);
    CHECK(re.maxVoices() == 8);
    re.setMaxVoices(0);            // clamp na >= 1
    CHECK(re.maxVoices() == 1);
    re.setMaxVoices(999);          // clamp na <= 64
    CHECK(re.maxVoices() == 64);
}

TEST_CASE("ResonanceEngine cache-ready API") {
    using namespace ithaca;
    ResonanceEngine res(8);
    std::array<bool,128> ready{}; ready[60] = true;
    CHECK_NOTHROW(res.setCacheReady(ready));
    CHECK_NOTHROW(res.clearCacheReady());
    CHECK_NOTHROW(res.requestRecacheFade());
}

TEST_CASE("budget gate pri plnem rozpoctu: prezije nejsilnejsi harmonika (targetGain srovnani)") {
    Fixture fx;
    PedalState pedal;
    VoicePool  pool(16);
    ResonanceEngine res;
    res.setEnabled(true); res.setGainDb(0.f);
    res.setMaxVoices(1);
    const float sr = 48000.f;
    pedal.setSustainCC(127);
    res.onPlayedNoteOn(48, 100, fx.bank, pool, pedal, sr);
    renderNBlocks(res, pedal, 2);   // fading (gain~0) hlasy se hned deaktivuji
    // Nejsilnejsi harmonika zdroje 48 je 60 (oktava nahoru, harm ~1.0).
    // Drive gate srovnaval init_gain s currentLevel()==0 cerstve spawnutych →
    // kazda dalsi harmonika ukradla predchozi a prezila POSLEDNI (72) +
    // spawn-churn (ring acquire + diskove cteni na kazdou harmoniku).
    CHECK(res.isResonating(60));
    CHECK_FALSE(res.isResonating(72));
    CHECK(res.activeCount() == 1);
}
