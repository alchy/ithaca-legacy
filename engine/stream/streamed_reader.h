#pragma once
// engine/stream/streamed_reader.h
// -------------------------------
// Sdileny streaming ctec pro Voice a ResonanceVoice (spec 2026-06-10 cast B):
// vlastnictvi ringu, lo/hi interpolacni okno, sev z predchoziho RAM regionu
// (head/preload_resonance) a refill heuristika (no-advance-on-drop).
//
// POLICY pri prazdnem ringu zustava v hlasech (~10 radku per hlas): Voice
// rozlisuje underrun vs cisty konec pres cleanEnd(); ResonanceVoice drzi
// EOF-hold pres eofAcquire() + holdHiFromLo()/bumpLoIdx(). Reader vraci
// RingEmpty a vystavuje primitivy.
//
// Bit-exact extrakce duplikovaneho kodu — zadna zmena chovani (hlida
// tests/test_render_regression.cpp). Bezi vyhradne na audio vlakne.

#include <cstdint>
#include <string>

namespace ithaca {

class StreamEngine;
struct RingHandle;

class StreamedSampleReader {
public:
    enum class Advance { Reached, RingEmpty };

    // Reset stavu + acquire ring + prvni request [start_frame, min(cap,
    // zbytek)). false = ring pool plny nebo se==nullptr (hlas dohraje RAM
    // region a utichne). Pri plne frontue requestu se offset NEposouva
    // (prirozeny retry v refill()).
    bool begin(StreamEngine* se, const std::string& path,
               int64_t start_frame, int64_t total_frames) noexcept;

    // Varianta bez requestu (ResonanceVoice: za RAM regionem uz nic neni →
    // ring se jen oznaci EOF). request_off = konec RAM regionu.
    bool beginEofOnly(StreamEngine* se, int64_t request_off) noexcept;

    // Sev: lo = posledni frame predchoziho RAM regionu, hi = prvni ring pop
    // (fallback hi=lo pri prazdnem ringu).
    void seed(float lo_l, float lo_r, int64_t lo_idx) noexcept;
    bool seeded() const noexcept { return ring_lo_idx_ >= 0; }

    // Posouva okno (lo<-hi, pop hi, idx++) dokud lo_idx < target. RingEmpty:
    // lo uz prepsane hi, idx NEinkrementovan (presne puvodni chovani Voice).
    Advance advance(int64_t target) noexcept;

    float   loL() const noexcept { return ring_lo_l_; }
    float   loR() const noexcept { return ring_lo_r_; }
    float   hiL() const noexcept { return ring_hi_l_; }
    float   hiR() const noexcept { return ring_hi_r_; }
    int64_t loIdx() const noexcept { return ring_lo_idx_; }
    // EOF-hold primitivy (ResonanceVoice policy).
    void    holdHiFromLo() noexcept { ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_; }
    void    bumpLoIdx() noexcept { ring_lo_idx_++; }

    // -- Bulk rezim (perf): 1x za blok snapshot w_ (acquire) + lokalni kurzor,
    //    1x za blok commit r_ (release) — misto 3 atomickych operaci na kazdy
    //    frame. Hodnoty bit-exact shodne s popFrame. Volat z process():
    //    beginBlock() pred render smyckou, endBlock() po ni (PRED refill(),
    //    ktery cte available() z atomik). release() commitne automaticky.
    void beginBlock() noexcept;
    void endBlock() noexcept;

    // Per-blok refill heuristika (prah z StreamEngine, half-cap reset
    // pendingu, no-advance-on-drop). Volat na konci process() u aktivniho
    // hlasu s ringem, PO endBlock().
    void refill(StreamEngine* se, const std::string& path) noexcept;

    // Vypopuj az max_frames do interleaved dst (prepareDamp). Vraci pocet.
    int  popInto(float* dst_interleaved, int max_frames) noexcept;

    bool    hasRing() const noexcept { return ring_ != nullptr; }
    // -1 kdyz ring neni (shoda s puvodnimi diagnostickymi logy).
    int     ringAvailable() const noexcept;
    bool    eofAcquire() const noexcept;     // ring->eof_ (acquire); false bez ringu
    bool    eofRelaxed() const noexcept;     // jen diagnostika/logy
    int64_t requestOffset() const noexcept { return file_request_off_; }
    // Cisty konec: cely soubor uz byl vyzadan (Voice policy pri RingEmpty).
    bool    cleanEnd() const noexcept { return file_request_off_ >= total_frames_; }

    // Vrat ring do poolu (pokud je) + plny reset stavu. Bezpecne i bez ringu.
    void release(StreamEngine* se) noexcept;

private:
    bool popFrameRaw(float& L, float& R) noexcept;

    RingHandle* ring_  = nullptr;
    int64_t  total_frames_     = 0;
    int64_t  file_request_off_ = 0;
    bool     stream_pending_   = false;
    // Bulk rezim: lokalni kurzory mezi beginBlock/endBlock. blk_w_ je mutable
    // kvuli re-snapshotu v const ringAvailable() (semantika atomic available).
    size_t          blk_r_    = 0;
    mutable size_t  blk_w_    = 0;
    bool            blk_open_ = false;
    float    ring_lo_l_ = 0.f, ring_lo_r_ = 0.f;
    float    ring_hi_l_ = 0.f, ring_hi_r_ = 0.f;
    int64_t  ring_lo_idx_ = -1;   // -1 = neseedovano
};

} // namespace ithaca
