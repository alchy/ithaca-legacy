# 4 · Konfigurace (state.json)

ithaca-gui si pamatuje, jak jste si ji nastavili. Když ji zavřete a zase
otevřete, najdete okno na stejném místě, vybranou stejnou banku, stejný MIDI
port a stejně poskládaný DSP řetěz. To všechno žije v jediném souboru —
`state.json` — a tahle kapitola je jeho úplným průvodcem: kde ho hledat, jak se
sám spravuje, a co znamená každý jeho klíč.

Důležité hned na úvod: **soubor si spravuje GUI samo a normálně ho nemáte proč
otevírat ručně.** Pár ladicích parametrů (typicky pro embedded cíle jako
Raspberry Pi) ale nemá v GUI žádný ovládací prvek — ty se nastavují jedině zde.
Proto se vyplatí vědět, jak je soubor postavený.

## Co to je a jak se sám spravuje

`state.json` je perzistované nastavení **ithaca-gui** (to je ten F8 front-end).
Ukládá geometrii okna, vybranou banku a MIDI port, parametry hlasů a rezonance,
kompletní nastavení DSP řetězu (Convolver, AGC, Enhancer, Limiter), aktivní
stránku CONFIGu, nastavení zvukového zařízení a úroveň logování.

O zápis a čtení se stará GUI samo, a to ve třech situacích:

- **Při prvním čistém ukončení**, pokud soubor ještě neexistuje. Bez souboru
  startuje GUI z vestavěných výchozích hodnot.
- **Během sezení s „debounce" zápisem.** Soubor se zapíše **~1 sekundu po první
  sledované změně** — pozor, *ne* po 1 s nečinnosti. Časovač se rozběhne první
  změnou a další změny ho už neresetují. Takže když plynule taháte za posuvník,
  soubor se zapisuje zhruba jednou za sekundu, ne při každém snímku.
- **Bezpodmínečně při čistém ukončení** (když zavřete okno) — tady se zachytí i
  finální poloha a velikost okna.

Zápis je **atomický**, aby vám pád nebo plný disk nerozbil konfiguraci v půlce
zápisu: GUI nejdřív zapíše do `state.json.tmp`, ověří stav streamu, a teprve pak
ho přejmenuje přes `state.json`. Když nastane I/O chyba (třeba zaplněný disk),
useknutý `.tmp` se smaže a původní funkční konfigurace zůstane nedotčená (viz
`saveState` v `app/gui/persistence.cpp`).

Formát je **plochý JSON objekt** — žádné vnořené objekty ani pole. Parsuje ho
minimální ručně psaný parser, ne plnohodnotná JSON knihovna, takže když do
souboru přece jen sáhnete ručně, udržte ho plochý. Na pořadí klíčů parseru
nezáleží (hledá je podle názvu); pořadí níže odpovídá tomu, co zapisuje
`saveState`.

## Kde soubor leží

Cestu počítá `defaultStatePath()` jako
`platformConfigDir() / "ithaca-legacy" / "state.json"` (v
`app/gui/persistence.cpp`). Základní adresář se liší podle operačního systému:

| OS      | Základní adresář (`platformConfigDir()`)                   | Plná cesta                                                       |
|---------|------------------------------------------------------------|-----------------------------------------------------------------|
| macOS   | `$HOME/Library/Application Support`                        | `$HOME/Library/Application Support/ithaca-legacy/state.json`     |
| Linux   | `$XDG_CONFIG_HOME`, jinak `$HOME/.config`                  | `$XDG_CONFIG_HOME/ithaca-legacy/state.json` (nebo `~/.config/...`) |
| Windows | `%APPDATA%`                                                | `%APPDATA%\ithaca-legacy\state.json`                            |

Pokud příslušná proměnná prostředí není nastavená, `platformConfigDir()` spadne
zpět na **aktuální pracovní adresář** — soubor by pak skončil v
`./ithaca-legacy/state.json`. Nadřazené adresáře se při uložení vytvoří
automaticky.

## Verzování schématu a migrace

Soubor v sobě nese číslo verze formátu, aby ho šlo bezpečně rozvíjet a starší
soubory nezahodit. Aktuální schéma je **`schema_version` 7**.

- `loadState()` přijme soubory se `schema_version` **3 až 7**. Jakákoli jiná
  hodnota (nebo chybějící `schema_version`) způsobí, že se načtení nezdaří a GUI
  startuje z výchozích hodnot. Po úspěšném načtení se `schema_version` v paměti
  bezpodmínečně nastaví na `7`, takže **další uložení soubor přepíše jako v7**.

