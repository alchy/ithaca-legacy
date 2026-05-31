// app/gui/panel_diag.h - diagnosticky panel (live metricy).
#pragma once
namespace ithaca::gui {
struct AppContext;
// Render diag panel na zadane pozici a velikosti. Ctie atomic getter z Engine.
void renderDiagPanel(AppContext& ctx, float x, float y, float w, float h);
}
