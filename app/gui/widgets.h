#pragma once
// app/gui/widgets.h — Art Deco widget vrstva. Vzor icr2 player/gui/widgets.h,
// adaptovano na nase tokeny (stribro/zlato) a nase indikatory (sustain/peak
// vodorovne, half-pedal ryska, 88-key klaviatura, grid rysky).
#include "theme.h"
#include "imgui.h"
#include <algorithm>
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
inline void StatTile(const char* label, const char* value, ImU32 value_col) {
    Eyebrow(label);
    if (Fonts::value) ImGui::PushFont(Fonts::value);
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

// ParamSlider: eyebrow label + plnosirkovy slider.
inline bool ParamSliderF(const char* label, float* v, float lo, float hi,
                         const char* fmt = "%.2f") {
    Eyebrow(label);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    return ImGui::SliderFloat((std::string("##") + label).c_str(), v, lo, hi, fmt);
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

// Kratka zlata grid ryska na krizeni (absolutni screen coords).
inline void GridTick(float screen_x, float screen_y, float len = 9.f) {
    ImGui::GetForegroundDrawList()->AddLine(
        ImVec2(screen_x, screen_y), ImVec2(screen_x, screen_y + len),
        Colors::gold, 1.f);
}

// 88-key klaviatura (MIDI 21..108). active(midi)->bool urcuje rozsviceni.
// Aktivni klavesa zlata. Kresli do (width x height) na aktualni cursor.
template <typename ActiveFn>
inline void Keyboard(float width, float height, ActiveFn active) {
    constexpr int FIRST = 21, LAST = 108;
    auto is_black = [](int m){ int n=m%12; return n==1||n==3||n==6||n==8||n==10; };
    int n_white = 0; for (int m=FIRST;m<=LAST;++m) if(!is_black(m)) ++n_white;
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    float kw = width / (float)n_white, bw = kw*0.6f, bh = height*0.62f;
    auto* dl = ImGui::GetWindowDrawList();
    int wi = 0;
    for (int m=FIRST;m<=LAST;++m){ if(is_black(m)) continue;
        float kx=p.x+wi*kw; ImU32 col = active(m) ? Colors::gold : IM_COL32(0x1c,0x20,0x24,255);
        dl->AddRectFilled({kx,p.y},{kx+kw-1.f,p.y+height},col);
        dl->AddLine({kx+kw-1.f,p.y},{kx+kw-1.f,p.y+height},Colors::line,0.5f);
        ++wi; }
    wi = 0;
    for (int m=FIRST;m<=LAST;++m){ if(!is_black(m)){++wi;continue;}
        float kx=p.x+(wi-1)*kw+kw-bw*0.5f;
        ImU32 col = active(m) ? IM_COL32(0x8a,0x73,0x30,255) : Colors::bg;
        dl->AddRectFilled({kx,p.y},{kx+bw,p.y+bh},col); }
}

} // namespace ithaca::gui::wdg
