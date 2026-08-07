// app/gui/page_play.cpp — PLAY: stav nastroje a zaroven prohlizec bank.
//
// Vytah: aktualni banka uprostred inverznim polem, sousedi nad a pod tlumene.
// Sipky nejsou — listuje se klepnutim na radek nebo tahem. Dulezite:
//
//   klepnuti na radek  → nacte tu banku hned
//   tah                → jen posouva; po pusteni dojede tlumenym dobehem
//                        na nejblizsi radek a TEPRVE PO USTALENI nacte
//
// Duvod pro to zpozdeni: nacteni banky trva vteriny a kryje ho modalni overlay.
// Kdyby se nacitalo behem rolovani, projeti seznamu by spustilo desitky loadu
// za sebou a z listovani by bylo peklo.
#include "pages.h"
#include "app_context.h"
#include "widgets.h"
#include "theme.h"
#include "layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ithaca::gui {

namespace L = ithaca::gui::layout;
using theme::Colors;
using theme::Fonts;

namespace {

// Jak casto se cisla vubec prekresluji. Kazdy snimek je zbytecne — hodnota se
// meni rychleji, nez ji stihnes precist. Osmkrat za vterinu je kompromis:
// cislo se da precist a zaroven ma stejnou zivost jako vlna v pozadi, takze
// se ta dve na obrazovce nerozchazi.
constexpr float kHoldWin = 0.125f;

// Kolik bunek ma stavovy radek. MIDI lampy odesly do paticky mezi ostatni
// indikatory (screen.cpp::lampRow), takze zbylo sedm cisel. Compact profil je
// sazi do dvou radek (4 + 3), na sirokem panelu jsou v jedne.
constexpr int kStatCells = 7;

// Drzi MAXIMUM za okno. Spravne pro veliciny, u kterych je zajimava spicka:
// pocet hlasu, zatez DSP, peak metr.
float holdMax(PanelState::Hold& s, float cur, float now_s, float win = kHoldWin) {
    if (cur > s.winmax) s.winmax = cur;
    if (now_s - s.t0 >= win) { s.shown = s.winmax; s.winmax = 0.f; s.t0 = now_s; }
    return s.shown;
}

// Drzi POSLEDNI hodnotu na konci okna. Pro polohu pedalu: max-hold by po
// pusteni jeste pul vteriny ukazoval 127, coz je u udaje, ktery se ma cist
// presne, horsi nez pomalejsi obnova.
float holdLast(PanelState::Hold& s, float cur, float now_s, float win = kHoldWin) {
    s.winmax = cur;
    if (now_s - s.t0 >= win) { s.shown = cur; s.t0 = now_s; }
    return s.shown;
}

float toDb(float lin) { return lin < 1e-6f ? -120.f : 20.f * std::log10(lin); }

// Jedno cislo se svym popiskem.
void statNum(ImDrawList* dl, ImVec2 pos, const char* label, const char* value,
             ImU32 col = Colors::ink) {
    const float lp = wdg::fontPx(Fonts::small);
    const float np = wdg::fontPx(Fonts::num);
    dl->AddText(Fonts::small, lp, pos, Colors::dimmer, label);
    dl->AddText(Fonts::num, np, ImVec2(pos.x, pos.y + lp + 4.f), col, value);
}

} // namespace

