// app/gui/panel_topbar.h - top bar (bank dropdown, MIDI dropdown, master slider).
#pragma once
#include "dsp/dsp_stage.h"

namespace ithaca::gui {
struct AppContext;
// Render top bar do ImGui frame; vyska ~36 px na celou sirku okna.
// Modifikuje ctx.state pri zmenach + vola setter engine / loadBank / midi open.
// reset_pages/n_reset = stranky, ktere resetuje tlacitko RESET (MASTER +
// RESONANCE). DSP chain se zamerne neresetuje — uzivatel si ho ladi zvlast
// a smazani celeho retezce jednim tlacitkem by bylo destruktivni prekvapeni.
void renderTopBar(AppContext& ctx, ithaca::dsp::IParamPage** reset_pages, int n_reset);
}
