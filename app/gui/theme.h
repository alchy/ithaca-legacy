#pragma once
// app/gui/theme.h — Art Deco theme: barevne tokeny (stribro=struktura,
// zlato=zivy akcent), fonty (Cormorant), apply_theme/load_fonts. Header-only,
// vzor prevzat z icr2 player/gui/theme.h.
#include "imgui.h"
#include <fstream>
#include <string>

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

// Najdi asset relativne k CWD (vzor icr2 util::find_asset_path). Vraci prazdne
// kdyz nenalezeno → load_fonts spadne na default ImGui font.
inline std::string find_asset_path(const std::string& rel) {
    auto exists = [](const std::string& p) {
        std::ifstream f(p); return f.good();
    };
    if (exists(rel)) return rel;
    static const char* prefixes[] = {
        "./third-party/", "./", "../third-party/",
    };
    for (const char* pre : prefixes) {
        std::string c = std::string(pre) + rel;
        if (exists(c)) return c;
    }
    return {};
}

// -- Fonty (Cormorant @ vice velikosti) ----------------------------------
struct Fonts {
    static inline ImFont* body    = nullptr; // 18px
    static inline ImFont* eyebrow = nullptr; // 11px, +1.5 tracking
    static inline ImFont* value   = nullptr; // 26px stat cisla
    static inline ImFont* brand   = nullptr; // 20px logo, +6 tracking
};

inline void load_fonts(const std::string& ttf_path) {
    if (Fonts::body) return;
    ImGuiIO& io = ImGui::GetIO();
    if (ttf_path.empty()) {
        Fonts::body = io.Fonts->AddFontDefault();
        Fonts::eyebrow = Fonts::value = Fonts::brand = Fonts::body;
        return;
    }
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

    // Letterspacing: ImGui 1.91 ma GlyphExtraSpacing (ImVec2, jen X osa).
    // (Novejsi ImGui to prejmenovalo na GlyphExtraAdvanceX — my mame 1.91.)
    cfg.GlyphExtraSpacing.x = 0.f;
    Fonts::body = io.Fonts->AddFontFromFileTTF(ttf_path.c_str(), 18.f, &cfg, ranges);
    cfg.GlyphExtraSpacing.x = 1.5f;   // prostrkane eyebrow popisky
    Fonts::eyebrow = io.Fonts->AddFontFromFileTTF(ttf_path.c_str(), 11.f, &cfg, ranges);
    cfg.GlyphExtraSpacing.x = 0.f;
    Fonts::value = io.Fonts->AddFontFromFileTTF(ttf_path.c_str(), 26.f, &cfg, ranges);
    cfg.GlyphExtraSpacing.x = 6.f;    // prostrkane logo ITHACA
    Fonts::brand = io.Fonts->AddFontFromFileTTF(ttf_path.c_str(), 20.f, &cfg, ranges);

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
    s.ItemSpacing      = ImVec2(10, 8);
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
