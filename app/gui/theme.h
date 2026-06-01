#pragma once
// app/gui/theme.h — Art Deco theme: barevne tokeny (stribro=struktura,
// zlato=zivy akcent), fonty (Cormorant), apply_theme/load_fonts. Header-only,
// vzor prevzat z icr2 player/gui/theme.h.
#include "imgui.h"
#include <cstdint>
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

} // namespace ithaca::gui::theme
