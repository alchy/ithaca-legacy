#pragma once
// app/gui/widgets.h — primitivy znakoveho displeje.
// ----------------------------------------------------------------------------
// Vsechno se kresli pres ImDrawList, ne pres stock ImGui widgety — ty maji
// vlastni vzhled i vlastni chovani (napr. skok na misto kliknuti), ktere se
// s pojetim znakoveho displeje i s dotykem tluce.
//
// Dve pravidla, ktera se tahnou celym souborem:
//   1. Duraz = INVERZNI POLE (plna vypln + tmavy text). Na dotyku neni hover,
//      takze vyber musi byt videt bez najeti; a skutecne znakove displeje
//      zadny tucny rez nemaji.
//   2. Nic interaktivniho pod Dims::touch (74 px). Kdyz je prvek opticky mensi,
//      dotykova zona se zvetsi neviditelne.
#include "theme.h"
#include "layout.h"
#include "motion.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ithaca::gui::wdg {

using theme::Colors;
using theme::Fonts;
namespace L = ithaca::gui::layout;

// -- Text -------------------------------------------------------------------

inline void drawText(ImDrawList* dl, ImFont* f, float px, ImVec2 pos, ImU32 col,
                     const char* txt) {
    dl->AddText(f, px, pos, col, txt);
}

// Sirka textu v danem pismu bez ohledu na aktualne pushnuty font.
inline float textW(ImFont* f, float px, const char* txt) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.f, txt).x;
}

// Velikost, v jake je pismo skutecne renderovano. Pisma jsou rasterizovana ve
// fyzickem rozliseni (size * g_scale) a zobrazovana pres io.FontGlobalScale,
// takze logicka velikost = FontSize * FontGlobalScale.
inline float fontPx(ImFont* f) {
    return f->FontSize * ImGui::GetIO().FontGlobalScale;
}

// -- Inverzni pole ----------------------------------------------------------
// Duraz na znakovem displeji. Vraci obdelnik, ktery pole zabralo.
inline ImVec2 invField(ImDrawList* dl, ImVec2 pos, ImFont* f, const char* txt,
                       float pad_x = 8.f, float h = 0.f,
                       ImU32 bg = Colors::inv_bg, ImU32 fg = Colors::inv_fg) {
    const float px = fontPx(f);
    const float w  = textW(f, px, txt) + pad_x * 2.f;
    if (h <= 0.f) h = px * 1.45f;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), bg);
    dl->AddText(f, px, ImVec2(pos.x + pad_x, pos.y + (h - px) * 0.5f), fg, txt);
    return ImVec2(w, h);
}

// -- Kontrolka --------------------------------------------------------------
// Tmava, dokud stav nenastane. Zhasla kontrolka musi byt videt, aby bylo
// poznat, ze existuje — proto se kresli i zhasla, jen nejtlumeneji.
inline void lamp(ImDrawList* dl, ImVec2 pos, const char* label, bool on,
                 ImU32 on_col = Colors::warn) {
    const float px = fontPx(Fonts::small);
    const float r  = 4.f;
    const ImU32 c  = on ? on_col : Colors::dimmer;
    dl->AddCircleFilled(ImVec2(pos.x + r, pos.y + px * 0.5f), r, c, 12);
    dl->AddText(Fonts::small, px, ImVec2(pos.x + r * 2.f + 6.f, pos.y), c, label);
}

inline float lampW(const char* label) {
    return 8.f + 6.f + textW(Fonts::small, fontPx(Fonts::small), label) + 18.f;
}

// -- Vodorovny bar (sustain, peak) ------------------------------------------
// tick01 < 0 = bez rysky. Ryska znaci prah (napr. half-pedal).
inline void hbar(ImDrawList* dl, ImVec2 pos, float w, float h, float frac01,
                 float tick01 = -1.f, ImU32 fill = Colors::inv_bg) {
    frac01 = std::clamp(frac01, 0.f, 1.f);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), Colors::trough);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), Colors::line);
    if (frac01 > 0.f)
        dl->AddRectFilled(ImVec2(pos.x + 1, pos.y + 1),
                          ImVec2(pos.x + w * frac01, pos.y + h - 1), fill);
    if (tick01 >= 0.f) {
        const float tx = pos.x + w * std::clamp(tick01, 0.f, 1.f);
        dl->AddRectFilled(ImVec2(tx - L::Dims::tick * 0.5f, pos.y - 3.f),
                          ImVec2(tx + L::Dims::tick * 0.5f, pos.y + h + 3.f),
                          Colors::ink);
    }
}

