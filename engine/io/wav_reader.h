#pragma once
// engine/io/wav_reader.h
// ----------------------
// Minimalni WAV reader. Vraci VZDY interleaved stereo float [-1,1] (mono se
// zdvoji do L+R). Podporuje 16/24/32-bit PCM a 32-bit IEEE float. Port
// predlohy z icr (cores/sampler/wav_loader.h), rozdeleny na .h/.cpp a
// doplneny o peekWavInfo() pro budouci streaming (faze 4).

#include <string>
#include <vector>

namespace ithaca {

// Plna data samplu v RAM.
struct WavData {
    std::vector<float> samples;   // interleaved stereo [L,R,L,R,...]
    int  frames      = 0;         // pocet stereo frames
    int  sample_rate = 0;
    bool valid       = false;
};

// Jen hlavickove informace (bez nacteni dat) — levne, pro rozhodovani o
// streamingu / rozpoctu pameti.
struct WavInfo {
    int  frames      = 0;
    int  sample_rate = 0;
    int  channels    = 0;         // puvodni pocet kanalu v souboru (1 nebo 2)
    bool valid       = false;
};

// Nacte cely WAV do RAM jako interleaved stereo float. Pri chybe vrati
// WavData s valid=false.
WavData readWav(const std::string& path);

// Precte jen fmt+data hlavicku. Pri chybe valid=false.
WavInfo peekWavInfo(const std::string& path);

} // namespace ithaca
