#pragma once
// app/gui/glow_auto.h — automaticke skrceni zare vlny pod tlakem.
// ----------------------------------------------------------------------------
// Dosah zare je jedina vec na panelu, ktera roste s VYPLNI: pri polomeru 10 px
// se kresli pas 20 px siroky pres celou obrazovku, pri 200 px uz je to pruh
// pres pul displeje. Na desktopu je to jedno, na V3D v pasivne chlazenem Pi ne.
//
// Proto je polomer parametr (GuiState::wave_glow) a nad nim tenhle regulator,
// ktery ho smi SNIZIT, kdyz uz to panel nestiha.
//
// CO SE MERI: PERIODA SNIMKU, ne cas straveny v kresleni stuh. Merit CPU cas
// zare by byla chyba: pocet drah profilu roste s polomerem jen LOGARITMICKY
// (glowLanes), zatimco vyplnena plocha roste s jeho DRUHOU mocninou. Desetkrat
// sirsi zar tedy stoji na CPU skoro totez a na GPU stonasobek — regulator
// postaveny na CPU case by na ni temer nereagoval. Perioda snimku zachyti obe
// strany: kdyz to nestiha grafika, zmesknou se vsync a perioda skoci.
//
// Z toho plyne i volba rozpoctu: nema to byt "kolik smi zar stat", ale strop
// PERIODY. Na panelu 60 Hz je nominal 16,7 ms, takze ~25 ms uz znamena
// zmesknuty snimek. Nula = automatika vypnuta (vychozi).
//
// Tri veci, na kterych zalezi vic nez na presnosti regulace:
//
//  1. NESMI TO PUMPOVAT. Zar, ktera dycha se zatezi, by byla horsi nez zar,
//     ktera je trvale mensi — a hlavne by dychala prave pri hrani, protoze
//     vic hlasu = vic prace = mensi zar. Proto se meni v DISKRETNICH krocich
//     a az po nekolika vterinach setrvaleho stavu, ne spojite.
//
//  2. DOLU RYCHLE, NAHORU POMALU. Prekroceni rozpoctu je problem hned;
//     navrat je luxus, ktery muze pockat. Nesymetricke casy tim zaroven
//     brani kmitani kolem prahu.
//
//  3. PASMO NECINNOSTI. Zvysuje se az kdyz je cena hluboko pod rozpoctem,
//     ne hned jak se pod nej dostane — jinak by se stridalo nahoru/dolu.
//
// Nezna ImGui ani engine: dostane zmerenou cenu a vrati nasobitel. Testovatelne
// bez displeje (tests/test_glow_auto.cpp).
#include <algorithm>
#include <cmath>

namespace ithaca::gui {

class GlowAuto {
public:
    // Nasobitel dosahu, ktery se ma prave ted kreslit. Vzdy <= `want`.
    float scale() const { return cur_; }
    float smoothedFrameMs() const { return ema_ms_; }

    // `frame_ms`  = perioda tohohle snimku [ms],
    // `want`      = co si preje uzivatel,
    // `budget_ms` = strop periody (0 = automatika vypnuta).
    void step(float frame_ms, float want, float budget_ms, float dt) {
        // -- Ocisteni vstupu ------------------------------------------------
        // Zapis pres negaci chyti NaN i zapornou hodnotu; std::max(NaN, 0.f)
        // by NaN propustil dal.
        //
        // Nekonecna perioda musi byt SRAZENA, ne jen propustena: prvni krok by
        // z EMA udelal nekonecno a druhy (inf - inf) NaN, po kterem uz jsou
        // vsechna porovnani nepravdiva a regulator natrvalo prestane reagovat.
        // Nasel to test, ne uvaha.
        if (frame_ms != frame_ms) return;        // NaN = rozbite hodiny, ignoruj
        if (!(frame_ms < kFrameMsMax)) frame_ms = kFrameMsMax;   // vc. nekonecna
        if (!(frame_ms >= 0.f))  frame_ms = 0.f;
        if (!(want >= 0.f))      want = 0.f;
        if (!(budget_ms >= 0.f)) budget_ms = 0.f;
        if (!(dt > 0.f) || !(dt < kDtMax)) return;
        if (cur_ > want) cur_ = want;            // uzivatel snizil strop

        // Vyhlazeni. Jedna spicka z planovace nesmi hnout polomerem.
        const float k = 1.f - std::exp(-dt / kTau);
        ema_ms_ += (frame_ms - ema_ms_) * k;

        if (budget_ms <= 0.f) return;            // automatika vypnuta

        if (ema_ms_ > budget_ms) {
            over_s_ += dt; under_s_ = 0.f;
            if (over_s_ >= kOverHold && cur_ > kMinScale) {
                cur_ = std::max(cur_ * kStepDown, kMinScale);
                over_s_ = 0.f;
            }
        } else if (ema_ms_ < budget_ms * kRelease && cur_ < want) {
            under_s_ += dt; over_s_ = 0.f;
            if (under_s_ >= kUnderHold) {
                cur_ = std::min(cur_ * kStepUp, want);
                under_s_ = 0.f;
            }
        } else {
            // Pasmo necinnosti: ani prilis draho, ani dost levne na navrat.
            over_s_ = under_s_ = 0.f;
        }
    }

    // Uzivatel zmenil parametr rucne — zacit od nej, ne od zdedene hodnoty.
    void reset(float want) {
        cur_ = (want >= 0.f) ? want : 0.f;      // chyti i NaN
        over_s_ = under_s_ = 0.f;
    }

private:
    static constexpr float kTau       = 1.0f;   // vyhlazeni ceny [s]
    static constexpr float kOverHold  = 3.0f;   // jak dlouho pres rozpocet, nez ubereme
    static constexpr float kUnderHold = 20.0f;  // ... a jak dlouho pod nim, nez pridame
    static constexpr float kRelease   = 0.5f;   // navrat az pod polovinou rozpoctu
    static constexpr float kStepDown  = 0.6f;
    static constexpr float kStepUp    = 1.5f;
    // Nula by znamenala holou caru; na tu se spadne jen rucne, ne automatikou —
    // automatika ma ubrat na vzhledu, ne ho vypnout.
    static constexpr float kMinScale  = 0.15f;
    // Meze duveryhodnosti vstupu. Deset vterin na snimek uz neni pomaly panel,
    // ale zaseknuty proces; delsi krok casu nema smysl integrovat.
    static constexpr float kFrameMsMax = 10000.f;
    static constexpr float kDtMax      = 10.f;

    float cur_    = 1.f;
    float ema_ms_ = 0.f;
    float over_s_ = 0.f;
    float under_s_ = 0.f;
};

} // namespace ithaca::gui
