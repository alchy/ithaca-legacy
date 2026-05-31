// app/gui/panel_topbar.h - top bar (bank dropdown, MIDI dropdown, master slider).
#pragma once
namespace ithaca::gui {
struct AppContext;
// Render top bar do ImGui frame; vyska ~36 px na celou sirku okna.
// Modifikuje ctx.state pri zmenach + vola setter engine / loadBank / midi open.
void renderTopBar(AppContext& ctx);
}
