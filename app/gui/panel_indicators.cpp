#include "panel_indicators.h"
#include "app_context.h"
#include "widgets.h"
#include "imgui.h"
namespace ithaca::gui {
void renderIndicatorStrip(AppContext& ctx, float, float) { wdg::Eyebrow("MIDI / DIAG / PEAK"); (void)ctx; }
} // namespace ithaca::gui
