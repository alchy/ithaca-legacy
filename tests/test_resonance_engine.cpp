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

// Banka pro test: 4 noty (60, 64, 67, 72), kazda 1 slot, 1 variant, 1 mic.
// Sampl je dlouhy (5 s @ 48k) — vejde se do testu bez doejti pri 50 blocich.
struct Fixture {
    Bank bank;
    // SampleAsset musi prezit po dobu testu — drzime ho jako member.
    SampleAsset assets[128];

    Fixture() {
        const int frames = 48000 * 5;        // 5 sekund
        for (int n : {60, 64, 67, 72}) {
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
    res.setStrength(1.f);
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
    // 60 budila 64 (M3, 0.20), 67 (P5, 0.60), 72 (oktava, 0.70) — vse > min.
    CHECK(res.isResonating(64));
    CHECK(res.isResonating(67));
    CHECK(res.isResonating(72));

    // Ted hraj 64 — rule B zafade rezonanci 64 (ktera prave znela), pak alokuje
    // hlavni hlas 64. Vysledek: 64 ma main voice (rezonance se fade-uje k 0).
    VoiceSpec vs64;
    vs64.asset = &fx.bank.notes[64].slots[0].variants[0];
    vs64.pitch_ratio = 1.0; vs64.vel_gain = 1.f;
    pedal.noteOn(64);
    res.onSelfNoteOn(64, sr);                // rule B na rezonujici 64
    pool.noteOn(64, vs64, sr);
    res.onPlayedNoteOn(64, 100, fx.bank, pool, pedal, sr);

    // 60 ma main voice → eligibility (1) blokuje rezonanci 60 od 64 (M3).
    CHECK_FALSE(res.isResonating(60));
    // 64 prave fade-uje (rule B); active() je stale true do dojeti rampy.
    CHECK(res.isResonating(64));
    // Renderni dost bloku, aby rule B fade dosel (5 ms = 240 fr; 30 bloku × 256 = 7680 fr).
    renderNBlocks(res, pedal, 30);
    CHECK_FALSE(res.isResonating(64));      // rule B fade dohrany, eligibility (main voice 64) blokuje
    // 67 a 72 stale rezonuji od 60 (nemaji main voice; 64→67/72 jen aktualizuje).
    CHECK(res.isResonating(67));
    CHECK(res.isResonating(72));
}

TEST_CASE("ResonanceEngine: uniqueness (2) — multi-source jen aktualizuje amplitudu") {
    Fixture fx;
    PedalState pedal;
    VoicePool  pool(16);
    ResonanceEngine res;
    res.setStrength(1.f);
    const float sr = 48000.f;

    pedal.setSustainCC(127);

    // Note-on 60 BEZ pool.noteOn (cisty rezonancni test — testujeme jen budici
    // chovani; eligibility na 60 by neblokoval, kdyby nehrala). Note-on 60 budi
    // 64 (M3, 0.20), 67 (P5, 0.60), 72 (oktava, 0.70).
    res.onPlayedNoteOn(60, 80, fx.bank, pool, pedal, sr);
    const int after1 = res.activeCount();
    CHECK(after1 == 3);                     // 64, 67, 72 rezonuji
    CHECK(res.isResonating(64));
    CHECK(res.isResonating(67));
    CHECK(res.isResonating(72));
    // Necht 67 naramppuje k targetu (30 ms ramp = 1440 fr ~ 6 bloku po 256).
    renderNBlocks(res, pedal, 8);
    const float lvl_67_before = res.currentLevelFor(67);
    CHECK(lvl_67_before > 0.f);

    // Note-on 64 (taky bez pool.noteOn → 64 by mohla mit main voice; pro tento
    // test ji ZAMERNE neaktivujeme — testujeme uniqueness na rezonancnich
    // hlasech, ne eligibility). 64 budi 60 (M3, 0.20), 67 (m3, 0.10),
    // 72 (m6, 0.10). 67 a 72 uz rezonuji od 60 → musi jen update (uniqueness).
    // 60 doposud nehraje jako rezonance — alokuje se novy slot.
    res.onPlayedNoteOn(64, 80, fx.bank, pool, pedal, sr);
    const int after2 = res.activeCount();
    // Pribyl jen 1 novy hlas (60). Slot 67 a 72 zustavaji — uniqueness.
    CHECK(after2 == after1 + 1);
    CHECK(res.isResonating(60));
    CHECK(res.isResonating(67));
    CHECK(res.isResonating(72));
    // 64 NEsmi rezonovat (play-on-self filter na M=64; 64 sama na sebe).
    // Pozn: 64 mohla mit rezonanci jiz od prvniho note-on 60→64 — to nahore
    // ASSERTujeme. Po druhem note-on (64 sama) ji NIC NEMENI (play-on-self
    // nas neztracuje, ale ani nealokuje). Takze 64 STALE rezonuje.
    CHECK(res.isResonating(64));

    // Uniqueness: gain 67 nesmi klesnout (addExcitation jen zvysuje target).
    // Po dalsim renderu by gain mel byt >= predesly (decay je pomaly tau~5s).
    renderNBlocks(res, pedal, 2);
    const float lvl_67_after = res.currentLevelFor(67);
    // Tolerance: max() pricte vyssi z (old, new excite) → target stoupne;
    // gain rampuje k targetu. Per-blok decay je ~0.998, takze za 2 bloku
    // jen ~0.4% pokles z decayu — pred tim ale addExcitation pricetla vic.
    CHECK(lvl_67_after >= lvl_67_before * 0.99f);
}

TEST_CASE("ResonanceEngine: pravidlo B — note-on na rezonujici notu zafade rezonanci") {
    Fixture fx;
    PedalState pedal;
    VoicePool  pool(16);
    ResonanceEngine res;
    res.setStrength(1.f);
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
    res.setStrength(1.f);
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
