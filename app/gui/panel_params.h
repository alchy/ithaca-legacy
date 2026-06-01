// app/gui/panel_params.h - genericky renderer parametricke stranky (IParamPage).
#pragma once
#include "dsp/dsp_stage.h"
namespace ithaca::gui {
struct AppContext;
void renderParamPage(AppContext& ctx, ithaca::dsp::IParamPage& page);
}