// -- Sloupcovy metr (peak L/R) ----------------------------------------------
inline void vmeter(ImDrawList* dl, ImVec2 pos, float w, float h, float frac01,
                   int segments = 12) {
    frac01 = std::clamp(frac01, 0.f, 1.f);
    const float seg_h = h / (float)segments;
    const int lit = (int)std::lround(frac01 * segments);
    for (int i = 0; i < segments; ++i) {
        const float y1 = pos.y + h - (i + 1) * seg_h + 1.f;
        const float y2 = pos.y + h - i * seg_h - 1.f;
        const bool  on = i < lit;
        // Poslednich ~15 % je jantarove: prehlceni pozna periferni videni
        // podle barvy, ne podle vysky sloupce.
        const ImU32 c = !on ? Colors::trough
                            : (i >= segments - 2 ? Colors::warn : Colors::inv_bg);
        dl->AddRectFilled(ImVec2(pos.x, y1), ImVec2(pos.x + w, y2), c);
    }
}

// -- Vlnova cara (pozadi) ---------------------------------------------------
// Pozadi ve stylu PS3 XMB: mekke cary tekouci pres plochu. Tvar rizne SKUTECNY
// zvuk, ale silne vyhlazeny — syrovy prubeh je zubaty a na pozadi by rusil
// cteni; vyhlazenim zustane reakce na hru, ale pohyb je hedvabny.
//
// Zadna vypln: samotna cara staci a nechava text citelny. Zato je siroka —
// tenka linka by se na pozadi ztratila a pusobila jako grafova mrizka.
//
// base_y je ABSOLUTNI souradnice osy, ne pomer: osa ma prochazet stredem
// vybraneho patche, ktery zna az stranka PLAY.
inline void waveLine(ImDrawList* dl, float x0, float w, float base_y,
                     const float* pts, int n, float amp_px, float gain,
                     ImU32 col, float alpha, float thickness) {
    if (n < 2 || alpha <= 0.004f) return;
    const ImU32 c = (col & 0x00FFFFFF) | ((ImU32)(std::clamp(alpha, 0.f, 1.f) * 255.f) << 24);
    dl->PathClear();
    for (int i = 0; i < n; ++i) {
        const float x = x0 + w * ((float)i / (float)(n - 1));
        // MEKKY limit misto tvrdeho orezu: soucet nosne vlny a modulace zvukem
        // muze presahnout 1.0 a clamp by vrcholy usekl naplocho (bylo videt
        // jako "clipovani"). tanh je ohne, takze tvar zustane hladky.
        // Mekky limit misto tvrdeho orezu. Vstup je konstruovan tak, aby se
        // bezne pohyboval hluboko v LINEARNI oblasti tanh — saturace slouzi
        // jen jako pojistka pro spicky, ne jako bezny rezim.
        const float y = base_y - std::tanh(pts[i] * gain * 0.85f) * amp_px * 1.15f;
        dl->PathLineTo(ImVec2(x, y));
    }
    dl->PathStroke(c, 0, thickness);
}

