#pragma once
// app/gui/splash.h — uvodni obrazovka.
// ----------------------------------------------------------------------------
// Kryje SKUTECNOU praci: engine init, audio, MIDI a natazeni prvni banky.
// Proto na ni nikde neni umele cekani — koncí, az dobehne load, ne po pevnem
// case. Kdyz se banka natahne rychle, zkrati se s ni.
#include "imgui.h"

namespace ithaca::gui {

struct AppContext;

// Vraci true, dokud se ma splash kreslit (a tedy potlacit normalni panel).
// Vola se kazdy frame; sama si drzi cas i prolnuti.
bool renderSplash(AppContext& ctx, float W, float H);

} // namespace ithaca::gui
