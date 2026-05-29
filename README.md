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