// -- Dotykovy slider --------------------------------------------------------
// Hodnota se meni JEN tahem, nikdy klepnutim. Stock ImGui slider skace na
// misto kliknuti — pri testovani stare verze to omylem prepsalo prah limiteru
// a na dotyku by to bylo mnohem horsi.
//
// Vraci true, kdyz se hodnota v tomhle framu zmenila.
inline bool paramSlider(const char* id, const char* label, float* v,
                        float lo, float hi, const char* fmt,
                        bool enabled = true) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float  w = ImGui::GetContentRegionAvail().x;
    const float  lab_px = fontPx(Fonts::small);
    const float  val_px = fontPx(Fonts::ui);

    const ImU32 lab_c = enabled ? Colors::dim   : Colors::dimmer;
    const ImU32 val_c = enabled ? Colors::ink   : Colors::dimmer;
    const ImU32 fil_c = enabled ? Colors::fill  : Colors::trough;

    // Popisek vlevo, hodnota vpravo, na spolecne uctare.
    char buf[48];
    std::snprintf(buf, sizeof(buf), fmt, *v);
    dl->AddText(Fonts::small, lab_px, ImVec2(o.x, o.y + 4.f), lab_c, label);
    dl->AddText(Fonts::ui, val_px,
                ImVec2(o.x + w - textW(Fonts::ui, val_px, buf), o.y), val_c, buf);

    // Track.
    const float ty = o.y + L::Dims::param_lab;
    const float th = L::Dims::param_trk;
    dl->AddRectFilled(ImVec2(o.x, ty), ImVec2(o.x + w, ty + th), Colors::trough);
    dl->AddRect(ImVec2(o.x, ty), ImVec2(o.x + w, ty + th), Colors::line);

    const float t = (hi > lo) ? std::clamp((*v - lo) / (hi - lo), 0.f, 1.f) : 0.f;
    if (t > 0.f)
        dl->AddRectFilled(ImVec2(o.x + 1, ty + 1),
                          ImVec2(o.x + w * t, ty + th - 1), fil_c);
    // Zarazka — siroka, at je videt kde presne hodnota je.
    const float gx = o.x + w * t;
    dl->AddRectFilled(ImVec2(gx - 3.f, ty - 2.f), ImVec2(gx + 3.f, ty + th + 2.f),
                      enabled ? Colors::ink : Colors::dimmer);

    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y));
    ImGui::InvisibleButton(id, ImVec2(w, L::Dims::param_lab + th));

    bool changed = false;
    if (enabled && ImGui::IsItemActive()) {
        const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, L::Dims::drag_slop);
        if (d.x != 0.f || d.y != 0.f) {   // az za prahem tahu
            const float mx = ImGui::GetIO().MousePos.x;
            const float nt = std::clamp((mx - o.x) / w, 0.f, 1.f);
            const float nv = lo + nt * (hi - lo);
            if (nv != *v) { *v = nv; changed = true; }
        }
    }
    ImGui::Dummy(ImVec2(0.f, L::Dims::param_gap));
    return changed;
}

// -- ON/OFF prepinac --------------------------------------------------------
inline bool toggle(const char* id, const char* label, bool on) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float px = fontPx(Fonts::small);
    const float w  = textW(Fonts::small, px, label) + 60.f;
    const float h  = L::Dims::touch * 0.62f;   // opticky mensi, zona plna (nize)

    if (on) {
        dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), Colors::inv_bg);
        dl->AddText(Fonts::small, px, ImVec2(o.x + 12.f, o.y + (h - px) * 0.5f),
                    Colors::inv_fg, label);
        dl->AddText(Fonts::small, px,
                    ImVec2(o.x + w - textW(Fonts::small, px, "ON") - 12.f,
                           o.y + (h - px) * 0.5f), Colors::inv_fg, "ON");
    } else {
        dl->AddRect(o, ImVec2(o.x + w, o.y + h), Colors::line);
        dl->AddText(Fonts::small, px, ImVec2(o.x + 12.f, o.y + (h - px) * 0.5f),
                    Colors::dim, label);
        dl->AddText(Fonts::small, px,
                    ImVec2(o.x + w - textW(Fonts::small, px, "OFF") - 12.f,
                           o.y + (h - px) * 0.5f), Colors::dimmer, "OFF");
    }
    ImGui::SetCursorScreenPos(o);
    // Dotykova zona je vyssi nez vykresleny chip.
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, L::Dims::touch));
    return clicked;
}

// -- Radek zalozek ----------------------------------------------------------
// Zvyraznena zalozka je zaroven nadpis stranky, proto stranky nemaji vlastni
// hlavicku. Vraci true pri zmene.
inline bool tabBar(const char* id, const char* const* labels, int n, int& sel,
                   float h = L::Dims::tab_h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float  total = ImGui::GetContentRegionAvail().x;
    const float  gap = 3.f;
    const float  w = (total - gap * (n - 1)) / (float)n;
    const float  px = fontPx(Fonts::small);
    bool changed = false;

    for (int i = 0; i < n; ++i) {
        const ImVec2 p(o.x + i * (w + gap), o.y);
        const ImVec2 q(p.x + w, p.y + h);
        const bool on = (i == sel);
        if (on) dl->AddRectFilled(p, q, Colors::inv_bg);
        else    dl->AddRect(p, q, Colors::line);
        const float tw = textW(Fonts::small, px, labels[i]);
        dl->AddText(Fonts::small, px,
                    ImVec2(p.x + (w - tw) * 0.5f, p.y + (h - px) * 0.5f),
                    on ? Colors::inv_fg : Colors::dim, labels[i]);

        ImGui::SetCursorScreenPos(p);
        char bid[32]; std::snprintf(bid, sizeof(bid), "%s_%d", id, i);
        if (ImGui::InvisibleButton(bid, ImVec2(w, h)) && !on) { sel = i; changed = true; }
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h));
    ImGui::Dummy(ImVec2(total, 0.f));
    return changed;
}

} // namespace ithaca::gui::wdg
