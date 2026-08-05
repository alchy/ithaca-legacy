// app/gui/page_sys.cpp — SYS: MIDI, audio, uroven logu, uzivatelske defaulty.
//
// Vsechno, co se nastavuje jednou a pak uz se toho clovek nedotkne. Proto je
// tady i RESET — na PLAY by se dal trefit omylem uprostred hrani.
//
// Cela stranka je sazena na ROZTAZENE RADKY (layout::splitRow): kazda volba
// dostane stejne sirokou bunku bez ohledu na delku popisku. Radky pak konci
// na stejne svislici a kazdy cil je stejne velky — pri ovladani prstem se
// netrefujes podle toho, jak dlouhy popisek volba nahodou ma.
#include "pages.h"
#include "app_context.h"
#include "dsp_state.h"
#include "widgets.h"
#include "theme.h"
#include "layout.h"
#include "dsp/dsp_stage.h"
#include "midi/midi_input.h"
#include "util/log.h"

#include <cstdio>
#include <string>
#include <vector>

namespace ithaca::gui {

namespace L = ithaca::gui::layout;
using theme::Colors;
using theme::Fonts;

namespace {

void label(ImDrawList* dl, ImVec2 p, const char* t) {
    dl->AddText(Fonts::small, wdg::fontPx(Fonts::small), p, Colors::dimmer, t);
}

// Rozvrzeni stranky. Pocita se z DOSTUPNE vysky, ne z konstant: vyska zalozek
// se odviji od sirky displeje (ctvercove dlazdice), takze kolik na SYS zbyde
// neni dopredu dane. Bez tohohle by se pri sirsim panelu spodni tlacitka
// prekryla s poslednim radkem voleb.
struct SysMetrics {
    float cell_h;      // vykreslena vyska volby
    float hit_h;       // dotykova zona volby (vyssi, omezena rozteci radku)
    float pitch;       // vyska celeho bloku "popisek + volby"
    float rings_y;     // uctara radku s odectenymi hodnotami
    float btn_y;       // horni hrana radku tlacitek
};

SysMetrics sysMetrics(const Rect& r) {
    const float px    = wdg::fontPx(Fonts::small);
    const float np    = wdg::fontPx(Fonts::ui);
    const float btn_h = L::Dims::touch;
    const float rings_h = px + np + 6.f;

    SysMetrics m{};
    m.btn_y   = r.hi.y - btn_h;
    m.rings_y = m.btn_y - L::Dims::gap - rings_h;

    constexpr int kRows = 4;                       // MIDI IN, CHANNEL, BUFFER, LOG
    m.pitch  = (m.rings_y - L::Dims::gap - r.lo.y) / (float)kRows;
    m.cell_h = std::clamp(m.pitch - px - 6.f - L::Dims::gap_s, 40.f, 56.f);
    // Zony se nesmi dotknout sousedniho bloku, jinak klepnuti padne jinam.
    m.hit_h  = std::min(L::Dims::touch, m.pitch - 2.f);
    return m;
}

// Popisek + roztazeny radek voleb pod nim. `reserve_w` odkroji misto vpravo pro
// tlacitko, ktere ma stat ve STEJNEM radku (RESCAN u MIDI IN) — jinak by viselo
// na jine uctare nez volby vedle nej.
// Vraci vybrany index nebo -1 a posune `y` na dalsi blok.
int settingRow(const char* id, const Rect& r, const SysMetrics& m, float& y,
               const char* lab, const char* const* items, int n, int cur,
               float reserve_w = 0.f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float px = wdg::fontPx(Fonts::small);
    label(dl, ImVec2(r.lo.x, y), lab);
    const float w = r.w() - reserve_w;
    const int hit = wdg::chipRow(id, ImVec2(r.lo.x, y + px + 6.f), w, items, n, cur,
                                 m.cell_h, m.hit_h);
    y += m.pitch;
    return hit;
}

} // namespace

void pageSys(AppContext& ctx, const Rect& r,
             ithaca::dsp::IParamPage** pages, int n_pages) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& ps = ctx.panels;
    const float px_s = wdg::fontPx(Fonts::small);
    const SysMetrics m = sysMetrics(r);
    float y = r.lo.y;

    if (!ps.midi_ports_scanned) {
        ps.midi_ports = ithaca::MidiInput::listPorts();
        ps.midi_ports_scanned = true;
    }

    // -- MIDI port + RESCAN v jednom radku ---------------------------------
    {
        // Seznam portu + polozka pro zadny. Otevira se podle JMENA, ne podle
        // indexu do cache: kdyz se zarizeni mezitim odpoji, index ukazuje jinam.
        std::vector<const char*> items;
        items.push_back("(none)");
        for (const auto& s : ps.midi_ports) items.push_back(s.c_str());
        int cur = 0;
        for (size_t i = 0; i < ps.midi_ports.size(); ++i)
            if (ps.midi_ports[i] == ctx.state.midi_port_name) { cur = (int)i + 1; break; }

        const float rescan_w = 150.f;
        const float row_y = y + px_s + 6.f;   // stejna uctara jako volby vedle
        const int hit = settingRow("##midi", r, m, y, "MIDI IN",
                                   items.data(), (int)items.size(), cur,
                                   rescan_w + L::Dims::gap_s);

        ImGui::SetCursorScreenPos(ImVec2(r.hi.x - rescan_w, row_y));
        if (wdg::button("##rescan", "RESCAN", rescan_w))
            ps.midi_ports = ithaca::MidiInput::listPorts();

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
    }

