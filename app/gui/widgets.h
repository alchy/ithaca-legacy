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
#include <cstdint>
#include <cstdio>
#include <string>

namespace ithaca::gui::wdg {

using theme::Colors;
using theme::Fonts;
namespace L = ithaca::gui::layout;

// -- Text -------------------------------------------------------------------
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
// Bere INTENZITU 0..1, ne ano/ne: nahle prepnuti cvaka, plynuly nabeh
// a dozniv se cte klidneji. Zhasla kontrolka musi byt videt, aby bylo poznat,
// ze existuje — proto se kresli i zhasla, jen nejtlumeneji.
inline void lamp(ImDrawList* dl, ImVec2 pos, const char* label, float intensity,
                 ImU32 on_col = Colors::warn) {
    const float px = fontPx(Fonts::small);
    const float r  = 4.f;
    const ImU32 c  = Colors::lerp(Colors::dimmer, on_col,
                                  std::clamp(intensity, 0.f, 1.f));
    dl->AddCircleFilled(ImVec2(pos.x + r, pos.y + px * 0.5f), r, c, 12);
    dl->AddText(Fonts::small, px, ImVec2(pos.x + r * 2.f + 6.f, pos.y), c, label);
}

inline float lampW(const char* label) {
    return 8.f + 6.f + textW(Fonts::small, fontPx(Fonts::small), label) + 18.f;
}

// -- Ramecek nebo inverzni vypln --------------------------------------------
// Jediny zpusob, jakym se na tomhle displeji dela duraz (viz pravidlo 1 nahore).
// Sdileji ho vsechny "chipove" prvky: bunka radku, tlacitko i prepinac.
inline void chipFrame(ImDrawList* dl, ImVec2 p, ImVec2 q, bool on) {
    if (on) dl->AddRectFilled(p, q, Colors::inv_bg);
    else    dl->AddRect(p, q, Colors::line);
}

// -- Vlnova cara (pozadi) ---------------------------------------------------
// Pozadi ve stylu PS3 XMB: mekke svetelne cary tekouci pres plochu. Kresli se
// ve dvou krocich — waveBuild() spocita body, waveGlow() z nich udela svetlo.
//
// base_y je ABSOLUTNI souradnice osy, ne pomer: osa ma prochazet stredem
// vybraneho patche, ktery zna az stranka PLAY.

// Barva s danou pruhlednosti (RGB z `col`, alfa z `alpha`; alfa se stropuje).
inline ImU32 tint(ImU32 col, float alpha) {
    return (col & 0x00FFFFFF)
         | ((ImU32)(std::clamp(alpha, 0.f, 1.f) * 255.f) << 24);
}

// Spocita BODY vlny do `out`. Vraci pocet zapsanych bodu (0 = nevejde se).
//
// Oddelene od obtazeni proto, ze zar se kresli TREMI tahy pres tytez body
// (siroky a slaby vespod, uzky a jasny nahore). Drive to byla tri volani
// jedne funkce, ktera pokazde znovu spocitala x, tanh i nasobeni amplitudou —
// pri peti stuhach a 128 bodech tedy 1920 volani tanh na snimek misto 640.
inline int waveBuild(ImVec2* out, int cap, float x0, float w, float base_y,
                     const float* pts, int n, float amp_px, float gain) {
    if (n < 2 || cap < n) return 0;
    for (int i = 0; i < n; ++i) {
        const float x = x0 + w * ((float)i / (float)(n - 1));
        // MEKKY limit misto tvrdeho orezu: soucet nosne vlny a modulace zvukem
        // muze presahnout 1.0 a clamp by vrcholy usekl naplocho (bylo videt
        // jako "clipovani"). tanh je ohne, takze tvar zustane hladky. Vstup je
        // konstruovan tak, aby se bezne pohyboval hluboko v LINEARNI oblasti —
        // saturace slouzi jen jako pojistka pro spicky, ne jako bezny rezim.
        const float y = base_y - std::tanh(pts[i] * gain * 0.85f) * amp_px * 1.15f;
        out[i] = ImVec2(x, y);
    }
    return n;
}

// Zar kolem krivky. Misto obtahovani carou se posila pas trojuhelniku, ktery
// ma pruhlednost primo ve vrcholech — grafika mezi nimi interpoluje spojite.
//
// Drive to byly tri obtahy AddPolyline pres sebe (siroky slaby, stredni, uzky
// jasny). Tri konstantni pruhlednosti daji ale schody, a nejvnitrnejsi obtah je
// pres celou svou tloustku PLNY — vysledek se cetl jako plochy PAS s hranou,
// ne jako cara, ktera pohasina do okoli.
//
// Profil napric carou se vzorkuje po drahach vrcholu. Dve pozadovane
// vlastnosti jdou proti sobe a kazda urcuje jeden parametr:
//
//   "jadro musi byt videt"     -> plne kryci oblast musi mit SIRKU (plato
//      o polosirce core_px). Prvni verze mela plnou pruhlednost jen ve
//      stredove drare, tedy v oblasti nulove sirky — rasterizace ji rozredila
//      mezi sousedni pixely a cara byla sotva znat.
//
//   "nesmi byt videt vrstvy"   -> spad musi byt vzorkovany dost husto. Pet
//      drah delalo v mistech zlomu viditelna rozhrani, protoze linearni
//      interpolace mezi nimi ma v kazde draze zlom derivace.
//
// Proto se profil nezadava konstantami, ale POCITA: pruhlednost jde po
// Gaussove krivce a drahy jsou rozlozene kvadraticky (husto u jadra, ridce
// v ohonu), takze pomer sousednich pruhlednosti zustava maly i daleko od
// stredu. Vysledek se cte jako svetlo, ne jako pas ani jako schodiste.
inline void waveGlow(ImDrawList* dl, const ImVec2* p, int n,
                     float core_px, float glow_px, ImU32 col, float alpha) {
    if (n < 2 || alpha <= 0.004f) return;

    // Drah na kazdou stranu od stredu. Cim vic, tim hladsi spad — a tim vic
    // geometrie. Ctyri uz vypadaji stejne jako sest, pet je rezerva pro
    // nejsirsi stuhu (dosah zare 10 px, tam jsou drahy nejdal od sebe).
    // Tohle je prvni misto, kde ubrat, kdyby na Pi bylo tesno.
    constexpr int   kSide  = 5;
    constexpr float kSpace = 1.8f;   // > 1 = hustsi vzorkovani u jadra
    constexpr float kFall  = 3.2f;   // strmost Gaussovy krivky
    // Tri obtahy pres sebe skladaly ve stredu vic, nez byla pruhlednost
    // kteregokoli z nich (1-(1-0.048)(1-0.108)(1-0.30) = 0.41 pri a = 0.30).
    // Jedna vrstva to sama nedozene a cara pak pusobi vybledle, proto se jadro
    // dosvetli na srovnatelnou uroven. tint() alfu stropuje na 1.
    constexpr float kCore  = 1.45f;

    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();

    // Profil je pro celou caru stejny — spocitat jednou, ne v kazdem bode.
    constexpr int kLanes = kSide * 2;
    float off[kLanes];
    ImU32 colr[kLanes];
    for (int j = 0; j < kSide; ++j) {
        const float u = (kSide > 1) ? (float)j / (float)(kSide - 1) : 0.f;
        const float d = core_px + (glow_px - core_px) * std::pow(u, kSpace);
        // Posledni draha musi byt uplne pruhledna, jinak ma zar na svem obvodu
        // hranu.
        const float a = (j == kSide - 1) ? 0.f : std::exp(-kFall * u * u);
        const ImU32 c = tint(col, alpha * a * kCore);
        off [kSide - 1 - j] = -d;  colr[kSide - 1 - j] = c;
        off [kSide + j]     =  d;  colr[kSide + j]     = c;
    }

    const int seg = n - 1;
    dl->PrimReserve(seg * (kLanes - 1) * 6, n * kLanes);
    const unsigned int base = dl->_VtxCurrentIdx;

    for (int i = 0; i < n; ++i) {
        // Normala z tecny (centralni diference). Na koncich jednostranne.
        const ImVec2 a = p[(i > 0) ? i - 1 : 0];
        const ImVec2 b = p[(i < n - 1) ? i + 1 : n - 1];
        float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1e-6f) { dx /= len; dy /= len; } else { dx = 1.f; dy = 0.f; }
        const float nx = -dy, ny = dx;

        const ImVec2 q = p[i];
        for (int k = 0; k < kLanes; ++k)
            dl->PrimWriteVtx(ImVec2(q.x + nx * off[k], q.y + ny * off[k]),
                             uv, colr[k]);
    }