- **Co která verze přinesla:**

  | verze | změna | migrace ze starší |
  |---|---|---|
  | v5 | DSP řetěz přestal být sada plochých klíčů a stal se **generickou sekcí** `dsp.<STAGE>.<Param::id>` | staré ploché klíče (`convolver_mix`, `agc_target`, …, včetně ještě staršího `bbe_*` → `ENHANCER`) se namapují do nové sekce |
  | v6 | přibyla sekce `defaults.` (uživatelský profil) | čistě aditivní — starý soubor sekci prostě nemá a `RESET` jede tovární |
  | v7 | `midi_channel` (jeden kanál) → `midi_channel_mask` (maska) | odvodí se: `−1` → `0xFFFF` (OMNI), *n* → `1 << n` |
- **Všechny klíče se čtou defenzivně.** Chybějící **nebo poškozený** (nečíselný)
  klíč spadne zpět na výchozí hodnotu struktury `GuiState` *jen pro to jedno
  pole* — zbytek souboru se i tak načte. (`loadState` čte každé číselné/bool
  pole přes pomocníky `readF`/`readB`/`readI` s `try/catch` na úrovni pole.)
  Právě tohle dělá staré soubory dopředně kompatibilní: schéma od původní v4
  výrazně narostlo (Convolver, Enhancer, rozdělení zisku a vrstvy rezonance,
  `resonance_window_ms`, pole zvukového zařízení), a přesto se soubor, který
  tyhle klíče ještě nezná, načte — každý chybějící klíč prostě dostane svůj
  default. Jediný špatný klíč už nezahodí celý stav; jen chybějící nebo cizí
  `schema_version` vrátí z `loadState` `nullopt` (a tím úplný pád na výchozí
  hodnoty).
- **Ošetření hodnot při načtení.** Geometrie okna se hlídá: `window_w < 320` spadne
  zpět na `1280`, `window_h < 240` na `720`. (Minimalizované okno na Windows
  perzistuje jako 0×0 a okno by se při dalším startu nevytvořilo.)
- **Migrace BBE → Enhancer.** Dřívější fáze „BBE" se přejmenovala na „Enhancer".
  Klíče `enhancer_*` spadnou zpět na starší klíče `bbe_*`, pokud chybí:
  `enhancer_process` ← `bbe_definition`, `enhancer_contour` ← `bbe_bass`,
  `enhancer_enabled` ← `bbe_enabled`. Soubor zapsaný před přejmenováním si tedy
  při prvním načtení převede staré hodnoty BBE do Enhanceru a poté se přepíše s
  novými klíči.
- `log_level` má při prázdné/chybějící hodnotě default `"info"`.

## Přehled všech polí

Níže jsou vypsaná všechna pole struktury `GuiState` (`app/gui/persistence.h`).
Rozsahy a jednotky polí hlasů/rezonance a DSP pocházejí z tabulek `Param` v
`app/gui/master_page.h`, `resonance_page.h` a
`engine/dsp/{convolver,agc,enhancer,limiter}.cpp`. GUI ořezává hodnoty na tyhle
rozsahy při nastavení přes ovládací prvek a DSP fáze ořezávají navíc ještě při
`set()`.

### Meta

| JSON klíč        | Typ  | Default | Povolené hodnoty | Význam | Nastavuje |
|------------------|------|---------|------------------|--------|-----------|
| `schema_version` | int  | `7`     | při načtení přijme 3 až 7; vždy se zapíše jako 7 | Verze formátu souboru | jen init/migrace |

### Geometrie okna

| JSON klíč  | Typ  | Default | Rozsah | Význam | Nastavuje |
|------------|------|---------|--------|--------|-----------|
| `window_x` | int  | `100`   | libovolný (mimo obrazovku se ořízne) | Pozice okna X (px obrazovky) | GUI (sledováno každý snímek) |
| `window_y` | int  | `100`   | libovolný (mimo obrazovku se ořízne) | Pozice okna Y (px obrazovky) | GUI (sledováno každý snímek) |
| `window_w` | int  | `1280`  | ≥ 320 (menší → při načtení padá na 1280) | Šířka okna (px) | GUI (sledováno každý snímek) |
| `window_h` | int  | `720`   | ≥ 240 (menší → při načtení padá na 720) | Výška okna (px). Default cílí na HW displej 1280×720. | GUI (sledováno každý snímek) |