void pagePlay(AppContext& ctx, const Rect& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& ps = ctx.panels;
    const float dt = ImGui::GetIO().DeltaTime;
    const float now_s = (float)ImGui::GetTime();

    // -- Rozvrzeni: vytah nahore, cisla dole ------------------------------
    // Na uzkem panelu se sedm sloupcu do sirky nevejde (744/7 = 106 px, ale
    // "128/256" ve stavovem pismu ma 117 px), takze je Compact profil sazi do
    // dvou radek. Vytah se o ten radek musi zkratit, jinak do nej zaroste.
    const int   cols   = std::min(L::g_screen.stat_cols, kStatCells);
    const int   rows   = (kStatCells + cols - 1) / cols;
    const float line_h = wdg::fontPx(Fonts::num) + wdg::fontPx(Fonts::small) + 4.f;
    // Pro jednu radku zustava puvodnich 110 px i s rezervou nad cisly.
    const float stat_h = 110.f + (float)(rows - 1) * (line_h + L::Dims::gap_s);
    const Rect reel_r{ r.lo, ImVec2(r.hi.x, r.hi.y - stat_h) };

    const int n = (int)ps.banks.size();
    const float row = L::Dims::row_h;
    // Vybrany nastroj sedi na stredu OBRAZOVKY, ne na stredu plochy vytahu:
    // ta je nesymetricka (lista nahore vyssi nez paticka dole, spodek ukrajuje
    // rada udaju), takze by pri kazde zmene velikosti okna utikal nahoru.
    // Clamp drzi radek uvnitr vyrezu i na velmi malem okne.
    //
    // Na uzkem panelu to ale neplati: plocha vytahu je tam tak nizka, ze stred
    // OBRAZOVKY padne az na jeji spodni hranu a videt je jedina polozka —
    // z vytahu se stane cedule. Compact profil proto stredi podle vlastni
    // plochy, aby byl videt soused nad i pod.
    const float half = row * 0.5f;
    const float want_y = (!L::g_screen.compact && ps.lcd_center_y > 0.f)
                       ? ps.lcd_center_y
                       : (reel_r.lo.y + reel_r.hi.y) * 0.5f;
    const float mid_y = std::clamp(want_y, reel_r.lo.y + half, reel_r.hi.y - half);
    // Vlna v pozadi protéká stredem vybraneho patche — osu si bere odsud.
    ps.scope_center_y = mid_y;

    // -- Interakce --------------------------------------------------------
    ImGui::SetCursorScreenPos(reel_r.lo);
    ImGui::InvisibleButton("##reel", ImVec2(reel_r.w(), reel_r.h()));
    const bool active = ImGui::IsItemActive();

    if (n > 0) {
        if (active) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left,
                                                      L::Dims::drag_slop);
            if (!ps.reel_dragging && (d.y != 0.f)) {   // az za prahem tahu
                ps.reel_dragging = true;
                ps.reel_grab0 = ps.reel.pos;
            }
            if (ps.reel_dragging) {
                // Tah dolu ukazuje predchozi polozky → posun opacnym smerem.
                ps.reel.pos = ps.reel_grab0 - d.y;
                ps.reel.vel = 0.f;
                ps.reel.target = ps.reel.pos;
            }
        } else if (ps.reel_dragging) {
            // Pusteno: dojed na nejblizsi radek a po ustaleni nacti.
            ps.reel_dragging = false;
            const float idx = std::clamp(std::round(ps.reel.pos / row), 0.f, (float)(n - 1));
            ps.reel.target = idx * row;
            ps.reel_armed = true;
        }

        // Klepnuti (bez tahu) na konkretni radek → rovnou tam a nacist.
        if (ImGui::IsItemDeactivated() && !ps.reel_dragging) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left,
                                                       L::Dims::drag_slop);
            if (dd.x == 0.f && dd.y == 0.f) {
                const int delta = (int)std::round((m.y - mid_y) / row);
                const int want = std::clamp(ps.reel_sel + delta, 0, n - 1);
                ps.reel.target = want * row;
                ps.reel_armed = true;
            }
        }
    }

    // Dobeh + spusteni loadu az v okamziku ustaleni.
    const bool just_settled = ps.reel.step(dt);
    if (n > 0)
        ps.reel_sel = std::clamp((int)std::lround(ps.reel.pos / row), 0, n - 1);
    if (just_settled && ps.reel_armed) {
        ps.reel_armed = false;
        const std::string& want = ps.banks[(size_t)ps.reel_sel].dir;
        if (want != ctx.state.bank_path) {       // uz nactenou znovu nenacitame
            ctx.state.bank_path = want;
            ctx.requestBankReload(want);
        }
    }

    // -- Vykresleni vytahu -------------------------------------------------
    dl->PushClipRect(reel_r.lo, reel_r.hi, true);
    if (n == 0) {
        const float px = wdg::fontPx(Fonts::ui);
        const char* msg = "no bank - choose a folder on the BANK page";
        dl->AddText(Fonts::ui, px,
                    ImVec2(reel_r.lo.x, mid_y - px * 0.5f), Colors::dimmer, msg);
    }
    for (int i = 0; i < n; ++i) {
        const float y = mid_y - (ps.reel.pos - i * row) - row * 0.5f;
        if (y > reel_r.hi.y || y + row < reel_r.lo.y) continue;   // mimo vyrez
        const int   dist = std::abs(i - ps.reel_sel);
        const bool  cur  = (dist == 0);
        const auto& e    = ps.banks[(size_t)i];

        if (cur) {
            const float px = wdg::fontPx(Fonts::big);
            dl->AddRectFilled(ImVec2(reel_r.lo.x, y),
                              ImVec2(reel_r.hi.x, y + row), Colors::inv_bg);
            dl->AddText(Fonts::big, px, ImVec2(reel_r.lo.x + 10.f,
                        y + (row - px) * 0.5f), Colors::inv_fg, e.name.c_str());
        } else {
            const float px = wdg::fontPx(Fonts::ui);
            const ImU32 c = (dist == 1) ? Colors::dim : Colors::dimmer;
            dl->AddText(Fonts::ui, px, ImVec2(reel_r.lo.x + 10.f,
                        y + (row - px) * 0.5f), c, e.name.c_str());
        }
    }
    dl->PopClipRect();

    // Vodici linky vyrezu — at je videt, ze jde o rolovaci pole.
    dl->AddLine(ImVec2(reel_r.lo.x, mid_y - row * 0.5f),
                ImVec2(reel_r.hi.x, mid_y - row * 0.5f), Colors::line);
    dl->AddLine(ImVec2(reel_r.lo.x, mid_y + row * 0.5f),
                ImVec2(reel_r.hi.x, mid_y + row * 0.5f), Colors::line);

    // -- Stav --------------------------------------------------------------
    // Osm stejnych sloupcu na spolecne uctare. Stav streamovacich ringu je
    // tady, ne na SYS: odecita se PRI HRANI (podle nej se pozna, ze banka
    // nestiha z disku), a SYS je stranka na nastavovani, kam se za hrani
    // neleze. Popisky jsou zkracene — na osm sloupcu uz neni misto na
    // "MAIN RINGS", vyznam je v dokumentaci.
    // Radek se kotvi u SPODNI hrany plochy, ne pevnym odsazenim od zacatku
    // pasma: drzi se tak dole u paticky misto aby plaval uprostred volneho
    // mista pod vytahem.
    const float sy = r.hi.y - 16.f - (float)rows * line_h
                                   - (float)(rows - 1) * L::Dims::gap_s;

    // Levy horni roh i-te bunky stavu.
    const float col_w = r.w() / (float)cols;
    auto cell_at = [&](int i) {
        return ImVec2(r.lo.x + col_w * (float)(i % cols),
                      sy + (line_h + L::Dims::gap_s) * (float)(i / cols));
    };

    char v[16], rs[16], pk[16], ds[16], su[16], rg[24], rgr[24];
    std::snprintf(v,  sizeof(v),  "%d", (int)holdMax(ps.h_voices,
                  (float)ctx.engine.activeVoices(), now_s));
    std::snprintf(rs, sizeof(rs), "%d", (int)holdMax(ps.h_reso,
                  (float)ctx.engine.resonanceVoices(), now_s));
    // Peak metr drzi spicku za okno — presne to, co peak metr ma delat.
    std::snprintf(pk, sizeof(pk), "%.1f",
                  toDb(holdMax(ps.h_peak,
                       std::max(ctx.engine.masterPeakL(), ctx.engine.masterPeakR()),
                       now_s)));
    std::snprintf(ds, sizeof(ds), "%.0f%%",
                  holdMax(ps.h_load, ctx.engine.dspLoadPeak(), now_s) * 100.f);
    std::snprintf(su, sizeof(su), "%d",
                  (int)holdLast(ps.h_sustain, (float)ctx.engine.pedalCC(), now_s));
    std::snprintf(rg,  sizeof(rg),  "%d/%d",
                  (int)holdMax(ps.h_main_rings, (float)ctx.engine.mainRingsUsed(), now_s),
                  ctx.engine.mainRingsTotal());
    std::snprintf(rgr, sizeof(rgr), "%d/%d",
                  (int)holdMax(ps.h_reso_rings, (float)ctx.engine.resonanceRingsUsed(), now_s),
                  ctx.engine.resonanceRingsTotal());

    statNum(dl, cell_at(0), "VOICES",    v);
    statNum(dl, cell_at(1), "RESO",      rs);
    statNum(dl, cell_at(2), "RING",      rg);
    statNum(dl, cell_at(3), "RING RESO", rgr);
    statNum(dl, cell_at(4), "PEAK dB",   pk);
    statNum(dl, cell_at(5), "DSP",       ds,
            ctx.engine.overloadRecent(4000.f) ? Colors::warn : Colors::ink);
    // Pedal uz nema vlastni bar — jeho INDIKACE je pata stuha v pozadi
    // (viz screen.cpp). Tady zustava jen cislo, protoze udaj se ma cist presne.
    statNum(dl, cell_at(6), "SUSTAIN",   su);
    // MIDI lampy NOTE/OFF sedi v paticce vedle UNDERRUN, CLIP a LOG
    // (screen.cpp::lampRow) — je to tentyz druh indikatoru, stav pristroje,
    // a patri tedy na kazdou stranku, ne jen sem.
}

} // namespace ithaca::gui
