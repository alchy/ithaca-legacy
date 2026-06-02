# Resonance Harmonic Model — Partial-Coincidence (Ideal Math) — Design

**Goal:** Replace the hand-tuned interval-class table in `harmonicProximity` with a physically-grounded **partial-coincidence** model derived analytically from the ideal harmonic series (no FFT / no sample measurement), so sympathetic-resonance eligibility and strength better match a real piano — including the correct up/down asymmetry.

**Why:** The current model (`harmonic_proximity.cpp`) scores resonance by a fixed `kIntervalWeight[12]` × `0.7^octaves`, which is symmetric (octave-up == octave-down) and is not based on which partials actually coincide. Real sympathetic resonance: a played string's partials drive other undamped strings whose partials coincide in frequency; upward partners get their **fundamental** driven (loud, radiates), downward partners only an **upper partial** (quieter). The user wants the ideal mathematical model, not measured spectra.

**Architecture:** Compute a coupling value from partial-coincidence between two strings' ideal harmonic series (12-TET fundamentals). Precompute a 128×128 coupling matrix once at startup; `harmonicProximity(target, source)` becomes a table lookup (zero runtime cost). Integration into `ResonanceEngine::onPlayedNoteOn` is unchanged (`excite = vel × harm × strength`); the new value is normalized so the strongest partner (octave-up) ≈ 1.0, so existing thresholds carry over.

**Tech Stack:** C++20, header-only math, doctest. Pure function — fully unit-testable.

---

## The model

Fundamental frequency from MIDI (12-TET): `f(n) = 440 · 2^((n − 69)/12)`.

Ideal harmonic partials (v1: pure harmonic, no inharmonicity): `f_k(n) = k · f(n)`, `k = 1..K`.

Coupling between candidate string `N` and played note `P`:

```
prox(N, P) = Σ_{k=1..K} Σ_{m=1..K}  A(k) · R(m) · g( Δcent( f_k(P), f_m(N) ) )
```

- **Drive** `A(k) = 1 / k^a` — energy of P's k-th partial (a = 1; lower partials carry more energy).
- **Receptivity** `R(m) = 1 / m^b` — how strongly N's m-th partial responds/radiates (b = 2; the fundamental responds far more than upper partials). **`b > a` is what produces the physical up/down asymmetry** (fundamental excitation ≫ partial excitation).
- **Overlap** `g(Δc) = exp(−(Δc / σ)²)` — resonance response vs detuning; `Δc` = cents between the two partial frequencies, `σ` = string resonance bandwidth (σ = 12 cents).
  `Δcent(f1, f2) = 1200 · |log2(f1 / f2)|`.
- `prox(N, N)` is forced to 0 (self handled by the voice pool, not resonance).

After summing, **normalize** the whole 128×128 matrix by the global max non-self value (which is the octave-up coupling) so the scale is `[0, 1]` with octave-up ≈ 1.0 — matching the current value range and keeping `kResonanceHarmonicMin = 0.05` / `kResonanceExciteMinGain = 0.001` meaningful.

### Why this is faithful (and fixes the asymmetry)

With defaults a=1, b=2, σ=12, K=16, the dominant coincidence terms give roughly:

| Partner (interval from P) | dominant (k,m) | term A(k)·R(m) | normalized prox |
|---|---|---|---|
| octave **up** (+12) | (2,1) | ½·1 = 0.50 | **1.00** |
| octave **down** (−12) | (1,2) | 1·¼ = 0.25 | 0.50 |
| two octaves up (+24) | (4,1) | ¼·1 = 0.25 | 0.50 |
| fifth up (+7) | (3,2) | ⅓·¼ ≈ 0.083 | ~0.17 |
| fifth down (−7) | (2,3) | ½·⅑ ≈ 0.056 | ~0.11 |
| major third up (+4) | (5,4) | ⅕·1/16 ≈ 0.0125 | ~0.03 |