Geometrie okna se aktualizuje do stavu v paměti každý snímek a perzistuje se při
ukončení (a přes debounce, pokud se mění jiná pole). K ořezu okna mimo obrazovku
viz poznámky níže.

### Banka

| JSON klíč         | Typ    | Default | Význam | Nastavuje |
|-------------------|--------|---------|--------|-----------|
| `bank_search_dir` | string | `""`    | Adresář, který procházel rozbalovací seznam bank při hledání kandidátů. Když je `bank_path` prázdný, je tohle jediný zdroj kandidátů. | CLI přepínač `--bank-dir` (přepíše při startu) |
| `bank_path`       | string | `""`    | Cesta k bance, která se má načíst při startu. Načítá se best-effort: při neúspěchu se zaloguje varování a engine běží naprázdno. | GUI (panel banky) |

Adresář s bankou může obsahovat buď volné WAV soubory (fixed-velocity, nebo
dynamic-velocity), **nebo** jeden pakovaný soubor `soundbank.ithaca` — viz
[kapitola o formátu banky](05-format-banky.md). Detekce a výběr jsou
automatické: adresář, který obsahuje `soundbank.ithaca`, se načte pakovanou
cestou. Výběr banky v GUI i CLI je tak jako tak stejný.

### MIDI

| JSON klíč        | Typ    | Default | Rozsah/hodnoty | Význam | Nastavuje |
|------------------|--------|---------|----------------|--------|-----------|
| `midi_port_name` | string | `""`    | podřetězec názvu reálného portu | MIDI vstupní port. Při startu se hledá jako podřetězec v seznamu živých portů; při shodě se zpět zapíše přesný název portu. Bez shody → varování, žádný port se neotevře. | GUI / init |
| `midi_channel_mask` | int | `65535` | bitová maska, bit *i* = kanál *i*+1 | MIDI přijímacé kanály. `0xFFFF` (65535) = všech šestnáct, tedy OMNI; `0` = nepřijde nic (panel to hlásí jako `NONE - MIDI MUTED`). Maska místo jednoho indexu proto, že jde zapnout libovolná **podmnožina** kanálů — třeba jen 1 a 3. | GUI (SYS → CHANNEL) |

### Parametry hlasů a rezonance

Na engine se aplikují při inicializaci přes `EngineConfig`; ty živě laditelné
najdete na stránkách CONFIGu **MASTER** a **RESONANCE**.

| JSON klíč              | Typ   | Default  | Rozsah         | Jednotka | Význam | Nastavuje |
|------------------------|-------|----------|----------------|----------|--------|-----------|
| `master_gain_db`       | float | `0.0`    | `-60` … `6`    | dB   | Master výstupní zisk (převádí se na lineární `10^(dB/20)`) | GUI (MASTER) |
| `release_ms`           | float | `200.0`  | `50` … `2000`  | ms   | Doba doznívání (release) hlasu | GUI (MASTER) |
| `resonance_enabled`    | bool  | `true`   | `true`/`false` | —    | Zapnout engine sympatické rezonance | GUI (RESONANCE / horní lišta) |
| `resonance_gain_db`    | float | `-12.0`  | `-60` … `0`    | dB   | Výstupní zisk rezonance | GUI (RESONANCE) |
| `resonance_layer_db`   | float | `-30.0`  | `-60` … `0`    | dB   | Cílová velocity vrstva (peak RMS), kterou rezonance vybírá pro každou notu (`nearestSlotByRms`) | GUI (RESONANCE) |
| `excite_decay_ms`      | float | `5000.0` | `500` … `30000`| ms   | Doba doznívání buzení (excitace) | GUI (RESONANCE) |
| `max_resonance_voices` | int   | `32`     | `1` … `64`     | —    | Max. počet rezonančních hlasů. **Živé** — posuvník „MAX RESONANCE" (parametr 3) na stránce RESONANCE volá `engine.setMaxResonanceVoices`; aplikuje se i při inicializaci enginu a sleduje ho debounce zápis. | GUI (RESONANCE) |
| `resonance_window_ms`  | int   | `12000`  | ≥ 0 (ms)       | ms   | Okno RAM-cache cílové rezonanční vrstvy pro každou notu. **Jen v JSONu — záměrně bez ovládacího prvku v GUI**; upravte ručně. Větší = delší rezonanční ocasy držené v RAM (více paměti). | jen JSON |
| `preload_ms`           | int   | `150`    | ≥ 0 (ms)       | ms   | Délka hlavy (head) každého samplu předem nahraná v RAM (zbytek se streamuje z disku). **Jen v JSONu.** Větší = víc dat rezidentních v RAM, méně streamování z disku (méně podtečení) — užitečné na embedded s rychlou RAM / pomalým úložištěm; zvednutím lze celé krátké samply nahrát naráz. | jen JSON |
| `cache_budget_mb`      | int   | `0`      | `0`=auto, jinak MB | MB | RAM rozpočet pro načtení banky. `0` = **auto** (~60 % fyzické RAM, přes `sysinfo`). `>0` = tvrdý strop. Při překročení se načítání **přeruší** (nekompletní banka + chyba v logu), místo aby spadlo na `bad_alloc`. **Jen v JSONu.** Chrání embedded (RPi5 / 4 GB) před OOM. | jen JSON |

