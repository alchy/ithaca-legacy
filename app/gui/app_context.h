// app/gui/app_context.h - drzi Engine + AudioDevice + MidiInput + log buffer
//                          + persistovany GuiState. Lifecycle = jeden ithaca-gui run.
//
// Vlastnictvi: AppContext je single-owner vsech tezkych objektu (engine, audio
// device, midi). main() vola initFromState() pred render loopem a shutdown()
// pred destrukci ImGui/GLFW. Pripadne dalsi panely v dalsich tascich budou cist
// ctx.engine / ctx.log_buf / ctx.state pres const&.
#pragma once

#include "engine.h"
#include "io/audio_device.h"
#include "midi/midi_input.h"

#include "log_subscriber.h"
#include "persistence.h"

#include <memory>

namespace ithaca::gui {

struct AppContext {
    ithaca::Engine                       engine;
    std::unique_ptr<ithaca::AudioDevice> audio;
    ithaca::MidiInput                    midi;
    LogRingBuffer                        log_buf;
    GuiState                             state;

    // Init: subscriber pripoji, engine init z state, audio start, optional
    // bank load + MIDI open. Vraci true pri uspechu (engine init musi projit;
    // bank/MIDI failures jsou jen warning).
    bool initFromState(const GuiState& s);

    // Cisty shutdown: midi close, audio stop, subscriber clear. Volat pred
    // destrukci ImGui/GLFW. Engine destruktor sam uvolni voice/stream/resonance.
    void shutdown();
};

} // namespace ithaca::gui
