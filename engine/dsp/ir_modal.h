#pragma once
// engine/dsp/ir_modal.h — syntetic soundboard body IR (modalni model kalibrovany
// na Chabassier/RR_9530): suma tlumenych modu f_n s decay tau=2/f_ve(f_n).
#include <vector>
namespace ithaca::dsp {
enum class IrPreset { BodySoft, BodyBright };
// Modalni tlumeni desky (RR_9530): f_ve(f) = 2e-5 f^2 + 7e-2 f. Per-mod decay tau=2/f_ve.
float soundboardDampingFve(float f);
// Vygeneruj body IR (delka <= max_len) pro engine sr.
std::vector<float> generateModalIr(IrPreset preset, float sr, int max_len);
}
