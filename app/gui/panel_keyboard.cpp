// app/gui/panel_keyboard.cpp - 88-key piano keyboard (MIDI 21..108).
// Bile a cerne klavesy renderujeme primo pres ImGui draw list. Aktivni
// noty zvyrazneny modre (intenzita = currentGainFor). Pedal indikator
// pod klavesnici: zeleny=cc0, zluty=cc64, cerveny=cc127.
#include "panel_keyboard.h"
#include "app_context.h"
#include "imgui.h"
#include <string>

namespace ithaca::gui {

namespace {
constexpr int kFirstMidi = 21;   // A0
constexpr int kLastMidi  = 108;  // C8 (88 klaves total)

// True kdyz je MIDI nota cerna klavesa (pitch class 1, 3, 6, 8, 10).
bool isBlackKey(int midi) {
    const int pc = ((midi % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
} // namespace

void renderKeyboardPanel(AppContext& ctx) {
    // Klaviatura vyplni dostupnou sirku, vyska fixne 60 px (klavesovy proužek).
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 60.f;

    // Cti masku aktivnich not (atomic snapshot).
    bool active[128];
    ctx.engine.activeMidiNotes(active);

    // Pocet bilych klaves v rozsahu (pro rozdeleni sirky).
    int white_count = 0;
    for (int m = kFirstMidi; m <= kLastMidi; ++m) {
        if (!isBlackKey(m)) ++white_count;
    }
    if (white_count <= 0) return;

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float padding = 10.f;
    const float pedal_strip_h = 22.f;   // vyska pedal indikatoru + popisek
    const float key_w   = (w - 2.f * padding) / (float)white_count;
    const float key_h   = h - pedal_strip_h - padding;
    const float black_w = key_w * 0.6f;
    const float black_h = key_h * 0.6f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Render bile klavesy prvni (cerne potom pres ne).
    int wi = 0;
    for (int m = kFirstMidi; m <= kLastMidi; ++m) {
        if (isBlackKey(m)) continue;
        const float kx = pos.x + padding + wi * key_w;
        const float ky = pos.y;
        ImU32 col = IM_COL32(245, 245, 245, 255);
        if (active[m]) {
            float g = ctx.engine.currentGainFor(m);
            if (g < 0.3f) g = 0.3f;
            if (g > 1.f)  g = 1.f;
            // Modra zvyrazneni: cim hlasnejsi, tim sytějsi modra.
            const int r = 80 + (int)(80.f * (1.f - g));
            col = IM_COL32(r, 130, 255, 255);
        }
        dl->AddRectFilled({kx, ky}, {kx + key_w - 1.f, ky + key_h}, col);
        dl->AddRect({kx, ky}, {kx + key_w - 1.f, ky + key_h},
                    IM_COL32(60, 60, 60, 255));
        ++wi;
    }

    // Render cerne klavesy nahore (over bile).
    wi = 0;
    for (int m = kFirstMidi; m <= kLastMidi; ++m) {
        if (!isBlackKey(m)) { ++wi; continue; }
        // Cerna klavesa lezi mezi bilou (wi-1) a wi.
        const float kx = pos.x + padding + (wi - 1) * key_w + key_w - black_w * 0.5f;
        const float ky = pos.y;
        ImU32 col = IM_COL32(20, 20, 20, 255);
        if (active[m]) {
            float g = ctx.engine.currentGainFor(m);
            if (g < 0.3f) g = 0.3f;
            if (g > 1.f)  g = 1.f;
            const int b = 160 + (int)(90.f * g);
            col = IM_COL32(50, 100, b, 255);
        }
        dl->AddRectFilled({kx, ky}, {kx + black_w, ky + black_h}, col);
    }

    // Pedal indikator pod klavesnici.
    const int cc = (int)ctx.engine.pedalCC();
    ImU32 pcol = (cc >= 100) ? IM_COL32(220, 70, 70, 255)
               : (cc >= 64)  ? IM_COL32(220, 200, 70, 255)
                              : IM_COL32(80, 200, 80, 255);
    const float pedal_y = pos.y + key_h + 4.f;
    dl->AddRectFilled({pos.x + padding, pedal_y},
                      {pos.x + w - padding, pedal_y + 10.f}, pcol);
    std::string txt = "Pedal CC64=" + std::to_string(cc);
    dl->AddText({pos.x + padding + 4.f, pedal_y + 11.f},
                IM_COL32(200, 200, 200, 255), txt.c_str());
}

} // namespace ithaca::gui
