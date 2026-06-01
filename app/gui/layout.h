#pragma once
// app/gui/layout.h — JEDINY zdroj pravdy pro rozmery GUI.
// ----------------------------------------------------------------------------
// Vsechny velikosti (okno, sloupce, vysky radku, paddingy, mezery, widgety)
// jsou pojmenovane konstanty na JEDNOM miste — ladis tady, ne roztrousene po
// panelech (to je slabina icr2, kde jsou rozmery magic numbers v kazdem .cpp).
//
// Scaling: g_scale (default 1.0, volitelne nastaveno z GLFW content-scale pri
// startu). Pouzij S(px) pro skalovany rozmer. Vetsina layoutu pouziva proporce
// ze sirky okna pro sloupce + pevne (skalovane) px pro vysky — stejny mix jako
// icr2, jen centralizovany a laditelny.

namespace ithaca::gui::layout {

// Globalni DPI scale. Nastaveno v main.cpp z glfwGetWindowContentScale().
inline float g_scale = 1.0f;

// Skalovany rozmer v px.
inline float S(float px) { return px * g_scale; }

// -- Laditelne rozmery (base px; projdou pres S() na call-site) --------------
namespace Dims {
    // Okno (default velikost pri prvnim spusteni).
    inline constexpr float win_w = 1280.f;
    inline constexpr float win_h = 820.f;

    // Sloupce hlavni rady. col_bank/col_dsp pevne, stred = zbytek (flex).
    inline constexpr float col_bank = 250.f;
    inline constexpr float col_dsp  = 290.f;

    // Vysky vodorovnych pasem. (Tesne kolem obsahu — zadne prazdne misto dole.)
    inline constexpr float topbar_h = 44.f;    // 1 radek combo/tlacitka + vzduch
    inline constexpr float strip_h  = 100.f;   // stat dlazdice (vetsi cisla) + peak L/R
    inline constexpr float kbd_h    = 100.f;   // klaviatura + popisek
    inline constexpr float log_h    = 80.f;    // LOG minimum (pohlcuje zbytek vysky)
    // Hlavni rada (bank/voice/dsp) se drzi pri obsahu — strop, aby pod slidery
    // nezustaval prazdny prostor a klaviatura sla nahoru. Zbytek pohlti LOG.
    inline constexpr float main_h_max = 235.f;

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
}

// Skalovane gettery (zkratky pro caste pouziti).
inline float padOuter()  { return S(Dims::pad_outer); }
inline float padPanel()  { return S(Dims::pad_panel); }
inline float rowGap()    { return S(Dims::row_gap); }
inline float rowGapS()   { return S(Dims::row_gap_s); }
inline float topbarH()   { return S(Dims::topbar_h); }
inline float stripH()    { return S(Dims::strip_h); }
inline float kbdH()      { return S(Dims::kbd_h); }
inline float logH()      { return S(Dims::log_h); }
inline float colBank()   { return S(Dims::col_bank); }
inline float colDsp()    { return S(Dims::col_dsp); }

} // namespace ithaca::gui::layout
