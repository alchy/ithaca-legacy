#pragma once
// engine/resonance/harmonic_proximity.h
// -------------------------------------
// Vraci harmonickou "blizkost" mezi dvema MIDI strunami [0, 1] — jak silne
// nota `source` budi rezonanci na strune `target`. Cista funkce, bez stavu
// (interne predpocitana 128×128 matice, lazy init).
//
// Model: PARTIAL-COINCIDENCE z idealni harmonicke rady (12-TET zakladni
// frekvence). Strunu N budi parcialy hrane noty P, ktere padnou na parcialy N:
//   prox(N,P) = Σ_{k,m} A(k)·R(m)·exp(-(Δcent(k·fP, m·fN)/σ)^2)
//   A(k)=1/k^a (energie parcialu P), R(m)=1/m^b (receptivita N; b>a → up/down
//   asymetrie), σ = rezonancni sirka [centy]. Normalizovano: octave-up ≈ 1.0.
//   f(N, N) = 0 (play-on-self resi voice_pool).
// Dusledek: oktava nahoru (budi ZAKLAD partnera) je silnejsi nez oktava dolu
// (budi jen 2. parcial) — fyzikalne vernejsi nez puvodni interval-tabulka.
//
// Ladici parametry (a, b, σ, K) jsou konstanty v .cpp. FUTURE: inharmonicita
// B(n) (stretch parcialu k vyskam) — struktura na to pripravena, default B=0.

namespace ithaca {

// Predpocita 128x128 coupling matici (drahy build: ~4M iteraci s log2f/expf,
// na RPi5 az ~1 s). Volat z off-RT kontextu (Engine::init) PRED prvnim pouzitim
// — jinak lazy init probehne pri prvnim note-onu NA AUDIO VLAKNE → dropout.
void initHarmonicProximity();

float harmonicProximity(int target_midi, int source_midi);

} // namespace ithaca