### Panel — vzhled a tempo

Všechna tato pole mají i CLI přepínač (`--wave-glow`, `--wave-glow-budget`,
`--frame-divider`, `--frame-divider-idle`), který persistovanou hodnotu při
startu přepíše. Doporučené hodnoty pro Raspberry Pi jsou v
[6 · Panel na Raspberry Pi](06-panel-a-rpi.md#ladicí-páky).

| Klíč | Typ | Default | Rozsah | Význam | Kdo mění |
|---|---|---|---|---|---|
| `wave_glow` | float | `1.0` | `0` … `128` | Násobitel **dosahu záře** vlny v pozadí. `0` = holá čára bez záře (nejlevnější), `1` = výchozí vzhled, `>1` = širší rozostření. Záře je jediná věc na panelu, která roste s **výplní** — hlavní páka pro slabší grafiku. | CLI / JSON |
| `wave_glow_budget_ms` | float | `0` | `0` … `1000` | Strop **periody snímku** [ms]; při překročení regulátor dosah sám sníží. `0` = automatika vypnutá. Na panelu 60 Hz je nominál 16,7 ms, takže ~25 znamená zmeškaný snímek. | CLI / JSON |
| `frame_divider` | int | `1` | `1` … `4` | Dělitel snímkové frekvence panelu. `2` = 30 fps na 60 Hz, tedy poloviční práce. | CLI / JSON |
| `frame_divider_idle` | int | `0` | `0` … `8` | Dělitel **v klidu** (nástroj mlčí, nikdo se ho nedotýká). `0` nebo hodnota ≤ `frame_divider` = nezpomalovat. Nahoru se přepíná okamžitě, dolů až po 2 s. | CLI / JSON |

> Hodnoty se **sanitizují**, ne clampují: `std::clamp(NaN, lo, hi)` vrací NaN,
> protože všechna porovnání s NaN jsou nepravdivá. Ručně editovaný soubor tedy
> může obsahovat cokoli — `nan` i `-5` skončí jako `0`, `1e9` jako strop.

### DSP řetěz — generická sekce `dsp.`

DSP řetěz zpracovává smíchaný stereo buffer v pevném pořadí
**Convolver → AGC → Enhancer → Limiter**. Všechny fáze jsou defaultně **vypnuté**,
takže čerstvá konfigurace zní transparentně.

Od schematu **v5** už to nejsou ploché klíče jako `convolver_mix`, ale jedna
generická sekce tvaru:

```
"dsp.<STÁGE>.<Param::id>": <hodnota>
```

Proč: dřív byl každý parametr vypsaný ručně na **šesti místech** (struktura,
načtení, uložení, aplikace na engine, zrcadlení, porovnání) a zapomenutí
kteréhokoli byla **tichá** chyba — parametr se prostě neuložil. Teď se přidání
parametru do stage nikde jinde neprojeví.

Klíčem je `Param::id`, tedy stabilní identifikátor určený právě pro persistenci
(ne popisek v GUI). Neznámý klíč se při načtení ignoruje, chybějící nechá stage
její vlastní default — starší i ručně editovaný soubor tedy prochází.

Ke každé stáge se navíc ukládá `enabled` (bool) a u Convolveru `choice`
(index voliče IR; `-1` = stránka volič nemá).

| Stáge | `Param::id` | Default | Rozsah | Jednotka | Význam |
|---|---|---|---|---|---|
| `CONVOLVER` | `mix`          | `0.15` | `0` … `1` | — | Poměr wet/dry (nízko = jemné tělo) |
| `CONVOLVER` | `decay`        | `0.50` | `0` … `1` | — | Tvarování doznìvání IR |
| `CONVOLVER` | `tone`         | `0.60` | `0` … `1` | — | Dolní propust IR (barva) |
| `CONVOLVER` | `size`         | `0.50` | `0` … `1` | — | Velikost těla (modální frekvenční posun) |
| `AGC` | `target_rms`         | `0.15` | `0.01` … `0.5` | RMS | Cílové RMS, ke kterému AGC stahuje |
| `AGC` | `release_ms`         | `200`  | `10` … `2000` | ms | Release AGC (attack je pevně 5 ms) |
| `AGC` | `gain_floor`         | `0.05` | `0` … `1` | zisk | Minimální podlaha zisku |
| `ENHANCER` | `process`       | `0`    | `0` … `12` | dB | Dynamický boost vyššího pásma + exciter |
| `ENHANCER` | `contour`       | `0`    | `0` … `12` | dB | Boost nízkého pásma |
| `ENHANCER` | `mid`           | `0`    | `-6` … `6` | dB | Presence zvon (~2,7 kHz) |
| `LIMITER` | `threshold_db`   | `0`    | `-40` … `0` | dB | Práh peaku (stereo-linkovaný) |
| `LIMITER` | `release_ms`     | `200`  | `10` … `2000` | ms | Release limiteru (attack je pevně 1 ms) |

Rozsahy pocházejí z tabulek `Param` v
`engine/dsp/{convolver,agc,enhancer,limiter}.cpp`.

### Sekce `defaults.` — uživatelský profil

Stejný tvar, jiný prefix. Je to snapshot, na který vrací tlačítko
`RESET TO FACTORY PROFILE` … respektive to, co si uživatel uložil jako svůj
výchozí stav:

```
"dsp.CONVOLVER.mix": 0.4,          <- živý stav
"defaults.CONVOLVER.mix": 0.25,    <- uživatelský default
```

Pokrývá **všechny** stránky parametrů, tedy i `MASTER` a `RESONANCE`, které
žádná DSP stage nejsou — klíčem je jméno stránky. Prázdná sekce znamená, že si
uživatel žádný profil neuložil a `RESET` jede na tovární `Param::def`.

USER profil se ukládá **sám při ukončení** — čím nástroj vypnete, s tím ho zase
zapnete. Podrobnosti v [H · GUI](../reference/H-gui.md).

### Volič stránky CONFIGu

| JSON klíč     | Typ  | Default | Povolené hodnoty | Význam | Nastavuje |
|---------------|------|---------|------------------|--------|-----------|
| `config_page` | int  | `0`     | `0`–`5`          | Vybraná stránka CONFIGu: `0`=MASTER, `1`=RESONANCE, `2`=CONVOLVER, `3`=AGC, `4`=ENHANCER, `5`=LIMITER. Hodnoty mimo rozsah se při startu ořežou na `0`. | GUI (přepínač CONFIG) |

### Zvukové zařízení

| JSON klíč           | Typ  | Default | Význam | Nastavuje |
|---------------------|------|---------|--------|-----------|
| `audio_block_size`  | int  | `256`   | Velikost bloku audio callbacku (latence). Měnitelná za běhu z GUI z comba BUFFER; ořezává se na `[32, 8192]`. | GUI (combo BUFFER) |
| `audio_sample_rate` | int  | `48000` | Sample rate enginu. **Jen v JSONu / v GUI read-only** (GUI ho zobrazuje, ale nemění). Nekladná hodnota padá zpět na 48000. Jak se SR samplu převádí na tenhle engine SR za běhu a jaký je doporučený offline workflow resamplingu, viz [kapitola o formátu banky](05-format-banky.md) (sekce o sample rate / resamplingu). | jen JSON |

### Logování

| JSON klíč   | Typ    | Default  | Povolené hodnoty | Význam | Nastavuje |
|-------------|--------|----------|------------------|--------|-----------|
| `log_level` | string | `"info"` | `debug` \| `info` \| `warn` \| `error` \| `fatal` (parser přijme i `off`/`warning`) | Minimální závažnost logu, aplikuje se přes `setMinSeverity`. Neparsovatelné hodnoty padají zpět na `info`. | CLI přepínač `--log-level` (přepíše při startu) / GUI (živě) |

Řetězce závažnosti parsuje `severity_from_string` (`engine/util/log.h`):
`debug`, `info`, `warn`/`warning`, `error`, `fatal` (necitlivé na velikost
písmen). `Off` existuje jako interní úroveň závažnosti, která potlačí veškerý
výstup.

## Přepsání z příkazové řádky

`ithaca-gui` přijímá přepínače (`app/gui/main.cpp`), které **při startu**
přepíšou perzistované hodnoty — ještě předtím, než se stav aplikuje na engine:

| Přepínač | Přepisuje | Chování |
|---|---|---|
| `--bank-dir <cesta>` | `bank_search_dir` | Adresář pro hledání bank. Aplikuje se, jen pokud není prázdný. |
| `--log-level <úr>` | `log_level` | `debug` \| `info` \| `warn` \| `error` \| `fatal`. |
| `--wave-glow <f>` | `wave_glow` | Dosah záře vlny, `0` … `128`. |
| `--wave-glow-budget <ms>` | `wave_glow_budget_ms` | Strop periody snímku pro automatiku, `0` = vypnuto. |
| `--frame-divider <n>` | `frame_divider` | Dělitel snímkové frekvence, `1` … `4`. |
| `--frame-divider-idle <n>` | `frame_divider_idle` | Dělitel v klidu, `0` … `8`. |

Nepersistované (platí jen pro daný běh):

| Přepínač | Chování |
|---|---|
| `--fullscreen` | Režim panelu. Na KMSDRM je to výchozí stav a přepínač se jen zaloguje. |
| `--video-driver <n>` | Vynutí SDL backend (`kmsdrm`, `x11`, `wayland`, …). |

`--help` / `-h` vypíše nápovědu a skončí.

Důležité: přepsaná hodnota se **perzistuje** jako každá jiná. Jakmile jednou
před```json
{
  "schema_version": 7,                 // vzdy 7
  "bank_search_dir": "/home/me/banks", // adresar pro hledani bank
  "bank_path": "",                     // banka nactena pri startu ("" = zadna)
  "midi_port_name": "",                // MIDI port hledany podle podretezce
  "log_level": "info",                 // debug|info|warn|error|fatal
  "midi_channel_mask": 65535,          // 0xFFFF = vsech 16 kanalu (OMNI)
  "master_gain_db": 0,                 // -60 .. 6 dB
  "resonance_enabled": true,           // sympaticka rezonance zapnuta
  "resonance_gain_db": -12,            // -60 .. 0 dB
  "resonance_layer_db": -30,           // -60 .. 0 dB (cilova vrstva)
  "release_ms": 200,                   // 50 .. 2000 ms
  "excite_decay_ms": 5000,             // 500 .. 30000 ms
  "max_resonance_voices": 32,          // 1 .. 64 (zivy posuvnik v GUI)
  "resonance_window_ms": 12000,        // okno RAM-cache (jen JSON, bez GUI)
  "preload_ms": 150,                   // preload hlavy na sampl (jen JSON)
  "cache_budget_mb": 0,                // 0=auto (~60% RAM); RAM rozpocet (jen JSON)
  "window_x": 100,                     // px (mimo obrazovku se orizne)
  "window_y": 100,                     // px
  "window_w": 1280,                    // px
  "window_h": 720,                     // px (HW cil 1280x720)
  "config_page": 0,                    // 0=MASTER 1=RESONANCE 2=CONVOLVER 3=AGC 4=ENHANCER 5=LIMITER
  "audio_block_size": 256,             // 32 .. 8192
  "wave_glow": 1,                      // dosah zare vlny; 0 = hola cara
  "wave_glow_budget_ms": 0,            // 0 = automatika vypnuta
  "frame_divider": 1,                  // 1 = kazdy vsync, 2 = 30 fps na 60 Hz
  "frame_divider_idle": 0,             // 0 = v klidu nezpomalovat
  "audio_sample_rate": 48000,          // jen JSON / v GUI read-only

  // Genericke sekce: "<prefix><STAGE>.<Param::id>". Carka se pise PRED kazdy
  // radek, takze prazdna mapa nenecha visici carku.
  "dsp.CONVOLVER.mix": 0.15,
  "dsp.CONVOLVER.decay": 0.5,
  "dsp.AGC.target_rms": 0.15,
  "dsp.LIMITER.threshold_db": 0,
  "dsp.CONVOLVER.enabled": false,      // kazda stage ma enabled
  "dsp.CONVOLVER.choice": 0,           // volic IR (jen Convolver)

  "defaults.MASTER.master_db": 0,      // uzivatelsky profil, stejny tvar
  "defaults.CONVOLVER.mix": 0.25
}
```onvolver_enabled": false,          // convolver defaultne vypnuty
  "convolver_mix": 0.15,               // 0 .. 1 wet/dry
  "convolver_choice": 0,               // 0=Body soft, 1=Body bright (modal)
  "convolver_decay": 0.5,              // 0 .. 1
  "convolver_tone": 0.6,               // 0 .. 1
  "convolver_size": 0.5,               // 0 .. 1
  "audio_block_size": 256,             // 32 .. 8192
  "audio_sample_rate": 48000           // jen JSON / v GUI read-only
}
```

Booly se zapisují jako `true`/`false`; floaty používají výchozí formátování `<<`
(celočíselné floaty jako `200` se vypíšou bez desetinné tečky).

## Poznámky a nástrahy

- **DSP defaulty = všechny fáze vypnuté.** `dsp.<STÁGE>.enabled` je u všech
  čtyř fází defaultně `false`, takže čerstvá konfigurace nijak nemění zvuk skrz
  DSP řetěz.
- **Rezonance je defaultně zapnutá** (`resonance_enabled: true`), na rozdíl od
  DSP fází.
- **Význam indexu `config_page`:** `0`=MASTER, `1`=RESONANCE, `2`=CONVOLVER,
  `3`=AGC, `4`=ENHANCER, `5`=LIMITER. Hodnoty mimo `0`–`5` se resetují na `0`.
- **`resonance_window_ms`, `preload_ms`, `cache_budget_mb` a `audio_sample_rate`
  nemají v GUI žádný ovládací prvek** — nastavují se jedině tímhle souborem
  (ladění inicializace enginu, hodí se pro embedded cíle jako RPi5).
  `max_resonance_voices` živý ovládací prvek v GUI *má* (posuvník „MAX RESONANCE"
  na stránce RESONANCE).
- **OOM pojistka.** `cache_budget_mb` (auto = ~60 % RAM) limituje načtení banky;
  při překročení se načítání přeruší s chybou (nekompletní banka), místo aby
  spadlo. Počty vláken stream workerů se **autodimenzují** podle počtu jader CPU
  při inicializaci enginu (žádné JSON pole) — ~polovina jader na hlavní
  streaming, ~čtvrtina na rezonanci.
- **Ořez okna mimo obrazovku.** Při startu GUI obnoví `window_x`/`window_y` a pak
  zkontroluje, jestli se aspoň oblast 100×100 px okna překrývá s nějakým
  připojeným monitorem. Pokud ne (třeba se od posledního uložení odpojil
  monitor), okno spadne zpět na pozici `(100, 100)` a tenhle fallback se zapíše
  zpět.
- **Banka a MIDI jsou best-effort.** Nenačitatelný `bank_path` nebo nenalezený
  `midi_port_name` jen zaloguje varování; GUI i tak nastartuje. Nalezenému MIDI
  portu se přesný název zapíše zpět do `midi_port_name`.
- **Debounce porovnává CELÝ stav.** Detekce změn v `main.cpp` už není ruční
  řetěz porovnání jednotlivých polí — používá `GuiState::operator==` (default),
  takže nové pole se hlídá samo. Dřívější ruční řetěz na `bank_search_dir`
  zapomínal.

  Jediná výjimka je **geometrie okna**, která je z porovnání vyřazená: mění se
  při každém posunu okna a spouštěla by ukládání pořád dokola. Ukládá se stejně
  při každém zápisu i při ukončení.

Výchozí rozsahy v `persistence.h` a tabulky `Param` se **shodují** — a od schematu
v5 už ani shodovat nemusí: rozsahy žijí **jen** v `Param` a persistence je nezná.

## Kudy dál

| Chci… | Jdi na |
|-------|--------|
| pochopit formát banky a jak se řeší sample rate / resampling | [5 · Formát banky](05-format-banky.md) |
| postavit projekt | [2 · Build a Makefile](02-build-makefile.md) |
| rozjet to na Raspberry Pi 5 | [3 · Raspberry Pi 5](03-raspberry-pi-5.md) |
| nahlédnout do enginu | [Část II — reference](../reference/00-overview.md) |
| vědět, co se chystá | [Plán a nedodělky](../plan2do.md) |
