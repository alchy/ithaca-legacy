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

// Sample-and-hold: drzi maximum za okno, jinak cisla pri 60 fps neblikaji
// jen necitelne — primo se nedaji precist.
float holdMax(PanelState::Hold& s, float cur, float now_s, float win = 0.4f) {
    if (cur > s.winmax) s.winmax = cur;
    if (now_s - s.t0 >= win) { s.shown = s.winmax; s.winmax = 0.f; s.t0 = now_s; }
    return s.shown;
}

float toDb(float lin) { return lin < 1e-6f ? -120.f : 20.f * std::log10(lin); }
float dbTo01(float db) { return std::clamp((db + 60.f) / 60.f, 0.f, 1.f); }

// Jedno cislo se svym popiskem.
void statNum(ImDrawList* dl, ImVec2 pos, const char* label, const char* value,
             ImU32 col = Colors::ink) {
    const float lp = wdg::fontPx(Fonts::small);
    const float np = wdg::fontPx(Fonts::num);
    dl->AddText(Fonts::small, lp, pos, Colors::dimmer, label);
    dl->AddText(Fonts::num, np, ImVec2(pos.x, pos.y + lp + 4.f), col, value);
}

// Nabere novou stopu z enginu a vykresli celou historii s klesajici sytosti.
// Vzorky se sbaluji do kPts bodu pres MIN/MAX v kazdem kosi — prumer by
// u zvukoveho signalu vykrátil spicky a stopa by vypadala mrtve.
void drawScope(AppContext& ctx, ImDrawList* dl, ImVec2 pos, float w, float h) {
    auto& sc = ctx.panels.scope;
    static float raw_l[ithaca::Engine::kScopeSize];
    static float raw_r[ithaca::Engine::kScopeSize];
    constexpr int kN = ithaca::Engine::kScopeSize;
    ctx.engine.scopeSnapshot(raw_l, raw_r, kN);

    // Nova stopa na pozici head.
    float* dl_ = sc.l[sc.head];
    float* dr_ = sc.r[sc.head];
    const int bucket = kN / PanelState::Scope::kPts;
    for (int i = 0; i < PanelState::Scope::kPts; ++i) {
        float mx_l = 0.f, mx_r = 0.f;
        for (int k = 0; k < bucket; ++k) {
            const float a = raw_l[i * bucket + k];
            const float b = raw_r[i * bucket + k];
            if (std::fabs(a) > std::fabs(mx_l)) mx_l = a;
            if (std::fabs(b) > std::fabs(mx_r)) mx_r = b;
        }
        dl_[i] = mx_l;
        dr_[i] = mx_r;
    }
    sc.head = (sc.head + 1) % PanelState::Scope::kGhosts;
    if (sc.count < PanelState::Scope::kGhosts) ++sc.count;

    // Ramecek vyrezu + osa.
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), Colors::line);
    dl->AddLine(ImVec2(pos.x, pos.y + h * 0.5f),
                ImVec2(pos.x + w, pos.y + h * 0.5f), Colors::trough);

    dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
    // Od nejstarsi k nejnovejsi: starsi slabsi a tencí.
    for (int g = 0; g < sc.count; ++g) {
        const int idx = (sc.head - 1 - g + PanelState::Scope::kGhosts * 2)
                        % PanelState::Scope::kGhosts;
        const float age = (float)g / (float)PanelState::Scope::kGhosts;
        const float a   = (1.f - age) * (1.f - age);      // kvadraticky dosvit
        wdg::scopeTrace(dl, pos, w, h, sc.r[idx], PanelState::Scope::kPts,
                        Colors::fill, a * 0.55f, 1.f);
        wdg::scopeTrace(dl, pos, w, h, sc.l[idx], PanelState::Scope::kPts,
                        Colors::inv_bg, a * 0.75f, g == 0 ? 1.8f : 1.f);
    }
    dl->PopClipRect();
}

} // namespace

void pagePlay(AppContext& ctx, const Rect& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& ps = ctx.panels;
    const float dt = ImGui::GetIO().DeltaTime;
    const float now_s = (float)ImGui::GetTime();

    // -- Rozvrzeni: vytah nahore, cisla + sustain dole --------------------
    const float stat_h = 96.f;
    const Rect reel_r{ r.lo, ImVec2(r.hi.x, r.hi.y - stat_h) };

    const int n = (int)ps.banks.size();
    const float row = L::Dims::row_h;
    const float mid_y = (reel_r.lo.y + reel_r.hi.y) * 0.5f;

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
    const float sy = r.hi.y - stat_h + 8.f;
    char v[16], rs[16], pk[16], ds[16];
    std::snprintf(v,  sizeof(v),  "%d", (int)holdMax(ps.h_voices,
                  (float)ctx.engine.activeVoices(), now_s));
    std::snprintf(rs, sizeof(rs), "%d", (int)holdMax(ps.h_reso,
                  (float)ctx.engine.resonanceVoices(), now_s));
    std::snprintf(pk, sizeof(pk), "%.1f",
                  toDb(std::max(ctx.engine.masterPeakL(), ctx.engine.masterPeakR())));
    std::snprintf(ds, sizeof(ds), "%.0f%%",
                  holdMax(ps.h_load, ctx.engine.dspLoadPeak(), now_s) * 100.f);

    const float col = r.w() / 5.f;
    statNum(dl, ImVec2(r.lo.x,             sy), "VOICES", v);
    statNum(dl, ImVec2(r.lo.x + col,       sy), "RESO",   rs);
    statNum(dl, ImVec2(r.lo.x + col * 2.f, sy), "PEAK dB", pk);
    statNum(dl, ImVec2(r.lo.x + col * 3.f, sy), "DSP",    ds,
            ctx.engine.overloadRecent(4000.f) ? Colors::error : Colors::ink);

    // NOTE lampy + SUSTAIN. Pedal je spojity 0-127 s prahem half-pedalu —
    // u piana zasadni zpetna vazba, proto ma vlastni bar a ne jen cislo.
    const float bx = r.lo.x + col * 4.f;
    const float lp = wdg::fontPx(Fonts::small);
    wdg::lamp(dl, ImVec2(bx, sy), "NOTE", ctx.engine.noteOnRecent(120.f), Colors::inv_bg);
    wdg::lamp(dl, ImVec2(bx + wdg::lampW("NOTE"), sy), "OFF",
              ctx.engine.noteOffRecent(120.f), Colors::dim);

    // Osciloskop misto sloupcoveho metru: ukazuje skutecny prubeh L a R
    // s dosvitem. Sirku si bere prvni, sustain bar pak konci pred nim.
    const float scope_w = 300.f, scope_h = 62.f;
    const float meters_x = r.hi.x - scope_w;
    drawScope(ctx, dl, ImVec2(meters_x, sy), scope_w, scope_h);
    dl->AddText(Fonts::small, lp, ImVec2(meters_x, sy + scope_h + 3.f),
                Colors::dimmer, "L \xC2\xB7 R");

    const int cc = (int)ctx.engine.pedalCC();
    char sus[24]; std::snprintf(sus, sizeof(sus), "SUSTAIN %d", cc);
    dl->AddText(Fonts::small, lp, ImVec2(bx, sy + lp + 8.f), Colors::dimmer, sus);
    wdg::hbar(dl, ImVec2(bx, sy + lp * 2.f + 12.f),
              std::max(40.f, meters_x - 16.f - bx), L::Dims::bar_h,
              (float)cc / 127.f, 0.5f);
}

} // namespace ithaca::gui
