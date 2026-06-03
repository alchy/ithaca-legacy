#pragma once
// engine/util/denormals.h
// -----------------------
// Flush-to-zero (FTZ) + denormals-are-zero (DAZ) na AKTUALNIM vlakne. Denormalni
// (subnormalni) floaty vznikaji u exponencialne doznivajicich signalu (release
// ocasky, decay rezonance, IIR stav Enhanceru/AGC/limiteru) a na x86 i ARM padaji
// do pomale (mikrokodove) cesty — 10x-100x pomalejsi → CPU spike → underrun
// "z niceho". FTZ/DAZ to resi: denormaly se chovaji jako 0 (−760 dB, neslysitelne;
// standard v audio DSP). Je to PER-VLAKNO FPU stav → volat na audio threadu.
//
// Cross-platform; na neznamych architekturach no-op (korektnost zachovana).

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  #include <xmmintrin.h>   // _MM_SET_FLUSH_ZERO_MODE (FTZ)
  #include <pmmintrin.h>   // _MM_SET_DENORMALS_ZERO_MODE (DAZ)
  #define ITHACA_DENORMALS_X86 1
#elif defined(__aarch64__) || defined(__arm__)
  #include <cstdint>
  #define ITHACA_DENORMALS_ARM 1
#endif

namespace ithaca {

// Zapne FTZ+DAZ na vlakne, ze ktereho je volana. Idempotentni, levne. Volat
// jednou per audio thread (drzi se pro cele vlakno).
inline void enableFlushDenormals() noexcept {
#if defined(ITHACA_DENORMALS_X86)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(ITHACA_DENORMALS_ARM)
  #if defined(__aarch64__)
    // FPCR bit 24 = FZ (flush-to-zero; flushuje denorm. vysledky i vstupy).
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ull << 24);
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
  #else
    // ARMv7 (32-bit): VFP FPSCR bit 24 = FZ.
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
  #endif
#else
    // Neznama architektura → no-op.
#endif
}

} // namespace ithaca