So: octave-up is strongest; **octave-down is half** (driven only at its 2nd partial → physically quieter, fixing today's spurious loud low resonance); fifths/thirds follow; the falloff with distance emerges from `A·R` (no hand-tuned octave decay). 12-TET detuning of non-octave partners (fifth ≈ 2 cents off just) is captured by `g()`.

### Inharmonicity (future toggle, not v1)

Real partials are stretched: `f_k(n) = k · f(n) · √(1 + B(n)·k²)`, with `B(n)` rising toward the treble. This would detune distant-octave coincidences (further reducing far coupling, realistically). v1 ships with `B = 0` (pure ideal). The model is structured so a `B(n)` curve can be added later by changing only the partial-frequency function.

---

## Components

### `engine/resonance/harmonic_proximity.h` / `.cpp`

- Keep the public signature `float harmonicProximity(int target_midi, int source_midi)` — callers (`onPlayedNoteOn`) are unchanged.
- Add internal model parameters as named constants: `kPartials (K=16)`, `kDriveExp (a=1.0)`, `kRecvExp (b=2.0)`, `kBandwidthCents (σ=12.0)`.
- Add a pure helper computing `prox(N,P)` from the formula above (the double sum over partials + Gaussian overlap).
- **Precompute** a `static const std::array<std::array<float,128>,128>` coupling matrix, built once (lazy `static` init on first call, or an explicit init): for every (target, source) pair compute the raw sum, then normalize by the global max. `harmonicProximity` returns `matrix[target][source]`.
  - Build cost: 128×128×K² ≈ 4.2M flops once — negligible at startup; thread-safe via function-local `static` (C++11 guaranteed-once init).
- `harmonicProximity(n, n)` returns 0 (kept).

### Integration (unchanged)

`ResonanceEngine::onPlayedNoteOn` keeps `harm = harmonicProximity(N, played_midi)`, the `harm < kResonanceHarmonicMin` gate, and `excite = vel_norm · harm · str`. Because `prox` is normalized to octave-up ≈ 1.0 (same scale as the old octave weight), the thresholds need no change. The downward-partner attenuation (octave-down 0.5 vs up 1.0) now flows naturally into `excite` → quieter low resonance, no separate gain weighting needed.

---

## Error handling / edge cases

- MIDI out of `[0,127]` → return 0 (guard at entry; the matrix is 128×128).
- `f1/f2` ratio is always positive (frequencies > 0); `log2` safe.
- Very large `Δcent` → `g()` underflows to ~0 (fine; those pairs contribute nothing).
- `K` capped at 16 (16th partial of A0 ≈ 440 Hz region — well within audible; higher K adds negligible coupling and cost).

---

## Testing (doctest, `tests/test_harmonic_proximity.cpp` — new or extend if exists)

Pure function → directly testable:

- **Self is zero:** `harmonicProximity(60,60) == 0`.
- **Ordering by strength:** for P=60, `prox(72) > prox(67) > prox(64)` (octave-up > fifth-up > third-up).
- **Up/down asymmetry:** `prox(72,60) > prox(48,60)` (octave up stronger than octave down) and `prox(48,60) > 0` (down still resonates).
- **Octave-up is the global max / normalized to ≈ 1:** `prox(72,60)` is ≥ every other `prox(x,60)` and ≈ 1.0 (within tolerance).
- **Distant falloff:** `prox(84,60) < prox(72,60)` (two octaves up < one octave up).
- **Range:** all values in `[0,1]`.
- **Symmetry sanity of the matrix build:** values are finite, non-negative.

(Existing `ResonanceEngine` behavior tests remain green since the integration/thresholds are unchanged.)

---

## File structure

**Modify:** `engine/resonance/harmonic_proximity.h`, `engine/resonance/harmonic_proximity.cpp` (replace interval-table body with the partial-coincidence model + precomputed matrix).
**New/extend:** `tests/test_harmonic_proximity.cpp` (+ register in `tests/CMakeLists.txt` if new).
**Unchanged:** `engine/resonance/resonance_engine.cpp` (integration, thresholds, budget gate).

---

## Out of scope / future

- **Inharmonicity `B(n)`** curve (treble stretch) — structured-in but defaults to 0.
- **Velocity-dependent brightness** (lower `a` at high velocity) — keep `a` fixed in v1; could fold velocity into `A(k)` later.
- **Unison/aliquot/duplex strings, soundboard coupling** — not modeled.
- Tuning `a, b, σ, K` by ear after listening — exposed as constants for easy adjustment, not GUI.
