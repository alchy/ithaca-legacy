# Resonance Partial-Coincidence Model — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (single task, inline). Steps use `- [ ]`.

**Goal:** Replace `harmonicProximity`'s interval-class table with a partial-coincidence model from the ideal harmonic series (12-TET fundamentals), precomputed into a normalized 128×128 matrix.

**Spec:** `docs/superpowers/specs/2026-06-02-resonance-partial-coincidence-design.md`. Params: K=16, a=1 (drive 1/kᵃ), b=2 (receptivity 1/mᵇ → up/down asymmetry), σ=12 cents, B=0. Normalize by global max (octave-up≈1.0) so existing thresholds carry over. Integration in `onPlayedNoteOn` unchanged.

**Build/test:** `cmake --build build -j` · `ctest --test-dir build`. Branch `feat/resonance-partial-model`. Czech comments; commit trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

---

### Task 1: partial-coincidence model + precomputed matrix

**Files:** Modify `engine/resonance/harmonic_proximity.cpp` (replace body), keep `harmonic_proximity.h` signature. Create `tests/test_harmonic_proximity.cpp` + register in `tests/CMakeLists.txt`.

- [ ] **Step 1 — failing tests** (`tests/test_harmonic_proximity.cpp`):

```cpp
// tests/test_harmonic_proximity.cpp — partial-coincidence rezonancni model.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "resonance/harmonic_proximity.h"
using namespace ithaca;

TEST_CASE("self je 0") { CHECK(harmonicProximity(60,60) == 0.f); }

TEST_CASE("poradi sily: oktava > kvinta > V.tercie (nahoru)") {
    CHECK(harmonicProximity(72,60) > harmonicProximity(67,60));
    CHECK(harmonicProximity(67,60) > harmonicProximity(64,60));
}

TEST_CASE("up/down asymetrie: oktava nahoru > oktava dolu > 0") {
    CHECK(harmonicProximity(72,60) > harmonicProximity(48,60));
    CHECK(harmonicProximity(48,60) > 0.f);
}

TEST_CASE("oktava nahoru je normalizovane maximum (~1.0)") {
    CHECK(harmonicProximity(72,60) == doctest::Approx(1.0f).epsilon(0.001));
    for (int t = 0; t < 128; ++t)
        CHECK(harmonicProximity(t,60) <= harmonicProximity(72,60) + 1e-4f);
}

TEST_CASE("pokles se vzdalenosti: 2 oktavy < 1 oktava") {
    CHECK(harmonicProximity(84,60) < harmonicProximity(72,60));
}

TEST_CASE("rozsah [0,1] a koncne hodnoty") {
    for (int t = 0; t < 128; ++t) {
        float v = harmonicProximity(t, 60);
        CHECK(v >= 0.f);
        CHECK(v <= 1.0f + 1e-4f);
        CHECK(std::isfinite(v));
    }
}
```

- [ ] **Step 2 — register test** (`tests/CMakeLists.txt`, append):
```cmake
add_executable(test_harmonic_proximity test_harmonic_proximity.cpp)
target_link_libraries(test_harmonic_proximity PRIVATE ithaca_core doctest)
add_test(NAME test_harmonic_proximity COMMAND test_harmonic_proximity)
```

- [ ] **Step 3 — verify fail:** `cmake -S . -B build >/dev/null && cmake --build build --target test_harmonic_proximity -j 2>&1 | tail -5` — expect the new ordering/asymmetry/normalization assertions to FAIL against the old interval-table model (e.g. octave-up != 1.0; up==down symmetric).

- [ ] **Step 4 — replace `engine/resonance/harmonic_proximity.cpp`** entirely with:

