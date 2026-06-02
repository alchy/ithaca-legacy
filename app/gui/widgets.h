#pragma once
// app/gui/widgets.h — Art Deco widget vrstva. Vzor icr2 player/gui/widgets.h,
// adaptovano na nase tokeny (stribro/zlato) a nase indikatory (sustain/peak
// vodorovne, half-pedal ryska, 88-key klaviatura, grid rysky).
#include "theme.h"
#include "layout.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <string>

namespace ithaca::gui::wdg {
using theme::Colors;
using theme::Fonts;

// Eyebrow: maly prostrkany uppercase popisek.
inline void Eyebrow(const char* t, ImU32 col = Colors::muted) {
    if (Fonts::eyebrow) ImGui::PushFont(Fonts::eyebrow);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(col));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    if (Fonts::eyebrow) ImGui::PopFont();
}

// StatTile: eyebrow popisek nad velkou hodnotou. value_col rozhoduje zlato/stribro.
// align = 0 (vlevo) / 0.5 (na stred) / 1 (vpravo) — zarovnani OBOU radku v ramci
// bunky (sirka = GetContentRegionAvail). margin = horizontalni odsazeni od kraju
// (drzi krajni dlazdice u okraje, ale ne nalepene). Pri align=0.5 se margin rusi
// (presny stred). Tim lze tri dlazdice roztahnout: vlevo | stred | vpravo.
inline void StatTile(const char* label, const char* value, ImU32 value_col,
                     float align = 0.f, float margin = 0.f) {
    const float cell_w = ImGui::GetContentRegionAvail().x;
    const float startx = ImGui::GetCursorPosX();
    auto place = [&](float tw) {
        ImGui::SetCursorPosX(startx + margin + (cell_w - 2.f*margin - tw) * align);
    };
    // Radek 1: eyebrow popisek (tlumeny).
    if (Fonts::eyebrow) ImGui::PushFont(Fonts::eyebrow);
    place(ImGui::CalcTextSize(label).x);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (Fonts::eyebrow) ImGui::PopFont();
    // Radek 2: velka hodnota.
    if (Fonts::value) ImGui::PushFont(Fonts::value);
    place(ImGui::CalcTextSize(value).x);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(value_col));
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    if (Fonts::value) ImGui::PopFont();
}

// Lampa: ● rozsvicena (col) / ○ zhasla (muted).
inline void Lamp(const char* label, bool on, ImU32 on_col = Colors::gold) {
    if (Fonts::eyebrow) ImGui::PushFont(Fonts::eyebrow);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(on ? on_col : Colors::muted));
    ImGui::Text("%s %s", on ? "\xE2\x97\x8F" : "\xE2\x97\x8B", label); // ● / ○
    ImGui::PopStyleColor();
    if (Fonts::eyebrow) ImGui::PopFont();
}

// ON/OFF prepinac (chip). Vraci true kdyz uzivatel kliknul (volajici prepne stav).
inline bool ToggleChip(const char* id, bool on) {
    if (Fonts::eyebrow) ImGui::PushFont(Fonts::eyebrow);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(on ? Colors::gold : Colors::muted));
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Colors::v(Colors::line_soft));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Colors::v(Colors::line));
    char buf[32]; std::snprintf(buf, sizeof(buf), "%s  %s##%s",
                                on ? "\xE2\x97\x8F" : "\xE2\x97\x8B",
                                on ? "ON" : "OFF", id);   // ● ON / ○ OFF
    bool clicked = ImGui::Button(buf);
    ImGui::PopStyleColor(4);
    if (Fonts::eyebrow) ImGui::PopFont();
    return clicked;
}

// ParamSlider: eyebrow label + plnosirkovy slider.
inline bool ParamSliderF(const char* label, float* v, float lo, float hi,
                         const char* fmt = "%.2f") {
    Eyebrow(label);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    return ImGui::SliderFloat((std::string("##") + label).c_str(), v, lo, hi, fmt);
}

