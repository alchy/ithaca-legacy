#pragma once
// app/gui/theme.h — pojeti znakoveho displeje.
// ----------------------------------------------------------------------------
// Vzor: LCD/OLED panely starych syntezatoru (Roland D-550, Alesis) — svetly text
// na modrem podsvicenem poli, jemny rastr radku, DURAZ INVERZI POLE, ne tucnym
// rezem. Skutecne znakove displeje zadny druhy rez nemaji; proto ma rozhrani
// jedinou vahu pisma a zvyrazneni se dela plnym polem s tmavym textem.
//
// Cela paleta je JEDNA modra v odstinech. Informaci nese jas a inverze,
// ne barevny kod — stejne jako na skutecnem znakovem displeji.
#include "imgui.h"

namespace ithaca::gui::theme {

// -- Barevne tokeny ---------------------------------------------------------
struct Colors {
    // Displej
    static constexpr ImU32 lcd       = IM_COL32(0x0e, 0x2f, 0x7a, 255); // podsvicene pole
    static constexpr ImU32 bezel     = IM_COL32(0x17, 0x18, 0x1a, 255); // ramecek = mrtva zona
    // Text (odstupnovany jas jedne barvy)
    static constexpr ImU32 ink       = IM_COL32(0xea, 0xf2, 0xff, 255); // plny jas
    static constexpr ImU32 dim       = IM_COL32(0xa8, 0xc0, 0xe8, 255); // tlumeny
    static constexpr ImU32 dimmer    = IM_COL32(0x6d, 0x8c, 0xc4, 255); // nejtlumenejsi
    // Struktura
    static constexpr ImU32 line      = IM_COL32(0x5c, 0x86, 0xd6, 255); // ramy, delice
    static constexpr ImU32 trough    = IM_COL32(0x0a, 0x23, 0x59, 255); // zahlubeni (track)
    static constexpr ImU32 fill      = IM_COL32(0x2f, 0x5d, 0xb3, 255); // vyplnena cast
    // Inverzni pole = vyber / duraz
    static constexpr ImU32 inv_bg    = IM_COL32(0xcf, 0xe0, 0xff, 255);
    static constexpr ImU32 inv_fg    = IM_COL32(0x0e, 0x2f, 0x7a, 255);
    // Stavy. ZADNA jina barva nez modra: varovani a chyby nese JAS a INVERZE,
    // presne jako na monochromatickem LCD. Barevny kod by tu byl cizorody —
    // a na podsvicenem modrem poli stejne nikdy nevypada dobre.
    static constexpr ImU32 warn      = IM_COL32(0xea, 0xf2, 0xff, 255); // plny jas
    static constexpr ImU32 error     = IM_COL32(0xcf, 0xe0, 0xff, 255); // inverzni blok
    static constexpr ImU32 hot       = IM_COL32(0xff, 0xff, 0xff, 255); // spicka metru

    // Prechod mezi dvema barvami. Pouziva se na obarveni vlny do cervena
    // pri blizeni ke clippingu — jedina zamerna vyjimka z modre palety,
    // protoze pretizeni nejde ukazat jasem (vlna uz je pri nem nejjasnejsi).
    static ImU32 lerp(ImU32 a, ImU32 b, float t) {
        t = (t < 0.f) ? 0.f : (t > 1.f ? 1.f : t);
        auto ch = [&](int sh) {
            const float x = (float)((a >> sh) & 0xFF);
            const float y = (float)((b >> sh) & 0xFF);
            return (ImU32)(x + (y - x) * t) & 0xFFu;
        };
        return ch(0) | (ch(8) << 8) | (ch(16) << 16) | (ch(24) << 24);
    }

    // Prehlceni. Odstiny cervene, do kterych vlna prechazi od -9 dB k 0 dB.
    static constexpr ImU32 clip_lo = IM_COL32(0xff, 0xa0, 0x6b, 255); // -9 dB
    static constexpr ImU32 clip_hi = IM_COL32(0xff, 0x3b, 0x3b, 255); //  0 dB

    static ImVec4 v(ImU32 c) {
        return ImVec4(( c        & 0xFF) / 255.f,
                      ((c >>  8) & 0xFF) / 255.f,
                      ((c >> 16) & 0xFF) / 255.f,
                      ((c >> 24) & 0xFF) / 255.f);
    }
};

// -- Pisma zabudovana v binarce (viz embedded_fonts.cpp) --------------------
struct FontBlob { const unsigned int* data; unsigned int size; };
FontBlob monoBlob();         // JetBrains Mono Regular — cele rozhrani
FontBlob brandBoldBlob();    // Barlow Condensed Bold  — "ITHACA"
FontBlob brandLightBlob();   // Barlow Condensed Light — "LEGACY"

// Rozhrani ma JEDNU vahu, jen ruzne velikosti (logicke px pro panel 1280x720).
struct Fonts {
    static inline ImFont* ui     = nullptr;  // 18 px — bezny text
    static inline ImFont* small  = nullptr;  // 14 px — popisky, zalozky, lampy
    static inline ImFont* num    = nullptr;  // 28 px — stavova cisla
    static inline ImFont* big    = nullptr;  // 40 px — vybrana banka
    static inline ImFont* brand  = nullptr;  // 64 px Bold  — ITHACA
    static inline ImFont* brandl = nullptr;  // 64 px Light — LEGACY
};

inline void load_fonts(float scale = 1.f) {
    if (Fonts::ui) return;
    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,   // Latin + Latin-1
        0x0100, 0x017F,   // Latin Extended-A (cestina)
        0x2010, 0x205E,   // interpunkce, pomlcky, …
        0x2190, 0x21FF,   // sipky
        0x25A0, 0x25FF,   // geometricke (● ○ ▲ ▼ █)
        0,
    };
    ImFontConfig cfg;
    cfg.OversampleH = 2; cfg.OversampleV = 2; cfg.PixelSnapH = true;

