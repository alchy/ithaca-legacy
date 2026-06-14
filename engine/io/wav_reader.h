#pragma once
// engine/io/wav_reader.h
// ----------------------
// Minimalni WAV reader. Vraci VZDY interleaved stereo float [-1,1] (mono se
// zdvoji do L+R). Podporuje 16/24/32-bit PCM a 32-bit IEEE float. Port
// predlohy z icr (cores/sampler/wav_loader.h), rozdeleny na .h/.cpp a
// doplneny o peekWavInfo() pro budouci streaming (faze 4).

#include <cstdint>
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

// Cte konkretni vyrez WAV souboru → interleaved stereo float. frame_off a
// frame_count jsou ve stereo frames (ne v samplech) — 64-bit, aby streaming
// worker mohl sahnout do multi-GB WAV souboru s velkymi offsety. Vrati WavData
// s frames = min(frame_count, dostupne_do_konce_souboru). Pri offsetu za koncem
// vrati valid=true s frames=0 a prazdnym samples (pro streaming je to OK
// signal "konec"). Pri chybe otevreni/parsovani vrati valid=false.
WavData readWavRange(const std::string& path, int64_t frame_off, int64_t frame_count);

// Prevod jednoho suroveho vzorku na float [-1,1] dle formatu (bits 16/24/32,
// audio_format 1=PCM / 3=IEEE float). Sdileno s pakovanou bankou — dekodovani
// blobu (sample_read.cpp) je tim bit-exact shodne s WAV ctenim.
float wavSampleToFloat(const uint8_t* p, uint16_t bits, uint16_t audio_format);

} // namespace ithaca