// Deco slider: tenka linka (track) + mala zarazka (grab) + hodnota vpravo.
// Kresleny pres ImDrawList (NE defaultni tlusty ImGui slider). grab_col rozhoduje
// zlato/stribro. Plnosirkovy. Vraci true kdyz se meni.
// enabled=false → read-only: stejny vzhled, ztlumene barvy, bez interakce
// (pro init-only parametry jako MAX RESONANCE).
inline bool DecoSlider(const char* label, float* v, float lo, float hi,
                       const char* fmt = "%.2f",
                       ImU32 grab_col = Colors::silver2,
                       bool enabled = true) {
    using namespace ithaca::gui::layout;
    const float row_h   = Dims::slider_h;
    const float track_h = Dims::slider_track;
    const float grab_h  = Dims::slider_grab;
    const float grab_w  = 3.f;

    const float width = ImGui::GetContentRegionAvail().x;
    ImVec2 o = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();

    // Ztlumene barvy v read-only rezimu (init-only parametr).
    const ImU32 fill_col  = enabled ? grab_col      : Colors::line;
    const ImU32 grab_dot  = enabled ? Colors::ink    : Colors::muted;
    const ImU32 value_col = enabled ? Colors::ink    : Colors::muted;

    // Label (eyebrow, vlevo) + hodnota (vpravo) na horni linii radku.
    if (Fonts::eyebrow) ImGui::PushFont(Fonts::eyebrow);
    dl->AddText(ImVec2(o.x, o.y), Colors::muted, label);
    char buf[32]; std::snprintf(buf, sizeof(buf), fmt, *v);
    float tw = ImGui::CalcTextSize(buf).x;
    dl->AddText(ImVec2(o.x + width - tw, o.y), value_col, buf);
    if (Fonts::eyebrow) ImGui::PopFont();

    // Track linie ve spodni tretine radku.
    float track_y = o.y + row_h - grab_h * 0.5f - 2.f;
    dl->AddRectFilled(ImVec2(o.x, track_y - track_h*0.5f),
                      ImVec2(o.x + width, track_y + track_h*0.5f),
                      Colors::line_soft);
    // Vyplnena cast po grab.
    float t = (hi > lo) ? (*v - lo) / (hi - lo) : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    float gx = o.x + width * t;
    dl->AddRectFilled(ImVec2(o.x, track_y - track_h*0.5f),
                      ImVec2(gx, track_y + track_h*0.5f), fill_col);
    // Zarazka (svisla).
    dl->AddRectFilled(ImVec2(gx - grab_w*0.5f, track_y - grab_h*0.5f),
                      ImVec2(gx + grab_w*0.5f, track_y + grab_h*0.5f),
                      grab_dot);

    // Read-only: jen rezervuj misto (Dummy), zadna interakce.
    if (!enabled) {
        ImGui::SetCursorScreenPos(o);
        ImGui::Dummy(ImVec2(width, row_h));
        return false;
    }
    // Interakce: invisible button pres cely radek, drag nastavuje hodnotu.
    ImGui::SetCursorScreenPos(o);
    ImGui::InvisibleButton((std::string("##ds_")+label).c_str(), ImVec2(width, row_h));
    bool changed = false;
    if (ImGui::IsItemActive()) {
        float mx = ImGui::GetIO().MousePos.x;
        float nt = std::clamp((mx - o.x) / width, 0.f, 1.f);
        float nv = lo + nt * (hi - lo);
        if (nv != *v) { *v = nv; changed = true; }
    }
    return changed;
}

// Vodorovny gradient fill bar s volitelnou ryskou. Pouziti: sustain (jeden,
// threshold ryska), peak L/R (dva, bez rysky). Kresli na aktualni cursor,
// rezervuje (width x h). width<=0 → cela dostupna sirka.
inline void HBar(float frac01, float width, float h,
                 ImU32 fill_lo, ImU32 fill_hi,
                 float tick01 = -1.f) {
    frac01 = std::clamp(frac01, 0.f, 1.f);
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, h));
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + width, o.y + h), Colors::line_soft);
    if (frac01 > 0.f)
        dl->AddRectFilledMultiColor(
            o, ImVec2(o.x + width * frac01, o.y + h),
            fill_lo, fill_hi, fill_hi, fill_lo); // horizontalni gradient
    if (tick01 >= 0.f) {
        float tx = o.x + width * std::clamp(tick01, 0.f, 1.f);
        dl->AddLine(ImVec2(tx, o.y - 2.f), ImVec2(tx, o.y + h + 2.f),
                    Colors::silver2, 1.f);
    }
}

// 88-key klaviatura (MIDI 21..108). active(midi)=primarni hlas (ZLATA),
// resonating(midi)=sympaticka rezonance (STRIBRNA, sekundarni). active ma
// prednost (kdyz nota hraje i rezonuje, je zlata). Kresli do (width x height).
template <typename ActiveFn, typename ResoFn>
inline void Keyboard(float width, float height, ActiveFn active, ResoFn resonating) {
    constexpr int FIRST = 21, LAST = 108;
    auto is_black = [](int m){ int n=m%12; return n==1||n==3||n==6||n==8||n==10; };
    int n_white = 0; for (int m=FIRST;m<=LAST;++m) if(!is_black(m)) ++n_white;
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    float kw = width / (float)n_white, bw = kw*0.6f, bh = height*0.62f;
    auto* dl = ImGui::GetWindowDrawList();
    int wi = 0;
    // Rezonujici noty: jen lehce svetlejsi nez podklad klavesy (decentni "zar",
    // ne druhy vyrazny highlight) — aby nekonkurovaly aktivnim (zlatym) hlasum.
    constexpr ImU32 kWhiteBase = IM_COL32(0x1c,0x20,0x24,255);
    constexpr ImU32 kWhiteReso = IM_COL32(0x2a,0x30,0x36,255);  // o malo svetlejsi
    for (int m=FIRST;m<=LAST;++m){ if(is_black(m)) continue;
        float kx=p.x+wi*kw;
        ImU32 col = active(m)     ? Colors::gold
                  : resonating(m) ? kWhiteReso
                                  : kWhiteBase;
        dl->AddRectFilled({kx,p.y},{kx+kw-1.f,p.y+height},col);
        dl->AddLine({kx+kw-1.f,p.y},{kx+kw-1.f,p.y+height},Colors::line,0.5f);
        ++wi; }
    wi = 0;
    // Cerna klapka je sama tmava, takze rezonancni seda musi byt VYRAZNEJSI nez
    // u bile klapky (kWhiteReso 0x2a3036) — jinak na tmavem podkladu splyne.
    // Volime o krok svetlejsi sedou, aby byla cerna rezonujici klapka citelna.
    constexpr ImU32 kBlackReso = IM_COL32(0x3c,0x44,0x4c,255);
    for (int m=FIRST;m<=LAST;++m){ if(!is_black(m)){++wi;continue;}
        float kx=p.x+(wi-1)*kw+kw-bw*0.5f;
        ImU32 col = active(m)     ? IM_COL32(0x8a,0x73,0x30,255)  // zlata-tmava
                  : resonating(m) ? kBlackReso
                                  : Colors::bg;
        dl->AddRectFilled({kx,p.y},{kx+bw,p.y+bh},col); }
}

} // namespace ithaca::gui::wdg
