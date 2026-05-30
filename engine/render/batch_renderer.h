#pragma once
// engine/render/batch_renderer.h
// ------------------------------
// Offline render not do jednoho stereo WAV bez audio device. Pro kazdou notu:
// note-on, drz duration_s, note-off, nech dozvuk tail_s. Pouziva se pro
// testovani prehravani bez HW a jako `make smoke`.

#include <string>
#include <vector>

namespace ithaca {

class Engine;

struct BatchNote {
    int   midi;
    int   velocity;
    float duration_s;
};

// Renderuje noty sekvencne do jednoho WAV (sum vsech, jedna za druhou).
// Vrati pocet uspesne odehranych not (s nenulovym vystupem se nepocita zvlast
// — vraci pocet zpracovanych not). tail_s = dozvuk po note-off.
int renderNotes(Engine& engine, const std::vector<BatchNote>& notes,
                const std::string& out_path, float tail_s = 0.5f);

} // namespace ithaca