```cpp
// engine/resonance/harmonic_proximity.cpp — viz .h
// Partial-coincidence model z idealni harmonicke rady (12-TET zakladni
// frekvence). Strunu N budi parcialy hrane noty P, ktere padnou na parcialy N.
// Sila = Σ drive(k)·recv(m)·overlap(detuning) pres pary parcialu. Predpocitano
// do normalizovane 128×128 matice pri prvnim volani (octave-up ≈ 1.0).
#include "resonance/harmonic_proximity.h"

#include <array>
#include <cmath>

namespace ithaca {
namespace {

constexpr int   kPartials       = 16;     // K
constexpr float kDriveExp       = 1.0f;   // A(k) = 1/k^a (energie parciálu P)
constexpr float kRecvExp        = 2.0f;   // R(m) = 1/m^b (receptivita N; b>a → up/down asymetrie)
constexpr float kBandwidthCents = 12.0f;  // sigma rezonancni sirky

inline float midiHz(int n) {
    return 440.f * std::pow(2.f, (float)(n - 69) / 12.f);
}

// Raw coupling prox(target N, source P): Σ_{k,m} A(k)·R(m)·exp(-(Δc/σ)^2).
float rawProx(int target, int source) {
    if (target == source) return 0.f;
    const float fP = midiHz(source);
    const float fN = midiHz(target);
    float sum = 0.f;
    for (int k = 1; k <= kPartials; ++k) {
        const float fk = (float)k * fP;
        const float A  = 1.f / std::pow((float)k, kDriveExp);
        for (int m = 1; m <= kPartials; ++m) {
            const float fm = (float)m * fN;
            const float dc = 1200.f * std::fabs(std::log2(fk / fm));  // centy
            const float x  = dc / kBandwidthCents;
            const float g  = std::exp(-x * x);
            if (g < 1e-4f) continue;   // zanedbatelny prispevek
            const float R  = 1.f / std::pow((float)m, kRecvExp);
            sum += A * R * g;
        }
    }
    return sum;
}

// Predpocitana normalizovana matice (lazy static, thread-safe init).
const std::array<std::array<float, 128>, 128>& couplingMatrix() {
    static const std::array<std::array<float, 128>, 128> M = [] {
        std::array<std::array<float, 128>, 128> mat{};
        float maxv = 0.f;
        for (int t = 0; t < 128; ++t)
            for (int s = 0; s < 128; ++s) {
                const float v = rawProx(t, s);
                mat[(size_t)t][(size_t)s] = v;
                if (v > maxv) maxv = v;
            }
        if (maxv > 0.f)
            for (auto& row : mat)
                for (auto& v : row) v /= maxv;   // octave-up → 1.0
        return mat;
    }();
    return M;
}

} // namespace

float harmonicProximity(int target_midi, int source_midi) {
    if (target_midi < 0 || target_midi > 127 ||
        source_midi < 0 || source_midi > 127) return 0.f;
    return couplingMatrix()[(size_t)target_midi][(size_t)source_midi];
}

} // namespace ithaca
```

- [ ] **Step 5 — verify pass:** `cmake --build build -j 2>&1 | grep -iE "error|warning:" | head` (clean) and `ctest --test-dir build -R test_harmonic_proximity` (all pass), then full `ctest --test-dir build 2>&1 | grep -E "passed|failed"` (no regressions — resonance integration unchanged).

- [ ] **Step 6 — smoke:** `( ./build/ithaca-gui --bank-dir /Users/j/SoundBanks/Ithaca >/tmp/s.txt 2>&1 & echo $! >/tmp/p ); sleep 3; kill $(cat /tmp/p) 2>/dev/null && echo "ran ok"`.

- [ ] **Step 7 — commit:**
```bash
git add engine/resonance/harmonic_proximity.cpp tests/test_harmonic_proximity.cpp tests/CMakeLists.txt
git commit -m "feat(resonance): partial-coincidence harmonic model (ideal math, precomputed)

Replaces the interval-class table with Σ drive(k)·recv(m)·overlap(detuning)
over ideal harmonic partials of both strings; precomputed 128x128 matrix
normalized so octave-up≈1.0 (thresholds unchanged). b>a gives the physical
up/down asymmetry (octave-up louder than octave-down).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Final verification
- [ ] `ctest --test-dir build` 100% pass.
- [ ] Listen: with pedal down, low note → resonance still favors upward partners (octave/fifth up) but lower partners are now relatively quieter; overall distribution feels closer to a real instrument.
- [ ] Optional: log `resonatingMidiNotes` distribution for a played low note to compare up/down spread vs the old model.

### Notes
- The harmonic_proximity.h header keeps its signature and doc comment; update the `// Model (faze 5, jednoduchy)` comment block in the header to describe the new partial-coincidence model.
- Tuning `a, b, σ, K` by ear later = edit the four constants; matrix rebuilds at next start.
