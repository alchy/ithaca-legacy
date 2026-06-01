#pragma once
// app/gui/panel_config.h - CONFIG selektor (pravy sloupec, misto DSP RACK).
// Seznam IParamPage* (VOICE + DSP stage); LED = enabled(); klik nastavi selected.
#include "dsp/dsp_stage.h"

namespace ithaca::gui {
struct AppContext;
// pages: pole IParamPage*, n jejich pocet, selected in/out (index vybrane stranky).
void renderConfigPanel(AppContext& ctx, ithaca::dsp::IParamPage** pages, int n, int& selected);
}
