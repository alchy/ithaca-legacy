#pragma once
// app/gui/layout.h — JEDINY zdroj pravdy pro rozmery GUI.
// ----------------------------------------------------------------------------
// Cil je 7" dotykovy panel 1280x720 zabudovany v nastroji (~210 DPI).
// Z toho plyne jedina tvrda podminka, ze ktere se odvozuje skoro vsechno
// ostatni: prst potrebuje ~9 mm, coz je na tomhle panelu ~74 px. Zadny
// interaktivni prvek nesmi byt mensi.
//
// DPI: rozmery se NESKALUJI. ImGui pracuje v logickych bodech a prepocet na
// fyzicke pixely resi backend (io.DisplayFramebufferScale z GLFW), takze
// nasobit rozmery content-scalem by scale aplikovalo DVAKRAT. g_scale slouzi
// VYHRADNE k rasterizaci pisem ve fyzickem rozliseni (viz theme.h::load_fonts).

namespace ithaca::gui::layout {

// Globalni DPI scale z glfwGetWindowContentScale(). Cte ho jen load_fonts.
inline float g_scale = 1.0f;

namespace Dims {
    // Panel.
    inline constexpr float win_w = 1280.f;
    inline constexpr float win_h = 720.f;

    // Minimalni dotykovy cil. Vsechno interaktivni se od nej odviji.
    inline constexpr float touch = 74.f;

    // Ramecek kolem plochy — mrtva zona, aby se u kraje panelu nedalo omylem
    // trefit ovladani. Neni to ozdoba, je to funkcni odsazeni.
    inline constexpr float bezel = 16.f;

    // Vodorovna pasma.
    inline constexpr float tab_h   = touch;   // radek zalozek (nahore)
    inline constexpr float subtab_h= 56.f;    // druhy radek (jen DSP) — mensi, ale nad prst
    inline constexpr float lamp_h  = 26.f;    // radek kontrolek nad spodnim okrajem
    inline constexpr float foot_h  = 34.f;    // stitek + SR/buffer dole

    // Seznam (vytah v PLAY, banky, log).
    inline constexpr float row_h      = 60.f;   // radek seznamu
    inline constexpr float row_big_h  = 76.f;   // zvyrazneny radek

    // Parametr = popisek + track.
    inline constexpr float param_lab  = 24.f;
    inline constexpr float param_trk  = touch;
    inline constexpr float param_gap  = 10.f;
    inline constexpr float param_h    = param_lab + param_trk + param_gap;

    // Mezery.
    inline constexpr float gap    = 10.f;
    inline constexpr float gap_s  = 6.f;

    // Metry a bary.
    inline constexpr float bar_h  = 14.f;   // sustain / peak
    inline constexpr float tick   = 2.f;    // ryska prahu

    // Kolik pixelu musi prst ujet, nez se z klepnuti stane tah. Bez tohoto
    // prahu by kazde klepnuti bylo mikrotazeni a hodnota by uskocila.
    inline constexpr float drag_slop = 8.f;
}

} // namespace ithaca::gui::layout