    // -- MIDI kanal --------------------------------------------------------
    {
        static const char* kCh[] = { "OMNI","1","2","3","4","5","6","7","8",
                                     "9","10","11","12","13","14","15","16" };
        const int cur = ctx.state.midi_channel < 0 ? 0 : ctx.state.midi_channel + 1;
        const int hit = settingRow("##ch", r, m, y, "CHANNEL", kCh, IM_ARRAYSIZE(kCh), cur);
        if (hit >= 0) {
            ctx.state.midi_channel = (hit == 0) ? -1 : hit - 1;
            ctx.midi.setChannel(ctx.state.midi_channel);
        }
    }

    // -- Audio buffer ------------------------------------------------------
    {
        static const char* kBuf[] = { "32","64","128","256","512","1024","2048" };
        static const int   kVal[] = {  32,  64,  128,  256,  512,  1024,  2048  };
        int cur = 3;
        for (int i = 0; i < IM_ARRAYSIZE(kVal); ++i)
            if (kVal[i] == ctx.engine.blockSize()) { cur = i; break; }
        const int hit = settingRow("##buf", r, m, y, "BUFFER", kBuf, IM_ARRAYSIZE(kBuf), cur);
        if (hit >= 0) ctx.setAudioBlockSize(kVal[hit]);
    }

    // -- Uroven logu -------------------------------------------------------
    {
        static const char* kLv[] = { "debug","info","warn","error","fatal","off" };
        int cur = 1;
        for (int i = 0; i < IM_ARRAYSIZE(kLv); ++i)
            if (ctx.state.log_level == kLv[i]) { cur = i; break; }
        const int hit = settingRow("##lv", r, m, y, "LOG", kLv, IM_ARRAYSIZE(kLv), cur);
        if (hit >= 0) {
            ctx.state.log_level = kLv[hit];
            log::Logger::default_().setMinSeverity(
                log::severity_from_string(ctx.state.log_level.c_str(), log::Severity::Info));
        }
    }

    // -- Odectene hodnoty: ringy vlevo, stav defaultu vpravo ----------------
    // Jeden radek pro obe: jsou to udaje ke cteni, ne ovladani, takze si
    // nezaslouzi kazdy vlastni pas. Ringy patri sem, ne na PLAY — podle nich
    // se ladi MAX RESONANCE, coz je nastavovaci cinnost, ne vykon.
    const bool have_def = !ctx.state.defaults.empty();
    {
        const float ry = m.rings_y;
        char rb[64];
        std::snprintf(rb, sizeof(rb), "MAIN %d/%d    RESO %d/%d",
                      ctx.engine.mainRingsUsed(), ctx.engine.mainRingsTotal(),
                      ctx.engine.resonanceRingsUsed(), ctx.engine.resonanceRingsTotal());
        label(dl, ImVec2(r.lo.x, ry), "RINGS");
        dl->AddText(Fonts::ui, wdg::fontPx(Fonts::ui), ImVec2(r.lo.x, ry + px_s + 6.f),
                    Colors::dim, rb);

        const char* lab_r = "PARAM DEFAULTS";
        const char* val_r = have_def ? "USER" : "FACTORY";
        dl->AddText(Fonts::small, px_s,
                    ImVec2(r.hi.x - wdg::textW(Fonts::small, px_s, lab_r), ry),
                    Colors::dimmer, lab_r);
        const float np = wdg::fontPx(Fonts::ui);
        dl->AddText(Fonts::ui, np,
                    ImVec2(r.hi.x - wdg::textW(Fonts::ui, np, val_r), ry + px_s + 6.f),
                    have_def ? Colors::ink : Colors::dim, val_r);
    }

    // -- Uzivatelske defaulty ----------------------------------------------
    // SAVE AS DEFAULT ulozi VSECHNY stranky parametru (MASTER, RESONANCE
    // i cely DSP retezec) do state.json jako sekci "defaults". RESET PARAMS
    // pak vraci prave na ne — na to, co si uzivatel oznacil za spravne
    // naladeni, ne na tovarni Param::def, ktery o jeho bance nic nevi.
    {
        const bool have = have_def;
        const L::Row row = L::splitRow(ImVec2(r.lo.x, m.btn_y), r.w(),
                                       L::Dims::touch, 2);

        ImGui::SetCursorScreenPos(row.at(0));
        if (wdg::button("##savedef", "SAVE AS DEFAULT", row.cell))
            snapshotPages(ctx.state.defaults, pages, n_pages);

        ImGui::SetCursorScreenPos(row.at(1));
        if (wdg::button("##reset", "RESET PARAMS", row.cell)) {
            if (have) {
                // Vlastni snapshot smi vratit i DSP retezec a RESONANCE LAYER:
                // neni to destruktivni prekvapeni, je to navrat k tomu, co si
                // uzivatel sam ulozil.
                applyPagesState(ctx.state.defaults, pages, n_pages);
            } else {
                // Tovarni cesta. DSP retezec se ZAMERNE nechava byt — smazani
                // celeho retezce jednim klepnutim by prekvapilo. Poradi stranek
                // (0 = MASTER, 1 = RESONANCE, dal DSP stage) urcuje main.cpp
                // a stejny predpoklad dela i dispatch v screen.cpp.
                constexpr int kFactoryPages = 2;
                for (int i = 0; i < n_pages && i < kFactoryPages; ++i)
                    pages[i]->resetToDefaults();
            }
        }
    }
}

} // namespace ithaca::gui
