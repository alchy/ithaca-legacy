# ithaca-legacy

Sample player s jedinym core (sample core). Cilem je nizko-latencni
crossplatform prehravac vlastnich pianovych samplu s dynamickymi velocity
sloty, round-robin, sympatetickou rezonanci a half-pedalingem; jadro pojede
i na Raspberry Pi se streamovanim samplu z disku.

Navrh: `docs/superpowers/specs/2026-05-29-ithaca-legacy-design.md`.

## Build (macOS)

Jednorazove stazeni vendored deps:

    make fetch-third-party

Build + test + smoke:

    make build
    make test
    make smoke

Vyber Debug/Release: `make BUILD_TYPE=Debug build`. Napoveda: `make`.

### Bank secret (sifrovane packed banky)

Build pri kazdem `configure`/buildu (Makefile prerekvizita i CMake) zavola
`tools/gen-bank-secret.py`, ktery **idempotentne** vytvori
`secret/bank_secret.key` (32 nahodnych bajtu) — jen pokud jeste neexistuje, nikdy
neprepise. Soubor je **gitignored** (necommituje se) a zkompiluje se do binarky
(`bank_secret_generated.h`). Slouzi k sifrovani/odsifrovani licencovanych
`soundbank.ithaca` (viz `docs/bank-format-packed.md` §7). Na cistem klonu se
vygeneruje sam pri prvnim buildu — zadny rucni krok. **Produkcni secret zazalohuj**
(ztrata = drive vydane licencovane banky nepujde otevrit). Vyzaduje `python3`.

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

    engine/    headless knihovna libithaca_core (zatim util/logger)
    app/cli/   ithaca-cli — headless konzument
    third-party/  vendored deps (fetch-third-party.sh)
    tests/     doctest unit testy
    docs/      dokumentace + specs/plans

## Jazyk

Dokumentace a komentare v kodu jsou cesky bez diakritiky; identifikatory
anglicky. Princip: explicit je lepsi nez implicit.
