#pragma once
// engine/io/sample_read.h
// Dispatcher cteni vzorku: bezny WAV soubor (deleguje na readWavRange) NEBO
// region v pakovane .ithaca bance (pread z blobu + dekodovani pres
// wavSampleToFloat). Jednotna semantika s readWavRange: interleaved stereo
// float vc. mono→stereo zdvojeni; EOF (frame_off >= frames) = valid s 0
// frames; chyba = valid=false.

#include "io/wav_reader.h"
#include "sample/sample_types.h"

namespace ithaca {

WavData readSampleRange(const SampleFile& file, int64_t frame_off,
                        int64_t frame_count);

// Bajty na vzorek pro kSampleFmt* kod; 0 = neznamy format.
int sampleFormatBytes(uint16_t sample_format);

} // namespace ithaca
