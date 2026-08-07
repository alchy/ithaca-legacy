#pragma once
// app/gui/pace.h — prepinani tempa prekreslovani podle toho, jestli se neco deje.
// ----------------------------------------------------------------------------
// Stuhy v pozadi tecou porad (nosna vlna bezi nezavisle na zvuku), takze ciste
// udalostni rezim — "prekresli az kdyz neco prijde" — pouzit nejde. Tempo se
// ale menit da: kdyz nastroj mlci a nikdo se ho nedotyka, staci vlne podstatne
// mensi snimkova frekvence. Na pasivne chlazenem Pi je to rozdil mezi "hraje
// a zaroven pocita animaci" a "jen hraje".
//
// Dve pravidla, ktera jsou dulezitejsi nez uspora:
//
//  1. NAHORU OKAMZITE. Zrychleni je odpoved na dotek. Kdyby se cekalo, prvni
//     dotek by prisel na pomalu bezici panel a pusobil by liny — presne ta
//     vec, kvuli ktere se na pristroji poznaji levna resenei.
//
//  2. DOLU AZ PO PRODLEVE. Zpomalit hned po dohrani noty by znamenalo
//     prepinat tempo porad dokola pri kazde pomlce, a kazde prepnuti
//     swapIntervalu muze na nekterych ovladacich stat jedno skubnuti.
//
// Nezna ImGui ani engine: dostane "deje se neco" a vrati delitel.
// Testovatelne bez displeje (tests/test_pace.cpp).

namespace ithaca::gui {

class PaceControl {
public:
    // Jak dlouho musi byt klid, nez se zpomali.
    static constexpr float kIdleHold = 2.0f;

    // `busy` = deje se neco (hlas, pedal, animace, vstup),
    // `fast` = delitel pri cinnosti, `slow` = delitel v klidu (0 = nezpomalovat).
    // Vraci delitel, ktery se ma prave ted pouzit.
    int step(bool busy, float dt, int fast, int slow) {
        if (fast < 1) fast = 1;
        if (busy) {
            idle_s_ = 0.f;
            return fast;
        }
        if (slow <= fast) return fast;      // zpomalovani vypnute nebo nesmyslne
        if (dt > 0.f && dt < 1.f) idle_s_ += dt;   // dlouhy vypadek se nepocita
        return (idle_s_ >= kIdleHold) ? slow : fast;
    }

    float idleSeconds() const { return idle_s_; }

private:
    float idle_s_ = 0.f;
};

} // namespace ithaca::gui
