#pragma once
// app/gui/layout.h — JEDINY zdroj pravdy pro rozmery GUI.
// ----------------------------------------------------------------------------
// Cil je 7" dotykovy panel 1280x720 zabudovany v nastroji (~210 DPI).
// Z toho plyne jedina tvrda podminka, ze ktere se odvozuje skoro vsechno
// ostatni: prst potrebuje ~9 mm, coz je na tomhle panelu ~74 px. Zadny
// interaktivni prvek nesmi byt mensi.
//
// DPI: rozmery se NESKALUJI. ImGui pracuje v logickych bodech a prepocet na
// fyzicke pixely resi backend (io.DisplayFramebufferScale z GLFW), takze
// nasobit rozmery content-scalem by scale aplikovalo DVAKRAT. g_scale slouzi
// VYHRADNE k rasterizaci pisem ve fyzickem rozliseni (viz theme.h::load_fonts).
#include "imgui.h"

#include <algorithm>

namespace ithaca::gui::layout {

// Globalni DPI scale z glfwGetWindowContentScale(). Cte ho jen load_fonts.
inline float g_scale = 1.0f;

namespace Dims {
    // Panel.
    inline constexpr float win_w = 1280.f;
    inline constexpr float win_h = 720.f;

    // Minimalni dotykovy cil. Vsechno interaktivni se od nej odviji.
    inline constexpr float touch = 74.f;

    // Ramecek kolem plochy — mrtva zona, aby se u kraje panelu nedalo omylem
    // trefit ovladani. Neni to ozdoba, je to funkcni odsazeni.
    inline constexpr float bezel = 16.f;

    // Vodorovna pasma.
    // Vyska hlavniho radku zalozek se POCITA — je to strana ctverce, tedy sirka
    // bunky (viz squareTabH nize). Tahle konstanta je jen strop pro pripad, ze
    // by okno bylo extremne siroke.
    inline constexpr float tab_h_max = 200.f;
    inline constexpr float tab_gap   = 3.f;
    inline constexpr float subtab_h= 56.f;    // druhy radek (jen DSP) — mensi, ale nad prst
    inline constexpr float lamp_h  = 26.f;    // radek kontrolek nad spodnim okrajem
    inline constexpr float foot_h  = 34.f;    // stitek + SR/buffer dole

    // Seznam (vytah v PLAY, banky, log).
    inline constexpr float row_h      = 60.f;   // radek seznamu
    inline constexpr float row_big_h  = 76.f;   // zvyrazneny radek

    // Parametr = jediny pas. Popisek i hodnota se sazi DOVNITR pasu (vlevo /
    // vpravo), ne nad nej: usetri to 24 px na kazdem parametru a vypada to jako
    // tah faderu na skutecnem panelu, ne jako formular s popiskem.
    inline constexpr float param_trk  = touch;
    inline constexpr float param_gap  = 10.f;
    inline constexpr float param_h    = param_trk + param_gap;
    // Na kolik smi pas stlacit, kdyz se blok nevejde (CONVOLVER ma ctyri
    // parametry a nad nimi jeste podzalozky). Slider se ovlada TAHEM, ne
    // klepnutim, takze mu nevadi klesnout pod plny dotykovy cil — trefit se
    // do pasu pres celou sirku je snadne i kdyz je nizsi.
    inline constexpr float param_trk_min = 52.f;
    inline constexpr float param_h_min   = param_trk_min + param_gap;

    // Mezery.
    inline constexpr float gap    = 10.f;
    inline constexpr float gap_s  = 6.f;

    // Metry a bary.
    inline constexpr float bar_h  = 14.f;   // sustain / peak
    inline constexpr float tick   = 2.f;    // ryska prahu

    // Kolik pixelu musi prst ujet, nez se z klepnuti stane tah. Bez tohoto
    // prahu by kazde klepnuti bylo mikrotazeni a hodnota by uskocila.
    inline constexpr float drag_slop = 8.f;

    // Nejuzsi bunka roztazeneho radku, pod kterou uz se radek radeji zalomi.
    // Neni to dotykovy cil (ten je `touch`) — je to mez citelnosti: uzsi pole
    // uz neuveze ani kratky popisek.
    inline constexpr float cell_min = 56.f;
}

// -- Roztazeny radek --------------------------------------------------------
// Rozdeli vodorovny pas na n STEJNE sirokych bunek a vrati jejich souradnice.
//
// Proc vubec: kdyz si kazdy prvek meri sirku z vlastniho textu, radky pod sebou
// konci jinde a panel vypada jako sazba na psacim stroji. Stejne siroke bunky
// drzi svislice zarovnane napric celou strankou a navic je kazdy cil stejne
// velky — na dotyku se pak netrefujes podle delky popisku.
//
// Kdyz by bunka klesla pod Dims::cell_min, radek se zalomi do vic radek.
// Na cilovem panelu 1280x720 se to nestane; je to pojistka pro okno na PC,
// ktere jde zmensit.
struct Row {
    ImVec2 origin{};                 // levy horni roh celeho pasu
    float  cell = 0.f;               // sirka jedne bunky
    float  cell_h = 0.f;             // vyska jedne bunky
    float  gap = 0.f;
    int    per_row = 1;              // kolik bunek na jednu radku
    int    rows = 1;

    // Levy horni roh i-te bunky.
    ImVec2 at(int i) const {
        const int r = (per_row > 0) ? i / per_row : 0;
        const int c = (per_row > 0) ? i % per_row : i;
        return ImVec2(origin.x + (float)c * (cell + gap),
                      origin.y + (float)r * (cell_h + gap));
    }
    // Pravy dolni roh i-te bunky.
    ImVec2 end(int i) const {
        const ImVec2 p = at(i);
        return ImVec2(p.x + cell, p.y + cell_h);
    }
    // Celkova vyska pasu vcetne zalomeni (bez koncove mezery).
    float height() const { return (float)rows * cell_h + (float)(rows - 1) * gap; }
};

// Strana ctvercove dlazdice hlavniho menu: SIRKA bunky, ne vyska radku.
// Zalozky jsou hlavni mechanismus prepinani, takze dostavaji nejvetsi plochu
// na panelu — ctverec se odviji od delsi (vodorovne) osy, ne od kratsi.
// Strop je tab_h_max a ctvrtina vysky displeje, aby v sirokem okne na PC
// nesnedly celou obrazovku.
inline float squareTabH(float w, int n, float lcd_h) {
    if (n <= 0) return Dims::tab_h_max;
    const float cell = (w - Dims::tab_gap * (float)(n - 1)) / (float)n;
    return std::min(std::min(cell, Dims::tab_h_max), lcd_h * 0.26f);
}

inline Row splitRow(ImVec2 origin, float w, float cell_h, int n,
                    float gap = Dims::gap_s) {
    Row r;
    r.origin = origin;
    r.cell_h = cell_h;
    r.gap    = gap;
    if (n <= 0) { r.per_row = 0; r.rows = 0; r.cell = w; return r; }

    // Kolik se jich vejde, aniz by bunka klesla pod cell_min.
    int per = (int)std::floor((w + gap) / (Dims::cell_min + gap));
    per = std::clamp(per, 1, n);
    r.per_row = per;
    r.rows    = (n + per - 1) / per;
    r.cell    = (w - gap * (float)(per - 1)) / (float)per;
    return r;
}

} // namespace ithaca::gui::layout
