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

    // Delka podkroku integrace a strop na delku snimku. Podkrok o neco delsi
    // nez 1/60 s, aby pri bezne snimkove frekvenci vysel presne jeden.
    static constexpr float kSubStep = 1.f / 59.f;
    static constexpr float kDtMax   = 0.25f;

    void snapTo(float p) { pos = target = p; vel = 0.f; }

    // Vraci true v tom framu, ve kterem se prave ustalilo.
    //
    // Integruje se po PEVNYCH podkrocich, ne jednim krokem o delce snimku.
    // Explicitni Euler je pri velkem kroku nestabilni, takze puvodni verze dt
    // orezavala na 0,05 s — jenze tim se pruzina pri nizke snimkove frekvenci
    // zpomalila oproti realnemu casu (pri 12 fps by dosedala 1,7x dele).
    // Podkroky drzi stabilitu i skutecne tempo zaroven; pri 60 fps je podkrok
    // presne jeden, takze se chovani nezmenilo.
    bool step(float dt) {
        if (!(dt > 0.f)) return false;
        dt = std::min(dt, kDtMax);         // dlouhy vypadek se nedohani
        const bool was_moving = moving();

        int n = (int)(dt / kSubStep) + 1;
        const float h = dt / (float)n;
        for (int i = 0; i < n; ++i) {
            const float a = (target - pos) * k - vel * c;
            vel += a * h;
            pos += vel * h;
        }

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
