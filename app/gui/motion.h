#pragma once
// app/gui/motion.h — tlumeny dobeh.
// ----------------------------------------------------------------------------
// JEDNA funkce pohybu pro cele rozhrani: vytah v PLAY i slova na uvodni
// obrazovce dosedaji stejne. Neni to efekt navic, je to chovani pristroje —
// jako rucicka meridla, ktera dojede a ustali se.
//
// Zamerne bez zavislosti na ImGui, aby to slo drzet v AppContext.
#include <algorithm>
#include <cmath>

namespace ithaca::gui::motion {

// Pruzina s tlumenim. Integruje se per frame; kdyz rychlost i odchylka klesnou
// pod prah, dosedne PRESNE na cil (jinak by to donekonecna dokmitavalo o zlomky
// pixelu a nikdy nespustilo akci navazanou na ustaleni).
struct Settle {
    float pos = 0.f;
    float vel = 0.f;
    float target = 0.f;

    // Tuhost a tlumeni. Podkriticke tlumeni = jedno prejeti a dosednuti,
    // presne to chovani, ktere ma splash i vytah.
    float k = 130.f;
    float c = 22.f;

    void snapTo(float p) { pos = target = p; vel = 0.f; }

    // Vraci true v tom framu, ve kterem se prave ustalilo.
    bool step(float dt) {
        if (dt <= 0.f) return false;
        dt = std::min(dt, 0.05f);          // dlouhy frame nesmi rozhodit integraci
        const bool was_moving = moving();
        const float a = (target - pos) * k - vel * c;
        vel += a * dt;
        pos += vel * dt;
        if (!moving()) {
            pos = target; vel = 0.f;
            return was_moving;             // hrana: ustalilo se prave ted
        }
        return false;
    }

    bool moving() const {
        return std::fabs(target - pos) > 0.4f || std::fabs(vel) > 6.f;
    }
};

} // namespace ithaca::gui::motion