    for (int s = 0; s < seg; ++s) {
        const unsigned int i0 = base + (unsigned)s * kLanes;
        const unsigned int i1 = i0 + kLanes;
        for (unsigned k = 0; k + 1 < (unsigned)kLanes; ++k) {
            dl->PrimWriteIdx((ImDrawIdx)(i0 + k));
            dl->PrimWriteIdx((ImDrawIdx)(i0 + k + 1));
            dl->PrimWriteIdx((ImDrawIdx)(i1 + k + 1));
            dl->PrimWriteIdx((ImDrawIdx)(i0 + k));
            dl->PrimWriteIdx((ImDrawIdx)(i1 + k + 1));
            dl->PrimWriteIdx((ImDrawIdx)(i1 + k));
        }
    }
}

// -- Dotykovy slider --------------------------------------------------------
// Hodnota se meni JEN tahem, nikdy klepnutim. Stock ImGui slider skace na
// misto kliknuti — pri testovani stare verze to omylem prepsalo prah limiteru
// a na dotyku by to bylo mnohem horsi.
//
// Vraci true, kdyz se hodnota v tomhle framu zmenila.
inline bool paramSlider(const char* id, const char* label, float* v,
                        float lo, float hi, const char* fmt,
                        bool enabled = true, float trk_h = 0.f) {
    if (trk_h <= 0.f) trk_h = L::Dims::param_trk;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float  w = ImGui::GetContentRegionAvail().x;
    const float  lab_px = fontPx(Fonts::small);
    const float  val_px = fontPx(Fonts::ui);

    const ImU32 lab_c = enabled ? Colors::dim   : Colors::dimmer;
    const ImU32 val_c = enabled ? Colors::ink   : Colors::dimmer;
    const ImU32 fil_c = enabled ? Colors::fill  : Colors::trough;

    // Track.
    const float ty = o.y;
    const float th = trk_h;
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

    // Popisek a hodnota DOVNITR pasu. Usetri to 24 px na kazdem parametru
    // (na strance DSP jsou ctyri) a pas pak vypada jako tah faderu, ne jako
    // formularove pole s popiskem nad nim.
    //
    // Text lezi castecne na vyplni a castecne na korytu, takze jedna barva
    // vzdy nekde splyne (svetly popisek na svetle vyplni zmizel). Kresli se
    // proto DVAKRAT, orezany na hrane vyplne: nad vyplni inverzne, nad korytem
    // normalne. Az po zarazce, aby ji text prekryl a ne naopak.
    char buf[48];
    std::snprintf(buf, sizeof(buf), fmt, *v);
    auto splitText = [&](ImFont* f, float px, ImVec2 pos, ImU32 c_over, ImU32 c_out,
                         const char* txt) {
        dl->PushClipRect(ImVec2(o.x, ty), ImVec2(gx, ty + th), true);
        dl->AddText(f, px, pos, c_over, txt);
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(gx, ty), ImVec2(o.x + w, ty + th), true);
        dl->AddText(f, px, pos, c_out, txt);
        dl->PopClipRect();
    };
    const ImU32 over_c = enabled ? Colors::inv_fg : Colors::dimmer;
    splitText(Fonts::small, lab_px,
              ImVec2(o.x + 14.f, ty + (th - lab_px) * 0.5f), over_c, lab_c, label);
    splitText(Fonts::ui, val_px,
              ImVec2(o.x + w - textW(Fonts::ui, val_px, buf) - 14.f,
                     ty + (th - val_px) * 0.5f), over_c, val_c, buf);

    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y));
    ImGui::InvisibleButton(id, ImVec2(w, th));

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

