#pragma once
// app/gui/frame_stats.h — kumulativni statistika snimku pro debug log.
// ----------------------------------------------------------------------------
// Proc to tu je: optimalizace GUI se nedaji posuzovat podle jednoho snimku ani
// podle prumeru. Vyvoj bezi na PC, kde je panel zdaleka nejlevnejsi vec v
// systemu, a kazda systemova udalost (planovac, GC jineho procesu, thermal)
// udela ve vzorku spicku, ktera prumer posune a nic nerekne.
//
// Proto se sbiraji DVE ruzne veliciny:
//
//   CAS (cpu_ms, present_ms) — zasumeny. Agreguje se do HISTOGRAMU, ze ktereho
//     se ctou percentily: p50 rekne, jak to bezi obvykle, p95 jak to bezi
//     spatne, a max se drzi zvlast, protoze prave odlehla hodnota je to
//     zajimave. Prumer se zamerne NEPOCITA — jedna 200ms spicka z planovace
//     ho posune tak, ze cislo prestane cokoli znamenat.
//
//     KDE VEST HRANICI: cpu_ms musi koncit u ImGui::Render(), NE u swapu.
//     Prvni pokus meril az za SwapBuffers a vysel p50 = p95 = 18,1 ms, tedy
//     presne perioda snimku — ovladac na Windows totiz na vsync neblokuje ve
//     SwapBuffers, ale uz v predchozim GL volani, takze cekani spadlo do naseho
//     okna. Rozpoznat to slo prave podle histogramu: p50 == p95 == jedna
//     prihradka znamena hodiny, ne praci. Vse za Render() (upload vertexu,
//     clear, swap) je proto v present_ms jako jeden celek — je to prace
//     ovladace promichana s cekanim a oddelit ji odsud nejde.
//
//   GEOMETRIE (vtx, idx, cmds) — NEZASUMENA. Pocet vertexu, ktere snimek
//     vyprodukuje, je ciste funkci toho, co kreslime: zadny planovac, zadny
//     jiny proces, zadna teplota. Diky tomu jde ucinek optimalizace overit
//     EXAKTNE i na vyvojovem PC, kde je mereni casu bezcenne. Kdyz se zapne
//     texturovy antialiasing, klesne pocet vertexu na segment ze 4 na 2 —
//     a je to videt jako cislo, ne jako dojem.
//
// Trida nezna ImGui ani logger: dostava hotova cisla a vraci hotovy radek.
// Diky tomu je testovatelna bez displeje a prezije vyclenení GUI do knihovny.
#include <cstdio>
#include <cstdint>

namespace ithaca::gui {

class FrameStats {
public:
    // Histogram je log-rozlozeny: 4 prihradky na oktavu od 0.05 ms, tedy 10
    // oktav do ~51 ms. Rozliseni 2^(1/4) = +19 % na prihradku je na p50/p95
    // bohate — u casu snimku nas zajima rad, ne desetina milisekundy.
    static constexpr int   kBuckets      = 40;
    static constexpr float kBucketMinMs  = 0.05f;
    static constexpr int   kPerOctave    = 4;

    explicit FrameStats(double period_s = 60.0, float budget_ms = 16.6f)
        : period_s_(period_s), budget_ms_(budget_ms) {}

    // Jeden snimek. cpu_ms = nase prace (panel + stavba draw listu, konci
    // u ImGui::Render()). present_ms = vse za tim vcetne cekani na vsync.
    void add(float cpu_ms, float present_ms, int vtx, int idx, int cmds) {
        ++frames_;
        bump(cpu_hist_, cpu_ms);
        bump(present_hist_, present_ms);
        if (cpu_ms > cpu_max_) cpu_max_ = cpu_ms;
        if (cpu_ms > budget_ms_) ++late_;
        vtx_last_ = vtx; idx_last_ = idx; cmds_last_ = cmds;
        if (vtx > vtx_max_) vtx_max_ = vtx;
    }

