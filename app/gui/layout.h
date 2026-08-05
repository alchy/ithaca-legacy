#pragma once
// app/gui/layout.h — JEDINY zdroj pravdy pro rozmery GUI.
// ----------------------------------------------------------------------------
// Vsechny velikosti (okno, sloupce, vysky radku, paddingy, mezery, widgety)
// jsou pojmenovane konstanty na JEDNOM miste — ladis tady, ne roztrousene po
// panelech (to je slabina icr2, kde jsou rozmery magic numbers v kazdem .cpp).
//
// DPI: rozmery se NESKALUJI. ImGui pracuje v logickych bodech a o prepocet na
// fyzicke pixely se stara backend (io.DisplayFramebufferScale z GLFW), takze
// nasobit rozmery content-scalem by scale aplikovalo DVAKRAT. g_scale slouzi
// VYHRADNE k rasterizaci fontu ve fyzickem rozliseni (viz theme.h::load_fonts,
// main.cpp nastavuje io.FontGlobalScale = 1/g_scale).
// Drive tu zila i funkce S(px) a sada skalovanych getteru (padOuter(), colBank(),
// ...) — nikde se nevolaly a jejich pouziti by prave to dvoji skalovani zpusobilo.
// Odstraneny, aby nesvadely.

namespace ithaca::gui::layout {

// Globalni DPI scale. Nastaveno v main.cpp z glfwGetWindowContentScale().
// Cte ho jen theme.h::load_fonts — na rozmery se NEAPLIKUJE, viz vyse.
inline float g_scale = 1.0f;

// -- Laditelne rozmery (logicke px) ------------------------------------------
namespace Dims {
    // Okno (default velikost pri prvnim spusteni). HW cilovy display 1280x720.
    inline constexpr float win_w = 1280.f;
    inline constexpr float win_h = 720.f;

    // Sloupce hlavni rady. col_bank/col_dsp pevne, stred = zbytek (flex).
    inline constexpr float col_bank = 250.f;
    inline constexpr float col_dsp  = 290.f;

    // Vysky vodorovnych pasem. (Tesne kolem obsahu — zadne prazdne misto dole.)
    inline constexpr float topbar_h = 44.f;    // 1 radek combo/tlacitka + vzduch
    inline constexpr float strip_h  = 100.f;   // stat dlazdice (vetsi cisla) + peak L/R
    inline constexpr float kbd_h    = 100.f;   // klaviatura + popisek
    inline constexpr float log_h    = 80.f;    // LOG minimum (pohlcuje zbytek vysky)
    // Hlavni rada (bank/voice/dsp) se drzi pri obsahu — strop, aby pod slidery
    // nezustaval prazdny prostor. Vetsi z config stranek (VOICE = 5 slideru)
    // se musi pohodlne vejit bez stlaceni; zbytek vysky pohlti LOG.
    inline constexpr float main_h_max = 280.f;

    // Padding / mezery.
    inline constexpr float pad_outer = 20.f;   // vnejsi okraj okna
    inline constexpr float pad_panel = 20.f;   // vnitrni padding panelu
    inline constexpr float gap_col   = 0.f;    // mezi sloupci (delic je hairline/tick)
    inline constexpr float row_gap   = 10.f;   // vertikalni mezera mezi pasmy/prvky
    inline constexpr float row_gap_s = 8.f;    // mala vertikalni mezera (label↔control)

    // Widgety.
    inline constexpr float slider_h     = 28.f;  // cely radek slideru (track+grab)
    inline constexpr float slider_track = 3.f;   // tloustka linky tracku
    inline constexpr float slider_grab  = 12.f;  // vyska zarazky
    inline constexpr float bar_h        = 9.f;   // sustain/peak bar
    inline constexpr float kbd_keys_h   = 56.f;  // vyska kláves (zbytek = popisek)
    inline constexpr float tick_len     = 10.f;  // grid ryska
    inline constexpr float lamp_gap     = 16.f;  // mezi MIDI lampami

    // Vnitrni odsazeni obsahu panelu od jeho leveho/praveho okraje. Mensi nez
    // pad_panel — panely stoji tesne vedle sebe a plny padding by je opticky
    // roztrhl. (Drive zila ta samá 14.f zvlast v panel_bank i panel_indicators.)
    inline constexpr float pad_inset    = 14.f;

    // Top bar: sirky ovladacich prvku.
    inline constexpr float tb_midi_w    = 210.f; // MIDI IN combo
    inline constexpr float tb_ch_w      = 90.f;  // CHANNEL combo
    inline constexpr float tb_buffer_w  = 72.f;  // BUFFER combo
    inline constexpr float tb_log_w     = 120.f; // LOG level combo
    inline constexpr float tb_gap       = 18.f;  // mezera mezi skupinami
}

} // namespace ithaca::gui::layout