// -- Akcni tlacitko ---------------------------------------------------------
// Pro akce (RELOAD, RESCAN, RESET). Prepinac by k nim vykreslil stav ON/OFF,
// coz u akce nedava smysl — nic se nezapina.
//
// `fixed_w > 0` = tlacitko dostane presne tuhle sirku (roztazeny radek). Bez
// nej se sirka meri z popisku, coz je v mrizce nezadouci: sousedni tlacitka by
// koncila jinde.
inline bool button(const char* id, const char* label, float fixed_w = 0.f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float px = fontPx(Fonts::small);
    const float tw = textW(Fonts::small, px, label);
    const float w  = (fixed_w > 0.f) ? fixed_w : (tw + L::Dims::btn_pad * 2.f);
    const float h  = L::Dims::touch * L::Dims::chip_h_f;
    // Pri mereni z popisku zustava text vlevo (tak to vypada v liste),
    // pri pevne sirce se centruje — jinak by v siroke bunce plaval u kraje.
    const float tx = (fixed_w > 0.f) ? (o.x + (w - tw) * 0.5f)
                                     : (o.x + L::Dims::btn_pad);

    ImGui::SetCursorScreenPos(o);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, L::Dims::touch));
    const bool down    = ImGui::IsItemActive();

    chipFrame(dl, o, ImVec2(o.x + w, o.y + h), down);
    dl->AddText(Fonts::small, px, ImVec2(tx, o.y + (h - px) * 0.5f),
                down ? Colors::inv_fg : Colors::ink, label);
    return clicked;
}