    // Vraci true, kdyz uplynula perioda — pak je v `out` radek a citace jsou
    // vynulovane. `now_s` je libovolne monotonni hodiny (v GUI ImGui::GetTime).
    bool maybeReport(double now_s, char* out, int cap) {
        if (last_report_s_ < 0.0) { last_report_s_ = now_s; return false; }
        const double dt = now_s - last_report_s_;
        if (dt < period_s_ || frames_ == 0) return false;

        std::snprintf(out, (size_t)cap,
            "%uf %.1ffps | cpu p50=%.2f p95=%.2f max=%.2f ms late=%u | "
            "present p50=%.2f ms | vtx=%d max=%d idx=%d cmd=%d",
            frames_, (double)frames_ / dt,
            (double)percentile(cpu_hist_, 0.50f),
            (double)percentile(cpu_hist_, 0.95f),
            (double)cpu_max_, late_,
            (double)percentile(present_hist_, 0.50f),
            vtx_last_, vtx_max_, idx_last_, cmds_last_);

        reset();
        last_report_s_ = now_s;
        return true;
    }

    // -- Pristup pro testy a pro pripadny panel diagnostiky -------------------
    uint32_t frames()   const { return frames_; }
    uint32_t late()     const { return late_; }
    float    cpuMax()   const { return cpu_max_; }
    int      vtxLast()  const { return vtx_last_; }
    float    cpuPercentile(float q)     const { return percentile(cpu_hist_, q); }
    float    presentPercentile(float q) const { return percentile(present_hist_, q); }

    void setPeriod(double s)   { period_s_  = s; }
    void setBudgetMs(float ms) { budget_ms_ = ms; }

    void reset() {
        for (int i = 0; i < kBuckets; ++i) { cpu_hist_[i] = 0; present_hist_[i] = 0; }
        frames_ = 0; late_ = 0; cpu_max_ = 0.f; vtx_max_ = 0;
    }

    // Horni hrana prihradky — pouziva ji percentile() i testy.
    static float bucketEdgeMs(int i) {
        float v = kBucketMinMs;
        for (int k = 0; k <= i; ++k) v *= kOctaveStep;
        return v;
    }

private:
    // 2^(1/4), rozepsano jako konstanta — constexpr std::pow neexistuje.
    static constexpr float kOctaveStep = 1.189207115f;

    static int bucketOf(float ms) {
        if (!(ms > kBucketMinMs)) return 0;          // vc. NaN a zapornych
        int i = 0;
        float edge = kBucketMinMs;
        while (i < kBuckets - 1 && ms > edge * kOctaveStep) {
            edge *= kOctaveStep;
            ++i;
        }
        return i;
    }

    static void bump(uint32_t* hist, float ms) { ++hist[bucketOf(ms)]; }

    // Percentil z histogramu. Vraci horni hranu prihradky, ve ktere kvantil
    // lezi — tedy konzervativne nahoru, coz je u casu spravny smer.
    float percentile(const uint32_t* hist, float q) const {
        if (frames_ == 0) return 0.f;
        const uint32_t want = (uint32_t)((float)frames_ * q);
        uint32_t acc = 0;
        for (int i = 0; i < kBuckets; ++i) {
            acc += hist[i];
            if (acc > want) return bucketEdgeMs(i);
        }
        return bucketEdgeMs(kBuckets - 1);
    }

    double   period_s_;
    float    budget_ms_;
    double   last_report_s_ = -1.0;
    uint32_t frames_ = 0;
    uint32_t late_   = 0;
    float    cpu_max_ = 0.f;
    int      vtx_last_ = 0, idx_last_ = 0, cmds_last_ = 0, vtx_max_ = 0;
    uint32_t cpu_hist_[kBuckets]{};
    uint32_t present_hist_[kBuckets]{};
};

} // namespace ithaca::gui
