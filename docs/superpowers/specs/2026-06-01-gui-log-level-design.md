# Volitelná úroveň logování — CLI flag + GUI runtime dropdown

**Datum:** 2026-06-01
**Stav:** schváleno k implementaci

## Cíl

Umožnit volbu minimální logované úrovně (Debug/Info/Warn/Error/Fatal), aby
šlo:
- pro **ladění** zapnout Debug a vidět detailní rezonanční logy,
- pro **výkon** zvednout úroveň nad Info, čímž audio thread přestane
  formátovat a zapisovat ladicí zprávy.

Engine (`ithaca-cli`) už `--log-level` má. Práce je hlavně na straně GUI,
které dnes nemá žádné ovládání a vždy běží na Info.

## Klíčové zjištění (proč to dává výkonový smysl)

`Logger::vlogRT` i `Logger::vlog` kontrolují severity **před** voláním
`vsnprintf` (`engine/util/log.cpp:168` a `:121`). Když je minimální úroveň
nad úrovní zprávy, stojí volání na audio threadu jen jeden relaxed atomic
load (`shouldLog`) — žádné formátování řetězce, žádný zápis do RT ringu.
`setMinSeverity`/`getMinSeverity` jsou už atomické a bezpečné za běhu
(`log.cpp:43-48`).

## Rozsah změn

### 1. Logger core — přidat `LOG_RT_DEBUG`

V `engine/util/log.h` chybí RT varianta pro Debug (existují jen
`LOG_RT_INFO/WARN/ERROR`). Přidat:

```cpp
#define LOG_RT_DEBUG(comp_, ...) ITHACA_LOG_RT_(::ithaca::log::Severity::Debug, comp_, __VA_ARGS__)
```

### 2. Rezonanční ladicí logy Info → Debug

Tři řádky přepsat z `LOG_RT_INFO` na `LOG_RT_DEBUG`:
- `engine/resonance/resonance_engine.cpp` — `EXCITE+` (~ř. 123), `SPAWN` (~ř. 155)
- `engine/voice/resonance_voice.cpp` — `START` (~ř. 95)

Tím default Info běží tiše; spam se objeví jen po přepnutí na Debug.

### 3. CLI — beze změny

`--log-level debug|info|warn|error|fatal` už funguje ve všech režimech
(`app/cli/main.cpp:97-98,134`). Runtime přepínání v CLI se **nedělá** —
záměrně, stačí startup flag. Po bodu 2 se z `--log-level` automaticky stává
přepínač pro skrytí/zobrazení rezonančního debugu.

### 4. GUI — perzistence (schema_version 1 → 2, BEZ zpětné kompatibility)

`app/gui/persistence.h` — `GuiState`:
```cpp
int         schema_version = 2;            // bump z 1
std::string log_level      = "info";       // nový klíč
```

`persistence.cpp`:
- `loadState` čte `log_level` jako standardní string klíč (styl `bank_path`).
  Stávající kontrola `if (s.schema_version != 1) return nullopt` se změní na
  `!= 2`, takže **staré soubory verze 1 se zahodí** a GUI nastartuje s
  defaulty. Žádný tolerantní parse.
- `saveState` přidá řádek `"log_level": "<value>"`.

`app/gui/main.cpp` — do debounce-porovnání v render loopu (`main.cpp:177`)
přidat `last_saved.log_level != ctx.state.log_level`.

### 5. GUI — startup flag

`app/gui/main.cpp` — přidat parsování `--log-level <lvl>` (vedle
`--bank-dir`). Override pattern jako u `--bank-dir`: načti state → pokud flag
zadán, přepiš `st.log_level` → `initFromState` aplikuje. Aktualizovat
`printUsage`.

### 6. GUI — aplikace úrovně + runtime dropdown

`app/gui/app_context.cpp` `initFromState`: hned na začátku (PŘED
`engine.init`, aby i bank-load logy ctily úroveň) zavolat
`log::Logger::default_().setMinSeverity(log::severity_from_string(state.log_level.c_str()))`.

`app/gui/panel_params.cpp`: combo box s úrovněmi Debug/Info/Warn/Error/Fatal.
Při změně:
- zapsat `ctx.state.log_level` (string),
- zavolat `ctx.engine` … resp. přímo `log::Logger::default_().setMinSeverity(...)`
  (Logger je proces-wide singleton, není třeba ho tahat přes Engine).

Mapování label ↔ string řeší `severity_from_string` / `severity_to_string`,
které už existují (`log.cpp:12-33`).

## Testy (TDD)

`tests/test_persistence.cpp`:
- **Round-trip** rozšířit o `log_level` (set "debug" → save → load → CHECK).
- Stávající case „wrong schema_version" (`:45`, dnes testuje 99) doplnit/upravit
  tak, aby ověřil, že **verze 1 → `nullopt`** (žádná zpětná kompatibilita).
- `severity_from_string`: neznámý/prázdný vstup → default Info (rychlý unit
  case, pokud ještě není pokryt).

Runtime gating (že Debug zpráva se při Info vůbec nezaformátuje) se čistě
unit-testuje obtížně; spoléhá se na už testovanou logiku `shouldLog` +
manuální ověření přes GUI dropdown.

## Dotčené soubory

| Soubor | Změna |
|--------|-------|
| `engine/util/log.h` | + `LOG_RT_DEBUG` makro |
| `engine/resonance/resonance_engine.cpp` | EXCITE+/SPAWN → Debug |
| `engine/voice/resonance_voice.cpp` | START → Debug |
| `app/gui/persistence.h` | schema_version=2, +log_level |
| `app/gui/persistence.cpp` | load/save log_level, verze !=2 → nullopt |
| `app/gui/main.cpp` | --log-level flag, debounce porovnání |
| `app/gui/app_context.cpp` | setMinSeverity v initFromState |
| `app/gui/panel_params.cpp` | runtime dropdown |
| `tests/test_persistence.cpp` | log_level round-trip + verze-1 reject |

## Mimo rozsah (YAGNI)

- Runtime přepínání v CLI.
- Per-component úrovně (jen globální min severity).
- Zpětná kompatibilita state.json verze 1.