// -- ON/OFF prepinac --------------------------------------------------------
// `fixed_w` viz button(). Popisek zustava vlevo a stav vpravo i v roztazene
// bunce — to je cteni "co / jak", ktere se centrovanim rozpadne.
inline bool toggle(const char* id, const char* label, bool on, float fixed_w = 0.f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float px = fontPx(Fonts::small);
    const float w  = (fixed_w > 0.f) ? fixed_w
                                     : (textW(Fonts::small, px, label) + 60.f);
    const float h  = L::Dims::touch * L::Dims::chip_h_f;  // opticky mensi, zona plna

    // Popisek vlevo, stav vpravo — cteni "co / jak". Centrovanim by se rozpadlo.
    const char* state = on ? "ON" : "OFF";
    const float ty    = o.y + (h - px) * 0.5f;
    chipFrame(dl, o, ImVec2(o.x + w, o.y + h), on);
    dl->AddText(Fonts::small, px, ImVec2(o.x + L::Dims::chip_pad, ty),
                on ? Colors::inv_fg : Colors::dim, label);
    dl->AddText(Fonts::small, px,
                ImVec2(o.x + w - textW(Fonts::small, px, state) - L::Dims::chip_pad, ty),
                on ? Colors::inv_fg : Colors::dimmer, state);
    ImGui::SetCursorScreenPos(o);
    // Dotykova zona je vyssi nez vykresleny chip.
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, L::Dims::touch));
    return clicked;
}

// -- Pole v roztazenem radku ------------------------------------------------
// Jedna bunka radku rozdeleneho pres layout::splitRow: ramecek nebo inverzni
// vypln + text NA STREDU. Text se orezava na bunku — jmena IR jsou cesty
// k souboru a ta se do bunky nevejdou.
inline void cell(ImDrawList* dl, ImVec2 p, ImVec2 q, const char* txt, bool on,
                 ImFont* f = nullptr) {
    if (!f) f = Fonts::small;
    const float px = fontPx(f);
    chipFrame(dl, p, q, on);
    const float tw = textW(f, px, txt);
    dl->PushClipRect(ImVec2(p.x + L::Dims::cell_clip, p.y),
                     ImVec2(q.x - L::Dims::cell_clip, q.y), true);
    dl->AddText(f, px,
                ImVec2(p.x + (q.x - p.x - tw) * 0.5f,
                       p.y + (q.y - p.y - px) * 0.5f),
                on ? Colors::inv_fg : Colors::dim, txt);
    dl->PopClipRect();
}

// -- Radek voleb ------------------------------------------------------------
// Volby jako pole vedle sebe, ROZTAZENE na celou zadanou sirku. Na dotyku lepsi
// nez rozbalovaci seznam: jedno klepnuti misto dvou a vzdy je videt cela
// nabidka. Vraci index, na ktery se kliklo (jinak -1).
//
// Sirka bunky se NEODVIJI od delky textu — jinak by radky pod sebou koncily
// jinde a kazdy cil by byl jinak velky.
// `hit_h` = vyska dotykove zony (0 = auto). Zona je vyssi nez vykreslena bunka,
// takze ji volajici musi omezit rozteci radku — jinak by zony sousednich radku
// zasahovaly do sebe a klepnuti by padalo do spatneho.
//
// `is_on(i)` rozhoduje, ktera bunka je zvyraznena. Predikat misto indexu proto,
// ze radek slouzi jak vyberu jedne polozky (port, buffer), tak prepinani
// nezavislych bitu (maska MIDI kanalu).
// -- Jedna smycka pro VSECHNY roztazene radky -------------------------------
// Rozdeleni sirky, generovani ID a dotykove zony jsou pokazde tataz vec; lisi
// se jen to, co se do bunky nakresli. `draw(i, p, q)` si tedy kresli sam.
//
// Drive byla tahle smycka opsana ctyrikrat: v tabBar, v chipRowIf, ve volici IR
// na strance DSP (page_params.cpp) a v dvojici tlacitek na SYS. Vcetne
// snprintf("%s_%d") pokazde znovu.
//
// Dotykova zona smi byt VYSSI nez vykreslena bunka a sazi se na jeji stred;
// volajici ji musi omezit rozteci radku, jinak by zony sousednich radku
// zasahovaly do sebe a klepnuti by padlo do spatneho.
template <class DrawFn>
int itemRow(const char* id, ImVec2 pos, float w, int n, float cell_h,
            float hit_h, float gap, DrawFn draw) {
    const L::Row row = L::splitRow(pos, w, cell_h, n, gap);
    hit_h = std::max(hit_h, cell_h);
    int hit = -1;

    for (int i = 0; i < n; ++i) {
        const ImVec2 p = row.at(i), q = row.end(i);
        draw(i, p, q);

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y - (hit_h - cell_h) * 0.5f));
        char bid[48]; std::snprintf(bid, sizeof(bid), "%s_%d", id, i);
        if (ImGui::InvisibleButton(bid, ImVec2(row.cell, hit_h))) hit = i;
    }
    return hit;
}

