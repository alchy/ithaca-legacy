// app/gui/panel_log.h - log strip panel: scrollable list poslednich eventu z LogRingBuffer.
#pragma once
namespace ithaca::gui {
struct AppContext;
void renderLogPanel(AppContext& ctx, float x, float y, float w, float h);
}
