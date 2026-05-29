// engine/render/batch_renderer.cpp — viz batch_renderer.h.
#include "render/batch_renderer.h"

#include "engine.h"
#include "io/wav_writer.h"
#include "util/log.h"

#include <algorithm>
#include <vector>

namespace ithaca {

int renderNotes(Engine& engine, const std::vector<BatchNote>& notes,
                const std::string& out_path, float tail_s) {
    const int   sr    = engine.sampleRate();
    const int   block = 512;
    std::vector<float> out;                              // interleaved stereo akumulator
    std::vector<float> bl(block), br(block);

    int done = 0;
    for (const auto& n : notes) {
        int sustain_frames = (int)(n.duration_s * (float)sr);
        int tail_frames    = (int)(tail_s * (float)sr);

        engine.allNotesOff();
        engine.noteOn(n.midi, n.velocity);

        // Sustain faze.
        for (int s = 0; s < sustain_frames; ) {
            int k = (std::min)(block, sustain_frames - s);
            std::fill(bl.begin(), bl.begin() + k, 0.f);
            std::fill(br.begin(), br.begin() + k, 0.f);
            engine.processBlock(bl.data(), br.data(), k);
            for (int i = 0; i < k; ++i) { out.push_back(bl[i]); out.push_back(br[i]); }
            s += k;
        }
        engine.noteOff(n.midi);
        // Tail (dozvuk).
        for (int s = 0; s < tail_frames; ) {
            int k = (std::min)(block, tail_frames - s);
            std::fill(bl.begin(), bl.begin() + k, 0.f);
            std::fill(br.begin(), br.begin() + k, 0.f);
            engine.processBlock(bl.data(), br.data(), k);
            for (int i = 0; i < k; ++i) { out.push_back(bl[i]); out.push_back(br[i]); }
            s += k;
        }
        done++;
    }

    auto& L = log::Logger::default_();
    if (!writeWavStereo16(out_path, out, sr)) {
        L.log("render", log::Severity::Error, "Nelze zapsat WAV: %s", out_path.c_str());
        return 0;
    }
    L.log("render", log::Severity::Info, "Render: %d not → %s (%zu frames)",
          done, out_path.c_str(), out.size() / 2);
    return done;
}

} // namespace ithaca
