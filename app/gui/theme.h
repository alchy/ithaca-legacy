#pragma once
// app/gui/theme.h — Art Deco theme: barevne tokeny (stribro=struktura,
// zlato=zivy akcent), fonty (Cormorant), apply_theme/load_fonts. Header-only,
// vzor prevzat z icr2 player/gui/theme.h.
#include "imgui.h"

namespace ithaca::gui::theme {

// -- Barevne tokeny (schema A) -------------------------------------------
struct Colors {
    static constexpr ImU32 bg        = IM_COL32(0x0d, 0x0e, 0x10, 255);
    static constexpr ImU32 bg_panel  = IM_COL32(0x09, 0x0a, 0x0b, 255);
    static constexpr ImU32 ink       = IM_COL32(0xe2, 0xe6, 0xea, 255); // jasne stribro
    static constexpr ImU32 silver    = IM_COL32(0xcd, 0xd2, 0xd6, 255); // default text
    static constexpr ImU32 silver2   = IM_COL32(0xaa, 0xb0, 0xb6, 255); // titulky, master, peak
    static constexpr ImU32 line      = IM_COL32(0x3a, 0x40, 0x46, 255);
    static constexpr ImU32 line_soft = IM_COL32(0x24, 0x2a, 0x2f, 255);
    static constexpr ImU32 muted     = IM_COL32(0x7e, 0x85, 0x8c, 255); // eyebrow/dim
    static constexpr ImU32 gold      = IM_COL32(0xd4, 0xaf, 0x37, 255); // AKCENT only

    static ImVec4 v(ImU32 c) {
        return ImVec4(( c        & 0xFF) / 255.f,
                      ((c >>  8) & 0xFF) / 255.f,
                      ((c >> 16) & 0xFF) / 255.f,
                      ((c >> 24) & 0xFF) / 255.f);
    }
};

// -- Fonty (Cormorant @ vice velikosti) ----------------------------------
// Font je ZABUDOVANY v binarce (viz embedded_font.cpp + CMake pravidlo
// cormorant_font). ithaca-gui tedy za behu nepotrebuje zadny asset — staci mu
// vlastni state.json. Data jsou komprimovana ImGui nastrojem
// binary_to_compressed_c; AddFontFromMemoryCompressedTTF si je rozbali do
// vlastniho bufferu, takze o vlastnictvi pameti se nemusime starat.
const unsigned int* cormorantCompressedData();
unsigned int        cormorantCompressedSize();

struct Fonts {
    static inline ImFont* body    = nullptr; // 18px
    static inline ImFont* eyebrow = nullptr; // 11px, +1.5 tracking
    static inline ImFont* value   = nullptr; // 34px stat cisla
    static inline ImFont* brand   = nullptr; // 20px logo, +6 tracking
};

inline void load_fonts(float scale = 1.f) {
    if (Fonts::body) return;
    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar ranges[] = {
        0x0020, 0x00FF, // Latin + Latin-1 (CZ + bösendorf ö)
        0x0100, 0x017F, // Latin Extended-A
        0x2010, 0x205E, // punctuation (dashes, …)
        0x2190, 0x21FF, // arrows (↻ U+21BB rescan)
        0x25A0, 0x25FF, // geometric (● ○ indikatory)
        0,
    };
    ImFontConfig cfg;
    cfg.OversampleH = 2; cfg.OversampleV = 2; cfg.PixelSnapH = true;

    // HiDPI ostrost: rasterizuj font ve FYZICKEM rozliseni (size * g_scale),
    // caller pak nastavi io.FontGlobalScale = 1/g_scale → zobrazi se v logicke
    // velikosti, ale ostre. (Drive: raster v logicke velikosti + FontGlobalScale
    // = roztazeny bitmap = velky + rozmazany.) Letterspacing taky * g_scale.
    const float sc = (scale > 0.f) ? scale : 1.f;
    // Letterspacing: ImGui 1.91 ma GlyphExtraSpacing (ImVec2, jen X osa).
    // (Novejsi ImGui to prejmenovalo na GlyphExtraAdvanceX — my mame 1.91.)
    // Vsechny ctyri velikosti jedou z TEHOZ zabudovaneho blobu.
    const unsigned int* data = cormorantCompressedData();
    const unsigned int  size = cormorantCompressedSize();
    auto add = [&](float px, float tracking) {
        cfg.GlyphExtraSpacing.x = tracking * sc;
        return io.Fonts->AddFontFromMemoryCompressedTTF(data, (int)size, px * sc,
                                                        &cfg, ranges);
    };
    Fonts::body    = add(18.f, 0.f);
    Fonts::eyebrow = add(11.f, 1.5f);   // prostrkane eyebrow popisky
    Fonts::value   = add(34.f, 0.f);
    Fonts::brand   = add(20.f, 6.f);    // prostrkane logo ITHACA

    if (!Fonts::body) { // load failed → default
        Fonts::body = io.Fonts->AddFontDefault();
        Fonts::eyebrow = Fonts::value = Fonts::brand = Fonts::body;
    } else {
        if (!Fonts::eyebrow) Fonts::eyebrow = Fonts::body;
        if (!Fonts::value)   Fonts::value   = Fonts::body;
        if (!Fonts::brand)   Fonts::brand   = Fonts::body;
    }
}

inline void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = s.ChildRounding = s.FrameRounding = 0.f;
    s.GrabRounding = s.PopupRounding = s.ScrollbarRounding = s.TabRounding = 0.f;
    s.WindowPadding    = ImVec2(0, 0);   // panely si delaji vlastni padding
    s.FramePadding     = ImVec2(8, 4);
    s.ItemSpacing      = ImVec2(10, 4);   // mensi vertikalni mezera mezi prvky/pasmy
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.ScrollbarSize    = 8.f;
    s.WindowBorderSize = s.ChildBorderSize = s.FrameBorderSize = 0.f;
    s.GrabMinSize      = 4.f;

