# ithaca-legacy

Sample player s jedinym core (sample core). Cilem je nizko-latencni
crossplatform prehravac vlastnich pianovych samplu s dynamickymi velocity
sloty, round-robin, sympatetickou rezonanci a half-pedalingem; jadro pojede
i na Raspberry Pi se streamovanim samplu z disku.

Kompletni dokumentace (knizka): **[docs/README.md](docs/README.md)** — provoz
(build, Raspberry Pi, konfigurace, format banky) i vnitrek enginu.

## Build (macOS)

Jednorazove stazeni vendored deps, pak build:

    make fetch-third-party
    make build

`make` (bez argumentu) nebo `make help` vypise vsechny dostupne cile
(configure / rebuild / test / smoke / clean / new-license / …) a override
promenne (BUILD_TYPE, GENERATOR, JOBS, …). Napr. `make BUILD_TYPE=Debug build`.

### Bank secret (sifrovane packed banky)

`secret/bank_secret.key` (32 B) je **commitnuty v repu** (repo je privatni) —
jeden konzistentni master secret napric vsemi buildy, CI i vyvojari. Build ho
pri `configure` zkompiluje do binarky (`bank_secret_generated.h`, build artefakt)
a python bake ho cte. Slouzi k sifrovani/odsifrovani licencovanych
`soundbank.ithaca` (viz `docs/prirucka/05-format-banky.md`). Kdyby klic chybel
(napr. smazany), `tools/gen-bank-secret.py` (Makefile prereq i CMake) vygeneruje
novy nahodny — POZOR: tim by se znehodnotily drive vydane licencovane banky;
normalne se pouzije commitnuty klic. Rotace klice = re-bake vsech licencovanych
bank. Vyzaduje `python3`.

### Predpoklady pro `make`

Top-level `Makefile` je jen orchestrator nad CMake — sam o sobe nestaci.
Aby `make` proslo, musi byt v PATH:

- **`cmake`** (>= 3.x) — povinne, dela vsechnu praci.
- **`bash`** — recepty pouzivaji bash semantiku; na Windows pres Git Bash / MSYS.
- **generator** — `ninja` kdyz je v PATH, jinak `Unix Makefiles` (macOS/Linux)
  nebo `Visual Studio 17 2022` (Windows). VS toolchain musi byt nainstalovany.
- **samotne `make`** — na Windows neni standardne pritomne (jen v MSYS/MinGW).

Kdyz `make` neni k dispozici (typicky cisty Windows), staci volat CMake primo —
provede presne to, co dela `make build`:

    cmake -S . -B build -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release --parallel 4

Vendored deps jsou pak v `third-party/` (viz `tools/fetch-third-party.sh`).
Vyslednou binarku najdes v `build/` (resp. `build/Release/` u VS generatoru).

> Pozn.: cross-platform (Windows/Linux/Raspberry Pi) je cil a build soubory
> ho nesou, ale zatim se overuje jen na macOS.

## Struktura

    engine/       headless knihovna libithaca_core (sample, voice, dsp,
                  resonance, stream, midi, io, util)
    app/cli/      ithaca-cli — headless konzument (batch render)
    app/gui/      ithaca-gui — Art Deco GUI (Dear ImGui + GLFW)
    third-party/  vendored deps (fetch-third-party.sh)
    tests/        doctest unit testy
    tools/        bake/secret skripty (python)
    secret/       bank_secret.key (master secret, viz vyse)
    docs/         dokumentace — knizka (viz docs/README.md)

## Jazyk

Komentare v kodu jsou cesky bez diakritiky, identifikatory anglicky. Knizka v
`docs/` je cesky **s diakritikou** (lepe se cte). Princip: explicit je lepsi nez
implicit.
