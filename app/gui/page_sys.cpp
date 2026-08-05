// app/gui/page_sys.cpp — SYS: MIDI, audio, uroven logu, RESET, diagnostika ringu.
//
// Vsechno, co se nastavuje jednou a pak uz se toho clovek nedotkne. Proto je
// tady i RESET — na PLAY by se dal trefit omylem uprostred hrani.
#include "pages.h"
#include "app_context.h"
#include "widgets.h"
#include "theme.h"
#include "layout.h"
#include "midi/midi_input.h"
#include "util/log.h"

#include <cstdio>
#include <string>

namespace ithaca::gui {

namespace L = ithaca::gui::layout;
using theme::Colors;
using theme::Fonts;

namespace {

// Radek voleb jako inverzni pole vedle sebe. Na dotyku lepsi nez rozbalovaci
// seznam: jedno klepnuti misto dvou a vzdy je videt cela nabidka.
// Vraci vybrany index (nebo -1 kdyz se neklikalo).
int chipRow(const char* id, ImVec2 pos, float maxw, const char* const* items,
            int n, int cur, float h = 48.f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float px = wdg::fontPx(Fonts::small);
    float x = pos.x;
    int hit = -1;
    for (int i = 0; i < n; ++i) {
        const float w = wdg::textW(Fonts::small, px, items[i]) + 26.f;
        if (x + w > pos.x + maxw) break;
        const bool on = (i == cur);
        if (on) {
            dl->AddRectFilled(ImVec2(x, pos.y), ImVec2(x + w, pos.y + h), Colors::inv_bg);
            dl->AddText(Fonts::small, px, ImVec2(x + 13.f, pos.y + (h - px) * 0.5f),
                        Colors::inv_fg, items[i]);
        } else {
            dl->AddRect(ImVec2(x, pos.y), ImVec2(x + w, pos.y + h), Colors::line);
            dl->AddText(Fonts::small, px, ImVec2(x + 13.f, pos.y + (h - px) * 0.5f),
                        Colors::dim, items[i]);
        }
        ImGui::SetCursorScreenPos(ImVec2(x, pos.y - (L::Dims::touch - h) * 0.5f));
        char bid[40]; std::snprintf(bid, sizeof(bid), "%s_%d", id, i);
        if (ImGui::InvisibleButton(bid, ImVec2(w, L::Dims::touch)) && !on) hit = i;
        x += w + 5.f;
    }
    return hit;
}

void label(ImDrawList* dl, ImVec2 p, const char* t) {
    dl->AddText(Fonts::small, wdg::fontPx(Fonts::small), p, Colors::dimmer, t);
}

} // namespace

void pageSys(AppContext& ctx, const Rect& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& ps = ctx.panels;
    const float px_s = wdg::fontPx(Fonts::small);
    const float lh   = 74.f;      // vyska jednoho radku nastaveni
    float y = r.lo.y;

    if (!ps.midi_ports_scanned) {
        ps.midi_ports = ithaca::MidiInput::listPorts();
        ps.midi_ports_scanned = true;
    }

    // -- MIDI port ---------------------------------------------------------
    label(dl, ImVec2(r.lo.x, y), "MIDI IN");
    {
        // Seznam portu + polozka pro zadny. Otevira se podle JMENA, ne podle
        // indexu do cache: kdyz se zarizeni mezitim odpoji, index ukazuje jinam.
        std::vector<const char*> items;
        items.push_back("(none)");
        for (const auto& s : ps.midi_ports) items.push_back(s.c_str());
        int cur = 0;
        for (size_t i = 0; i < ps.midi_ports.size(); ++i)
            if (ps.midi_ports[i] == ctx.state.midi_port_name) { cur = (int)i + 1; break; }

        const int hit = chipRow("##midi", ImVec2(r.lo.x, y + px_s + 6.f),
                                r.w() - 130.f, items.data(), (int)items.size(), cur);
        if (hit == 0) {
            ctx.midi.close();
            ctx.state.midi_port_name.clear();
        } else if (hit > 0) {
            const std::string want = ps.midi_ports[(size_t)hit - 1];
            const auto live = ithaca::MidiInput::listPorts();
            int idx = -1;
            for (size_t k = 0; k < live.size(); ++k) if (live[k] == want) { idx = (int)k; break; }
            if (idx >= 0) {
                ctx.midi.close();
                ctx.midi.setChannel(ctx.state.midi_channel);   // pred open
                if (ctx.midi.open(ctx.engine, idx)) ctx.state.midi_port_name = want;
            } else {
                log::Logger::default_().log("gui", log::Severity::Warning,
                    "MIDI port disappeared: %s", want.c_str());
                ps.midi_ports = live;
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(r.hi.x - 120.f, y + px_s));
        if (wdg::toggle("##rescan", "RESCAN", false))
            ps.midi_ports = ithaca::MidiInput::listPorts();
    }
    y += lh;

    // -- MIDI kanal --------------------------------------------------------
    label(dl, ImVec2(r.lo.x, y), "CHANNEL");
    {
        static const char* kCh[] = { "OMNI","1","2","3","4","5","6","7","8",
                                     "9","10","11","12","13","14","15","16" };
        const int cur = ctx.state.midi_channel < 0 ? 0 : ctx.state.midi_channel + 1;
        const int hit = chipRow("##ch", ImVec2(r.lo.x, y + px_s + 6.f), r.w(),
                                kCh, IM_ARRAYSIZE(kCh), cur);
        if (hit >= 0) {
            ctx.state.midi_channel = (hit == 0) ? -1 : hit - 1;
            ctx.midi.setChannel(ctx.state.midi_channel);
        }
    }
    y += lh;

    // -- Audio buffer ------------------------------------------------------
    label(dl, ImVec2(r.lo.x, y), "BUFFER");
    {
        static const char* kBuf[] = { "32","64","128","256","512","1024","2048" };
        static const int   kVal[] = {  32,  64,  128,  256,  512,  1024,  2048  };
        int cur = 3;
        for (int i = 0; i < IM_ARRAYSIZE(kVal); ++i)
            if (kVal[i] == ctx.engine.blockSize()) { cur = i; break; }
        const int hit = chipRow("##buf", ImVec2(r.lo.x, y + px_s + 6.f), r.w(),
                                kBuf, IM_ARRAYSIZE(kBuf), cur);
        if (hit >= 0) ctx.setAudioBlockSize(kVal[hit]);
    }
    y += lh;

    // -- Uroven logu -------------------------------------------------------
    label(dl, ImVec2(r.lo.x, y), "LOG");
    {
        static const char* kLv[] = { "debug","info","warn","error","fatal","off" };
        int cur = 1;
        for (int i = 0; i < IM_ARRAYSIZE(kLv); ++i)
            if (ctx.state.log_level == kLv[i]) { cur = i; break; }
        const int hit = chipRow("##lv", ImVec2(r.lo.x, y + px_s + 6.f), r.w(),
                                kLv, IM_ARRAYSIZE(kLv), cur);
        if (hit >= 0) {
            ctx.state.log_level = kLv[hit];
            log::Logger::default_().setMinSeverity(
                log::severity_from_string(ctx.state.log_level.c_str(), log::Severity::Info));
        }
    }
    y += lh;

    // -- Diagnostika streamovani ------------------------------------------
    // Ringy patri sem, ne na PLAY: podle nich se ladi MAX RESONANCE, coz je
    // nastavovaci cinnost, ne vykon.
    char rb[64];
    std::snprintf(rb, sizeof(rb), "MAIN %d/%d    RESO %d/%d",
                  ctx.engine.mainRingsUsed(), ctx.engine.mainRingsTotal(),
                  ctx.engine.resonanceRingsUsed(), ctx.engine.resonanceRingsTotal());
    label(dl, ImVec2(r.lo.x, y), "RINGS");
    dl->AddText(Fonts::ui, wdg::fontPx(Fonts::ui), ImVec2(r.lo.x, y + px_s + 6.f),
                Colors::dim, rb);

    // -- RESET -------------------------------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(r.hi.x - 190.f, r.hi.y - L::Dims::touch));
    if (wdg::toggle("##reset", "RESET PARAMS", false)) {
        // Genericky pres Param::def — MASTER a RESONANCE, ne DSP retezec:
        // smazani celeho retezce jednim klepnutim by bylo destruktivni prekvapeni.
        ctx.engine.setMasterGain(1.f);
        ctx.state.master_gain_db = 0.f;
        ctx.state.release_ms = 200.f;      ctx.engine.setReleaseMs(200.f);
        ctx.state.excite_decay_ms = 5000.f; ctx.engine.setExciteDecayMs(5000.f);
        ctx.state.resonance_gain_db = -12.f; ctx.engine.setResonanceGainDb(-12.f);
        ctx.state.resonance_enabled = true;  ctx.engine.setResonanceEnabled(true);
        ctx.state.max_resonance_voices = 32; ctx.engine.setMaxResonanceVoices(32);
    }
}

} // namespace ithaca::gui