    auto& c = s.Colors;
    c[ImGuiCol_WindowBg]       = Colors::v(Colors::bg);
    c[ImGuiCol_ChildBg]        = Colors::v(Colors::bg);
    c[ImGuiCol_PopupBg]        = Colors::v(Colors::bg_panel);
    c[ImGuiCol_Border]         = Colors::v(Colors::line);
    c[ImGuiCol_BorderShadow]   = ImVec4(0,0,0,0);
    c[ImGuiCol_Text]           = Colors::v(Colors::silver);
    c[ImGuiCol_TextDisabled]   = Colors::v(Colors::muted);
    c[ImGuiCol_FrameBg]        = Colors::v(Colors::line_soft);
    c[ImGuiCol_FrameBgHovered] = Colors::v(Colors::line);
    c[ImGuiCol_FrameBgActive]  = Colors::v(Colors::line);
    c[ImGuiCol_SliderGrab]       = Colors::v(Colors::silver2);
    c[ImGuiCol_SliderGrabActive] = Colors::v(Colors::ink);
    c[ImGuiCol_Button]         = ImVec4(0,0,0,0);
    c[ImGuiCol_ButtonHovered]  = Colors::v(Colors::line_soft);
    c[ImGuiCol_ButtonActive]   = Colors::v(Colors::line);
    c[ImGuiCol_Header]         = Colors::v(Colors::line_soft);
    c[ImGuiCol_HeaderHovered]  = Colors::v(Colors::line);
    c[ImGuiCol_HeaderActive]   = Colors::v(Colors::line);
    c[ImGuiCol_Separator]      = Colors::v(Colors::line_soft);
    c[ImGuiCol_ScrollbarBg]    = ImVec4(0,0,0,0);
    c[ImGuiCol_ScrollbarGrab]  = Colors::v(Colors::line);
}

} // namespace ithaca::gui::theme