    // HiDPI ostrost: rasterizuj ve FYZICKEM rozliseni (size * scale); volajici
    // nastavi io.FontGlobalScale = 1/scale → zobrazi se v logicke velikosti, ostre.
    const float sc = (scale > 0.f) ? scale : 1.f;
    const FontBlob mono = monoBlob();

    auto addMono = [&](float px, float tracking) {
        cfg.GlyphExtraSpacing.x = tracking * sc;
        return io.Fonts->AddFontFromMemoryCompressedTTF(
            mono.data, (int)mono.size, px * sc, &cfg, ranges);
    };
    Fonts::ui    = addMono(18.f, 0.f);
    Fonts::small = addMono(14.f, 1.2f);   // prostrkane popisky
    Fonts::num   = addMono(28.f, 0.f);
    Fonts::big   = addMono(40.f, 0.f);

    // Wordmark — jine pismo nez rozhrani. Na skutecnych pristrojich mel displej
    // znakovy font, ale logo na panelu bylo sitotiskem v uzkem grotesku.
    const FontBlob bb = brandBoldBlob(), bl = brandLightBlob();
    cfg.GlyphExtraSpacing.x = 7.f * sc;
    Fonts::brand  = io.Fonts->AddFontFromMemoryCompressedTTF(
        bb.data, (int)bb.size, 64.f * sc, &cfg, ranges);
    Fonts::brandl = io.Fonts->AddFontFromMemoryCompressedTTF(
        bl.data, (int)bl.size, 64.f * sc, &cfg, ranges);

    if (!Fonts::ui) {   // nemelo by nastat (data jsou v binarce), ale radeji
        Fonts::ui = io.Fonts->AddFontDefault();
        Fonts::small = Fonts::num = Fonts::big = Fonts::brand = Fonts::brandl = Fonts::ui;
    } else {
        if (!Fonts::small)  Fonts::small  = Fonts::ui;
        if (!Fonts::num)    Fonts::num    = Fonts::ui;
        if (!Fonts::big)    Fonts::big    = Fonts::ui;
        if (!Fonts::brand)  Fonts::brand  = Fonts::ui;
        if (!Fonts::brandl) Fonts::brandl = Fonts::brand;
    }
}

inline void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    // Znakovy displej nema zaoblene rohy ani stiny.
    s.WindowRounding = s.ChildRounding = s.FrameRounding = 0.f;
    s.GrabRounding = s.PopupRounding = s.ScrollbarRounding = s.TabRounding = 0.f;
    s.WindowPadding    = ImVec2(0, 0);
    s.FramePadding     = ImVec2(10, 8);
    s.ItemSpacing      = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.ScrollbarSize    = 10.f;
    s.WindowBorderSize = s.ChildBorderSize = s.FrameBorderSize = 0.f;
    s.GrabMinSize      = 8.f;

    auto& c = s.Colors;
    c[ImGuiCol_WindowBg]         = Colors::v(Colors::lcd);
    c[ImGuiCol_ChildBg]          = Colors::v(Colors::lcd);
    c[ImGuiCol_PopupBg]          = Colors::v(Colors::trough);
    c[ImGuiCol_Border]           = Colors::v(Colors::line);
    c[ImGuiCol_BorderShadow]     = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text]             = Colors::v(Colors::ink);
    c[ImGuiCol_TextDisabled]     = Colors::v(Colors::dimmer);
    c[ImGuiCol_FrameBg]          = Colors::v(Colors::trough);
    c[ImGuiCol_FrameBgHovered]   = Colors::v(Colors::fill);
    c[ImGuiCol_FrameBgActive]    = Colors::v(Colors::fill);
    c[ImGuiCol_SliderGrab]       = Colors::v(Colors::inv_bg);
    c[ImGuiCol_SliderGrabActive] = Colors::v(Colors::ink);
    c[ImGuiCol_Button]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ButtonHovered]    = Colors::v(Colors::fill);
    c[ImGuiCol_ButtonActive]     = Colors::v(Colors::line);
    c[ImGuiCol_Header]           = Colors::v(Colors::fill);
    c[ImGuiCol_HeaderHovered]    = Colors::v(Colors::fill);
    c[ImGuiCol_HeaderActive]     = Colors::v(Colors::line);
    c[ImGuiCol_Separator]        = Colors::v(Colors::line);
    c[ImGuiCol_ScrollbarBg]      = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]    = Colors::v(Colors::fill);
}

} // namespace ithaca::gui::theme
