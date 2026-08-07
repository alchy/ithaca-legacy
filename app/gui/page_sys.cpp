// app/gui/page_sys.cpp — SYS: MIDI port, MIDI kanaly, audio buffer, defaulty.
//
// Vsechno, co se nastavuje jednou a pak uz se toho clovek nedotkne. Proto je
// tady i RESET — na PLAY by se dal trefit omylem uprostred hrani.
//
// Co tu ZAMERNE neni:
//   - uroven logu sedi na strance LOG, kde potreba ji zmenit vlastne vznika
//   - stav ringu a stav defaultu se odecitaji za hrani, takze patri na PLAY
// Uvolnena vyska padne vhod MIDI kanalum, ktere jsou po dvou radkach.
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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ithaca::gui {

namespace L = ithaca::gui::layout;
using theme::Colors;
using theme::Fonts;

namespace {

// Kanaly se sazi na DVE radky po osmi. Na jedne by pri uzsim panelu byly bunky
// uzsi nez cokoli jineho na strance a radek by pretekal do BUFFERu pod nim.
constexpr int kChanRows   = 2;
constexpr int kChanPerRow = 8;

void label(ImDrawList* dl, ImVec2 p, const char* t) {
    dl->AddText(Fonts::small, wdg::fontPx(Fonts::small), p, Colors::dimmer, t);
}

// Rozvrzeni stranky. Pocita se z DOSTUPNE vysky, ne z konstant: vyska zalozek
// se odviji od sirky displeje (ctvercove dlazdice), takze kolik na SYS zbyde
// neni dopredu dane. Bez tohohle by spodni tlacitka prekryla posledni radek.
struct SysMetrics {
    float cell_h;      // vykreslena vyska volby
    float hit_h;       // dotykova zona volby (vyssi, omezena rozteci radku)
    float unit;        // rozteC jedne radky voleb
    float lab_h;       // popisek nad blokem
    float btn_y;       // horni hrana radku tlacitek
};

SysMetrics sysMetrics(const Rect& r) {
    SysMetrics m{};
    m.lab_h = wdg::fontPx(Fonts::small) + 6.f;

    // Odzdola pas tlacitek, zbytek jsou tri bloky voleb.
    L::Band band{r};
    const Rect btns = band.takeBottom(L::Dims::touch);
    m.btn_y = btns.lo.y;

    // Tri bloky (MIDI IN, CHANNEL, BUFFER), z toho CHANNEL ma dve radky voleb.
    constexpr int kBlocks   = 3;
    constexpr int kChipRows = 1 + kChanRows + 1;
    const float avail = band.h();
    m.unit = (avail - (float)kBlocks * m.lab_h
                    - (float)kBlocks * L::Dims::gap_s) / (float)kChipRows;
    m.cell_h = std::clamp(m.unit - L::Dims::gap_s, 36.f, 56.f);
    // Zona se nesmi dotknout sousedni radky, jinak klepnuti padne jinam.
    m.hit_h  = std::min(L::Dims::touch, m.unit - 2.f);
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
    label(dl, ImVec2(r.lo.x, y), lab);
    const int hit = wdg::chipRow(id, ImVec2(r.lo.x, y + m.lab_h),
                                 r.w() - reserve_w, items, n, cur,
                                 m.cell_h, m.hit_h);
    y += m.lab_h + m.unit + L::Dims::gap_s;
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

        const float rescan_w = L::Dims::act_w;
        const float row_y = y + m.lab_h;   // stejna uctara jako volby vedle
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
                ctx.midi.setChannelMask(ctx.state.midi_channel_mask);   // pred open
                if (ctx.midi.open(ctx.engine, idx)) ctx.state.midi_port_name = want;
            } else {
                log::Logger::default_().log("gui", log::Severity::Warning,
                    "MIDI port disappeared: %s", want.c_str());
                ps.midi_ports = live;
            }
        }
    }

    // -- MIDI kanaly: dve radky po osmi, nezavisle prepinatelne -------------
    // OMNI uz neni polozka nabidky — je to proste stav, kdy sviti vsech
    // sestnact, a uzivatel si ho naklika. Dve veci tim ziskame: jde nastavit
    // libovolna PODMNOZINA kanalu (treba jen 1 a 3), coz jediny volic neumel,
    // a nabidka se vejde i na uzsi panel, kde drive pretekala do BUFFERu.
    {
        static const char* kCh[16] = { "1","2","3","4","5","6","7","8",
                                       "9","10","11","12","13","14","15","16" };
        label(dl, ImVec2(r.lo.x, y), "CHANNEL");

        const uint16_t mask = ctx.state.midi_channel_mask;
        int on = 0;
        for (int i = 0; i < 16; ++i) on += (mask >> i) & 1u;

        // Stav masky vpravo od popisku. Bez nej by nebylo poznat, ze vsech
        // sestnact znamena OMNI — a hlavne ze zadny znamena ticho, coz je
        // legitimni, ale snadno omylem nastavitelny stav.
        char note[32];
        ImU32 note_c = Colors::dim;
        if (on == 16)     std::snprintf(note, sizeof(note), "ALL (OMNI)");
        else if (on == 0) { std::snprintf(note, sizeof(note), "NONE - MIDI MUTED");
                            note_c = Colors::clip_lo; }
        else              std::snprintf(note, sizeof(note), "%d OF 16", on);
        dl->AddText(Fonts::small, px_s,
                    ImVec2(r.hi.x - wdg::textW(Fonts::small, px_s, note), y),
                    note_c, note);

        int hit = -1;
        for (int rw = 0; rw < kChanRows; ++rw) {
            char id[16]; std::snprintf(id, sizeof(id), "##ch%d", rw);
            const int h = wdg::chipRowMask(id,
                ImVec2(r.lo.x, y + m.lab_h + (float)rw * m.unit), r.w(),
                kCh + rw * kChanPerRow, kChanPerRow, mask, rw * kChanPerRow,
                m.cell_h, m.hit_h);
            if (h >= 0) hit = rw * kChanPerRow + h;
        }
        if (hit >= 0) {
            ctx.state.midi_channel_mask = (uint16_t)(mask ^ (1u << hit));
            ctx.midi.setChannelMask(ctx.state.midi_channel_mask);
        }
        y += m.lab_h + (float)kChanRows * m.unit + L::Dims::gap_s;
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

    // -- Profily -----------------------------------------------------------
    // Nastroj zna dva profily:
    //
    //   FACTORY  zapeceny v binarce (Param::def na kazde strance). Nemenny,
    //            nezavisly na state.json — zachranna sit, na kterou se da
    //            vzdy vratit.
    //   USER     to, jak ma nastroj nastaveny uzivatel. Uklada se do
    //            state.json jako sekce "defaults" a pokryva VSECHNY stranky
    //            parametru vcetne DSP retezce.
    //
    // USER profil se uklada SAM pri ukonceni programu (viz main.cpp), takze
    // cim nastroj vypnes, s tim ho zase zapnes. Tlacitko tady dela totez, jen
    // hned — hodi se, kdyz si chces stav pojistit jeste pred hranim.
    //
    // Na kterem profilu nastroj jede, sviti ve stitku v paticce.
    {
        const L::Row row = L::splitRow(ImVec2(r.lo.x, m.btn_y), r.w(),
                                       L::Dims::touch, 2);

        ImGui::SetCursorScreenPos(row.at(0));
        if (wdg::button("##setuser", "SET CURRENT AS USER PROFILE", row.cell))
            snapshotPages(ctx.state.defaults, pages, n_pages);

        ImGui::SetCursorScreenPos(row.at(1));
        if (wdg::button("##factory", "RESET TO FACTORY PROFILE", row.cell)) {
            // Cely retezec vcetne DSP: tlacitko rika FACTORY PROFILE, takze
            // polovicaty reset by lhal.
            for (int i = 0; i < n_pages; ++i) pages[i]->resetToDefaults();
            // A rovnou i USER profil — jinak by ho pri ukonceni prepsal
            // automaticky uklad a uzivatel by mel v souboru dve ruzne verze
            // podle toho, jestli mezitim neco zmenil. Takhle je stav po
            // kliknuti jednoznacny: obe kopie jsou tovarni.
            ctx.state.defaults.clear();
            snapshotPages(ctx.state.defaults, pages, n_pages);
        }
    }
}

} // namespace ithaca::gui