template <class OnFn>
int chipRowIf(const char* id, ImVec2 pos, float w, const char* const* items,
              int n, OnFn is_on, float h = L::Dims::chip_h, float hit_h = 0.f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hit_h <= 0.f) {
        // Pri zalomeni do vic radek nesmi zona presahnout rozteC.
        const L::Row probe = L::splitRow(pos, w, h, n);
        hit_h = (probe.rows > 1) ? (h + probe.gap) : L::Dims::touch;
    }
    return itemRow(id, pos, w, n, h, hit_h, L::Dims::gap_s,
                   [&](int i, ImVec2 p, ImVec2 q) {
                       cell(dl, p, q, items[i], is_on(i));
                   });
}

// Vyber JEDNE polozky. Klepnuti na uz vybranou nic nedela (neni co menit).
inline int chipRow(const char* id, ImVec2 pos, float w, const char* const* items,
                   int n, int cur, float h = L::Dims::chip_h, float hit_h = 0.f) {
    const int hit = chipRowIf(id, pos, w, items, n,
                              [cur](int i) { return i == cur; }, h, hit_h);
    return (hit == cur) ? -1 : hit;
}

// Prepinani NEZAVISLYCH bitu masky. Klepnuti na rozsvicenou ji zhasne
// a naopak; sousedni bity zustavaji, jak byly.
inline int chipRowMask(const char* id, ImVec2 pos, float w,
                       const char* const* items, int n, uint32_t mask,
                       int bit0 = 0, float h = L::Dims::chip_h, float hit_h = 0.f) {
    return chipRowIf(id, pos, w, items, n,
                     [mask, bit0](int i) { return ((mask >> (bit0 + i)) & 1u) != 0u; },
                     h, hit_h);
}

// Vyska, kterou takovy radek zabere (kvuli zalomeni ji volajici nezna dopredu).
inline float chipRowHeight(float w, int n, float h = L::Dims::chip_h) {
    return L::splitRow(ImVec2(0, 0), w, h, n).height();
}

// -- Radek zalozek ----------------------------------------------------------
// Zvyraznena zalozka je zaroven nadpis stranky, proto stranky nemaji vlastni
// hlavicku. Vraci true pri zmene.
// `square` = dlazdice se kresli jako CTVEREC o strane h, vycentrovany v bunce,
// ale dotykova zona zustava cela bunka. Ctverec pres celou sirku bunky by pri
// sedmi zalozkach mel 175 px na vysku a stranka DSP by se uz nevesla; takhle
// je videt ctverec, prst ma k dispozici vic.
inline bool tabBar(const char* id, const char* const* labels, int n, int& sel,
                   float h, bool square = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float  total = ImGui::GetContentRegionAvail().x;
    // Sirka bunky se bere z Row, ne jako (q.x - p.x): to druhe je ve floatu
    // o kousek jine cislo (p.x + cell - p.x != cell) a ctverce by se posunuly
    // o zlomek pixelu.
    const L::Row row = L::splitRow(o, total, h, n, L::Dims::tab_gap);

    const int hit = itemRow(id, o, total, n, h, h, L::Dims::tab_gap,
        [&](int i, ImVec2 p, ImVec2 q) {
            const bool on = (i == sel);
            if (!square) { cell(dl, p, q, labels[i], on); return; }
            // Ctverec vycentrovany v bunce; dotykova zona zustava cela bunka.
            // Ctverec pres celou sirku bunky by pri sedmi zalozkach mel 175 px
            // na vysku a stranka DSP by se uz nevesla.
            const float s  = std::min(h, row.cell);
            const float mx = p.x + row.cell * 0.5f;
            cell(dl, ImVec2(mx - s * 0.5f, p.y), ImVec2(mx + s * 0.5f, p.y + s),
                 labels[i], on);
        });

    const bool changed = (hit >= 0 && hit != sel);
    if (changed) sel = hit;

    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + row.height()));
    ImGui::Dummy(ImVec2(total, 0.f));
    return changed;
}

} // namespace ithaca::gui::wdg
