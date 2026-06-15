# Makefile — build orchestrator

Top-level `Makefile` je tenký orchestrator nad CMake: autodetekuje platformu,
generátor a počet jader, vygeneruje bank secret a spustí CMake. Sám nic
nekompiluje — veškerou práci dělá CMake (`Makefile` jen volá správné příkazy).
Bez `cmake` (a na Windows bez VS toolchainu) neprojde; viz [README](../README.md).

## Příklady

```sh
make                       # napoveda (default cil)
make build                 # zkompiluj vse (configure pri prvnim behu)
make test                  # build + doctest pres ctest
make smoke                 # build + rychly batch-render self-test
make rebuild               # clean + configure + build (od nuly)
make BUILD_TYPE=Debug build        # Debug misto Release
make GENERATOR=Ninja configure     # vynutit Ninja generator
make JOBS=4 build                  # omezit paralelismus

# licencovana (sifrovana) banka — viz docs/bank-format-packed.md §7
make new-license SRC=~/banks/sp-customgrand-dynamic DST=~/banks/sp-customgrand-licensed
make new-license SRC=<dyn> DST=<out> LICENSE_JSON=owner.json   # neinteraktivni
```

## Cíle (targets)

| Cíl | Co dělá |
|-----|---------|
| `help` (default) | Vypíše detekovanou platformu/generátor/jádra a seznam cílů. |
| `check-tools` | Ověří, že je v PATH `cmake` (povinné) a `ninja` (volitelné). |
| `fetch-third-party` | Stáhne vendored závislosti (`tools/fetch-third-party.sh`) do `third-party/`. |
| `bank-secret` | Vygeneruje `secret/bank_secret.key`, pokud chybí (idempotentní). |
| `configure` | Vygeneruje secret + spustí CMake configure do `$(BUILD_DIR)/`. |
| `build` | Zkompiluje vše (na první build si vyžádá `configure`). Binárka v `$(BUILD_DIR)/`. |
| `rebuild` | `clean` + `configure` + `build` — čistý build od nuly. |
| `test` | `build` + spustí doctest přes `ctest --output-on-failure`. |
| `smoke` | `build` + vyrobí 1 fixture WAV a ověří `ithaca-cli --render`. |
| `new-license` | Zabalí dynamickou banku do **šifrované** `soundbank.ithaca` + vytvoří `license.ithaca`. Vyžaduje `SRC` a `DST`. |
| `clean` | Smaže `$(BUILD_DIR)/`. |
| `info` | Vypíše detekované proměnné (PLATFORM/GENERATOR/BUILD_TYPE/JOBS). |

## Override-able proměnné

| Proměnná | Default | Význam |
|----------|---------|--------|
| `BUILD_DIR` | `build` | Adresář pro CMake build. |
| `BUILD_TYPE` | `Release` | `Release` / `Debug`. RT audio engine bez optimalizací je nepoužitelný → default Release. |
| `GENERATOR` | Ninja → jinak Unix Makefiles / VS 2022 | CMake generátor (autodetekce dle PATH/platformy). |
| `JOBS` | počet jader | Paralelismus kompilace. |
| `SECRET_FILE` | `secret/bank_secret.key` | Cesta k master secretu pro šifrované banky. |
| `SRC` / `DST` | — | Zdrojová dynamická banka / cílový adresář pro `new-license`. |
| `LICENSE_JSON` | — | (volitelné u `new-license`) JSON s údaji vlastníka → neinteraktivní bake. |

## Co se děje pod kapotou

- **Platform/generátor autodetekce** (nahoře v Makefile): macOS/Linux/Windows;
  Ninja když je v PATH, jinak Unix Makefiles (Unix) nebo Visual Studio 2022
  (Windows). Recepty běží v bash sémantice (i na Windows přes Git Bash/MSYS).
- **`configure`** nejdřív zavolá `tools/gen-bank-secret.py $(SECRET_FILE)` (zajistí
  bank secret — viz níže), pak `cmake -S . -B $(BUILD_DIR) -G "$(GENERATOR)"
  -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)`. CMake přitom ze secretu vygeneruje
  `bank_secret_generated.h` (zkompiluje se do binárky) a stáhne/ověří vendored deps.
- **`build`** závisí na `$(BUILD_DIR)/CMakeCache.txt` — pokud chybí, spustí se
  `configure` automaticky. Pak `cmake --build $(BUILD_DIR) --parallel $(JOBS)`.
- **Bank secret:** `secret/bank_secret.key` (32 B) je **commitnutý v repu**
  (repo je privátní) → jeden konzistentní klíč napříč buildy. `gen-bank-secret.py`
  ho vytvoří jen když chybí (nikdy nepřepíše); v běžném klonu už existuje a je to
  no-op. Slouží k šifrování/dešifrování licencovaných bank. Detaily:
  [bank-format-packed.md §7.5](bank-format-packed.md).
- **`new-license`** ověří `SRC`/`DST`, zajistí secret a spustí
  `tools/bake_soundbank.py --license` (interaktivní dotaz na vlastníka) nebo
  `--license-json` (z `LICENSE_JSON`), vždy s `--verify`. Výstup: šifrovaná
  `DST/soundbank.ithaca` + `DST/license.ithaca`. Plný workflow + threat model:
  [bank-format-packed.md](bank-format-packed.md).

## Bez `make` (čistý Windows)

`Makefile` je jen orchestrator — totéž jde volat přímo přes CMake:

```sh
python3 tools/gen-bank-secret.py secret/bank_secret.key
cmake -S . -B build -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
```
