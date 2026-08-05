# GUI

Oblast `app/gui/` implementuje **čelní panel nástroje Ithaca Legacy** nad Dear ImGui (backend GLFW + OpenGL 3.3). Není to desktopová aplikace, která umí běžet i na Pi — je to panel zabudovaný v nástroji, ovládaný prstem.

Z toho plyne jediná tvrdá podmínka, ze které se odvozuje skoro všechno ostatní: cílem je **7" dotykový displej 1280×720 (~210 DPI)**, kde prst potřebuje ~9 mm, tedy **74 px**. Žádný interaktivní prvek nesmí být menší.

Životní cyklus řídí `main()`: načti persistovaný `GuiState` → otevři GLFW okno (v režimu panelu bez dekorací) → inicializuj ImGui a zabudovaná písma → `AppContext` (engine, audio, MIDI; banka se načítá asynchronně) → **render loop** → finální `saveState` → shutdown v opačném pořadí.

---

## Pojetí

Vzor: LCD/OLED panely starých syntezátorů (Roland D-550, Alesis). Světlý text na modrém podsvíceném poli.

| Pravidlo | Proč |
|---|---|
| **Důraz inverzí pole**, ne tučným řezem | Skutečné znakové displeje druhý řez nemají. Na dotyku navíc neexistuje hover, takže výběr musí být vidět bez najetí. |
| **Jedna modrá v odstínech** | Informaci nese jas a inverze, ne barevný kód. |
| **Jediná výjimka: přehlcení** | Od −9 dB se vlna barví do červena. Přetížení nejde ukázat jasem — vlna je při něm už tak nejjasnější. |
| **Rámeček jako mrtvá zóna** | Není to ozdoba: odsazení od kraje panelu, aby se u okraje nedalo omylem trefit ovládání. Černý, aby panel působil zapuštěně. |
| **Texty anglicky** | Trvalý jazyk rozhraní. Komentáře v kódu zůstávají česky. |

Písma jsou **zabudovaná v binárce** — za běhu se nečte žádný asset, jen `state.json`. Rozhraní má **jedinou váhu** (JetBrains Mono Regular), protože znakový displej žádnou jinou nemá. Wordmark na úvodní obrazovce je v úzkém grotesku (Barlow Condensed Bold + Light): na skutečných přístrojích měl displej znakový font, ale logo na panelu bylo sítotiskem.

---

## Soubory

| Soubor | Odpovědnost |
|---|---|
| `main.cpp` | CLI, GLFW/ImGui init, render loop, modální overlay, debounce, shutdown |
| `screen.cpp` | Shell: rámeček, záložky, dispatch stránek, pozadí s vlnou, spořič, patička |
| `splash.{h,cpp}` | Úvodní obrazovka |
| `pages.h` | Deklarace stránek + `Rect` |
| `page_play.cpp` | PLAY — stav i prohlížeč bank |
| `page_bank.cpp` | BANK — procházení adresářů |
| `page_params.cpp` | Generický renderer `IParamPage` + stránka DSP s podzáložkami |
| `page_sys.cpp` | SYS — MIDI, audio, log level, ringy, uživatelské defaulty |
| `page_log.cpp` | LOG |
| `widgets.h` | Znakové primitivy kreslené přes `ImDrawList` |
| `theme.h` | Paleta, písma, `apply_theme()`, `load_fonts()` |
| `layout.h` | Rozměry + `splitRow` (viz níže) |
| `motion.h` | Tlumený doběh |
| `app_context.{h,cpp}` | Vlastník engine/audio/MIDI + `PanelState` |
| `dsp_state.h` | Snapshot/obnova `IParamPage` — most `DspChain` ↔ `GuiState` |
| `state_binding.h` | Most `GuiState` ↔ `Engine` |
| `persistence.{h,cpp}` | `state.json`, schema v6 |
| `embedded_fonts.cpp` | Zabudovaná písma |
| `log_subscriber.{h,cpp}` | Kruhový buffer log eventů |

**Testy:** `test_persistence`, `test_gui_dsp_state`, `test_gui_state_binding`, `test_log_subscriber`.

---

## `layout::splitRow` — jediný sazeč řádku

Rozdělí vodorovný pás na **n stejně širokých buněk** a vrátí jejich souřadnice.

```cpp
const L::Row row = L::splitRow(origin, width, cell_h, n);
row.at(i);  row.end(i);  row.height();
```

Když si každý prvek měří šířku z vlastního textu, řádky pod sebou končí jinde a každý cíl je jinak velký — na dotyku se pak trefuješ podle toho, jak dlouhý popisek volba náhodou má. Stejně široké buňky drží svislice zarovnané napříč celou stránkou.

Používá to **všechno, co stojí ve řádku**: hlavní i podzáložky (`wdg::tabBar`), řádky voleb na SYS (`wdg::chipRow`), ON/OFF + volič IR na DSP, dvojice tlačítek dole.

Kdyby buňka klesla pod `Dims::cell_min`, řádek se zalomí. Na cílovém panelu se to nestane; je to pojistka pro okno na PC.

---

## Struktura

Plochých sedm stránek, statická lišta **nahoře**:

```
PLAY · BANK · TONE · RESO · DSP · SYS · LOG
```

Zvýrazněná záložka **je zároveň nadpis stránky**, takže stránky nemají vlastní hlavičku a vejde se na ně o jeden parametr víc. Lišta je nahoře, ne dole: původně byla dole kvůli lícování s budoucími fyzickými tlačítky, ale ta nemusí být pod displejem ani existovat.

Záložky jsou **čtverce o straně rovné šířce buňky** (~172 px na panelu 1280×720) — přepínání stránek je hlavní mechanismus ovládání, takže dostává největší plochu na panelu. Výška se počítá až za běhu (`layout::squareTabH`) ze šířky displeje, ne z konstanty; strop je `tab_h_max` a čtvrtina výšky, aby v širokém okně na PC nesnědly obrazovku.

Pod obsahem je **patička**: štítek `ITHACA LEGACY` vlevo, kontrolky `UNDERRUN · CLIP · LOG` uprostřed, audio režim vpravo. Kontrolky měly dřív vlastní pás, který ukrajoval 36 px výšky na každé stránce — na 7" panelu citelně.

Patička sedí u spodní hrany se **stejným odsazením, jaké má pás záložek od horní**. Dřív měla vlastní pásmo pevné výšky a text se kreslil u jeho horního okraje, takže pod ním zbývalo 20 px navíc a patička opticky plavala nad spodkem displeje.

### Stránky

**PLAY** — stav i prohlížeč bank zároveň. Výtah: vybraná banka uprostřed inverzním polem, sousedi tlumeně. Sedí na **svislém středu displeje**, ne na středu vlastní plochy (ta je nesymetrická). Dole šest sloupců na společné účaři: `VOICES · RESO · PEAK dB · DSP · SUSTAIN` + dvojice MIDI lamp.

**BANK** — procházení adresářů. Na panelu není klávesnice, takže cestu nelze napsat; bez `bank_search_dir` se prochází od adresáře startu. Zobrazují se jen adresáře, které vypadají jako banka — rozpoznání je **levná sonda** (existuje uvnitř `m###/` nebo `.ithaca`?), ne plný sken: procházení musí být okamžité. **RELOAD dostává celý řádek** u spodní hrany: je to jediná akce stránky a úzké tlačítko v rohu se prstem hledá hůř než pás přes celou šířku.

**TONE / RESO / DSP** — jedou přes `pageParams`, generický renderer `IParamPage`. DSP má druhý řádek záložek pro čtyři stage a jeho ON/OFF přepínač i volič IR stojí v **jednom roztaženém řádku**.

**SYS** — MIDI port a kanál, audio buffer, úroveň logu, stav ringů, uživatelské defaulty. Vše, co se nastaví jednou; proto je tu i RESET, na PLAY by se dal trefit omylem. Rozteč řádků se počítá z **dostupné** výšky (`SysMetrics`), ne z konstant — kolik na SYS zbyde, závisí na výšce záložek, a ta na šířce displeje.

**LOG** — severita jasem a inverzí, ne barvou. Dole **volba úrovně logu**: potřeba ji změnit vzniká právě ve chvíli, kdy výpis čteš. Je to tatáž funkce (`logLevelRow`), jakou používá SYS — jedna definice, aby se nabídka ani chování nerozešly.

---

## Ovládání

| Gesto | Chování |
|---|---|
| Klepnutí na řádek výtahu | Načte tu banku hned |
| Tažení | Posouvá; po puštění dojede tlumeným doběhem a **načte až po ustálení** |
| Slidery | Mění se **tahem, ne klepnutím** |

Obě zpoždění mají tentýž důvod: **načtení banky trvá vteřiny**. Kdyby se načítalo během rolování, projetí seznamu by spustilo desítky loadů za sebou.

U sliderů jde o jinou past: stock ImGui slider skáče na místo kliknutí a při testování staré verze to omylem přepsalo práh limiteru. Na dotyku by to bylo mnohem horší. Tah má práh `Dims::drag_slop`, aby se klepnutí nevyhodnotilo jako mikrotažení.

### Sazba parametrů

Slidery se sázejí **odspodu**: opticky se oddělí od záložek a prst na ně dosáhne dál od menu, takže při míření do kraje netrefí přepínač stránky.

Popisek i hodnota jsou **uvnitř pásu**, ne nad ním. Ušetří to 24 px na každém parametru (na DSP jsou čtyři) a pás pak vypadá jako tah faderu, ne jako formulářové pole. Text leží částečně na výplni a částečně na korytu, takže se kreslí **dvakrát, ořezaný na hraně výplně** — jedna barva by vždycky někde splynula.

Když se blok nevejde, výška pásu se stlačí až na `param_trk_min`. Slider se ovládá tahem přes celou šířku, takže mu nižší pás nevadí tolik jako tlačítku.

---

## Pozadí — ambientní vizualizér

Ve stylu PS3 XMB: gradient do světla vpravo nahoře + pět měkkých stuh se září.

**Tvar NENÍ průběh zvuku.** Kreslit vzorky přímo bylo při pomalém tempu příliš neklidné — pozadí má *indikovat*, že zvuk hraje, ne aby se z něj dal číst tvar vlny. Tvar je proto **parametrický** (součet tří pomalých sinusovek s driftující fází) a zvuk mu **moduluje amplitudu**.

Vlna **plyne zleva doprava, protože se přehrává**: posuvná historie hlasitosti, nová hodnota vstupuje zleva, průchod šířkou ~2 s.

| Vlastnost | Detail |
|---|---|
| Zdroje | L (světlý), R (hlubší modrý), pedál (dlouhý klidný nádech, amplituda ~3/4) |
| Osa | Střed vybraného patche — vlna protéká jménem banky |
| Vyhasínání | RMS okna proti **absolutnímu** prahu −74 dB; náběh rychlý, pokles ~4 s |
| Přehlcení | −9 dB → 0 dB přechod do červena |
| Sytost | PLAY 100 %, ostatní stránky 42 % (jinak prochází textem) |

### Tři chybné modely, které tomu předcházely

Stojí za zaznamenání, protože každý vypadal rozumně:

1. **Kreslit průběh vzorků.** Neklidné při nokturnu.
2. **Normalizovat špičku RMS referencí.** Špička je u hudby několikanásobek RMS, takže podíl trvale seděl na dorazu limiteru — vypadalo to jako clipping. Řešení: dvě nezávislé reference.
3. **Zvuk přičítat k výchylce.** Znaménková špička přeskakuje mezi + a − každý frame → zubatá čára se schody. Řešení: zvuk moduluje *amplitudu*.

A jedna chyba výpočtu: prostorové vyhlazení historie se počítalo modulem přes kruhový buffer, takže na levém okraji sáhlo „před nejnovější vzorek" a přeteklo na konec kruhu (data stará dvě sekundy) — vlna se tam tvrdě lámala. Okno se teď ořezává.

### Cena

Naměřeno na macOS: rozdíl mezi zapnutou a vypnutou vlnou je **v šumu** (~12,5 % vs ~12,4 % jednoho jádra), tedy hluboko pod 1 %. Na RPi5 to bude jinak — poroste výplň gradientu přes celou plochu a geometrie záře (5 stuh × 3 průchody × 128 bodů = 1920 úseček/frame). Páky, kdyby bylo těsno: zář na dva průchody, `kPts` na 96.

---

## Spořič

Po **pěti minutách bez doteku** se ovládací prvky pomalu vytratí a zůstane jen vlna. Hraní obrazovku **neprobouzí** — když hraješ, panel nepotřebuješ. První dotek vrátí vše hned (náběh 0,18 s, odchod 2,5 s).

Není to jen efekt: lišta, rámečky a inverzní pole jsou pořád na stejném místě, a na OLED panelu je to recept na vypálení.

**Vrstvení:** závoj překryje už nakreslené ovládání barvou pozadí a vlna se dokreslí znovu přes něj. Průhledný překryv přes všechno by ztlumil i vlnu.

---

## Úvodní obrazovka

Setření shora (jako test segmentů na LCD při zapnutí) → ITHACA přilétá shora, LEGACY zdola → **dotknou se** a v ten okamžik obě krátce zesílí zář → rozejdou se a dosednou. Jedna vlna, žádný druhý overshoot: přejetí je odměřené přesně na polovinu mezery mezi slovy.

Zesílení záře v kontaktu není ozdoba — na OLED a VFD panelech jas reálně kolísá se zátěží.

Pod wordmarkem běží **skutečné kroky initu** (engine, audio, MIDI, banka) s progresem, takže splash slouží i jako diagnostika: když se neotevře audio device nebo není MIDI port, je to vidět na místě, kde jinak svítí OK.

**Končí, až doběhne skutečný load**, ne po pevném čase. Minimum je doba animace, aby se neusekla v půlce.

---

## Modální overlay

Průběh načítání banky nebo hlášení o neplatné licenci.

Ukáže se **až u loadů delších než 350 ms** a nabíhá i mizí prolnutím. Pakovaná banka se načte za necelou vteřinu a overlay pak jen probliknul přes celý panel — nástroj takhle blikat nemá. Je to ztmavení, ne překrytí; pod ním je pořád vidět, co se děje. Progres je **řetěz bloků**, protože znakový displej plynulý pruh nemá.

Licenční hlášení se ukáže okamžitě, bez prodlevy: je to chyba čekající na potvrzení, ne průběh.

Během úvodní obrazovky se overlay nekreslí — první load je vidět na ní samotné.

---

## Generický stav parametrů

Historicky měl `GuiState` 16 plochých polí. Přidání jednoho parametru znamenalo úpravu na **šesti** místech a zapomenutí kteréhokoli byla **tichá** chyba. `Param::id` i `Param::def` přitom byly mrtvá pole.

```cpp
struct DspStageState {
    bool enabled = false;
    int  choice  = -1;                     // -1 = stránka nemá volič
    std::map<std::string, float> params;   // Param::id -> hodnota
};
std::map<std::string, DspStageState> dsp;
```

`dsp_state.h` pracuje s **`IParamPage`, ne s `DspStage`** — díky tomu tentýž mechanismus pokryje i MASTER a RESONANCE, které žádné DSP nejsou:

| Funkce | Vysvětlení |
|---|---|
| `snapshotPage` / `applyToPage` | Jedna stránka. Základ všeho ostatního. |
| `snapshotPages` / `applyPagesState` | Pole stránek — pohání SAVE AS DEFAULT a RESET PARAMS |
| `dspStateFromChain` | Chain → state, každý frame (panely mění stage přímo) |
| `applyDspStateToChain` | State → chain při startu; chybějící klíč se přeskočí |

> **Pořadí:** musí běžet až po `engine.init()` — ten volá `dsp_.prepare()` a teprve tam Convolver naplní seznam IR.

---

## Uživatelské defaulty

`RESET PARAMS` na SYS vrací na **tvoje** hodnoty, ne na tovární. `Param::def` je konstanta v kódu a o konkrétní bance ani sestavě nic neví; jakmile si nástroj naladíš, tovární default je horší výchozí bod než to, co právě zní správně.

- **SAVE AS DEFAULT** pořídí snapshot **všech** stránek parametrů (MASTER, RESONANCE i celý DSP řetězec) do sekce `defaults` v `state.json`.
- **RESET PARAMS** ho obnoví. Když žádný uložený není, jede tovární cesta jako dřív.
- Na stránce svítí `PARAM DEFAULTS: USER` / `FACTORY`, aby bylo poznat, co se stane.

Vlastní snapshot smí vrátit i DSP řetězec a `RESONANCE LAYER`, kterých se tovární reset záměrně nedotýká: smazat celý řetězec jedním klepnutím by překvapilo, ale vrátit se k tomu, co sis sám uložil, není destruktivní.

Snapshot je klíčovaný jménem stránky a `Param::id`, takže nová stage nebo nový parametr projde **bez jediné změny v kódu**.

---

## `state_binding.h`

`engineConfigFromState` **validuje** persistované hodnoty (`sample_rate ≤ 0` → 48000, `block_size` clamp `[32, 8192]`, dB→lineár) a validované zapisuje **zpět do `state`** — jinak by GUI ukazovalo něco jiného, než čím engine jede.

---

## Persistence (schema v6)

Dvě generické sekce stejného tvaru `<prefix><STRÁNKA>.<Param::id>`:

```
"dsp.CONVOLVER.mix": 0.4,          <- živý stav
"defaults.CONVOLVER.mix": 0.25,    <- uživatelský default
```

`parseFlatJson` rozparsuje soubor v jednom průchodu a hlavně **umí klíče vyjmenovat**, což je pro obě sekce nutné — jejich jména persistence dopředu nezná.

Escape je plný včetně `\uXXXX` a surrogate párů; control znaky < 0x20 se zapisují escapované. Čárka se píše **před** každý řádek, takže prázdná mapa nenechá visící čárku.

Přijímá schema **3–6**. Migrace v3/v4 → v5 mapuje staré ploché klíče (včetně ještě staršího `bbe_*` → ENHANCER); v5 → v6 je čistě aditivní — starý soubor prostě sekci `defaults` nemá a `RESET` jede továrně.

Persistence debounce porovnává **celý** `GuiState` (`operator== = default`); geometrie okna je z porovnání vyřazena, protože se mění při každém posunu.

---

## Režim panelu

```bash
./build/ithaca-gui --fullscreen [--bank-dir <cesta>] [--log-level <lvl>]
```

Okno bez dekorací přes celou plochu — **záměrně ne exkluzivní fullscreen**: ten přepíná mód obrazovky a na Pi komplikuje přepnutí na konzoli, když se něco pokazí.

> GLFW potřebuje běžící display server. Z holé konzole bez X11/Wayland okno nevytvoří.

Dvě pasti, které to má ošetřené: geometrie okna se v režimu panelu **nepersistuje** (jinak by se uložilo rozlišení panelu), a `renderScreen` dostává rozměry parametrem místo z `GuiState`.

---

## Křížové odkazy

| Oblast | Co GUI ovládá / čte |
|---|---|
| **Engine** | `engineConfigFromState` → `init`; `applyStateToEngine`; async `reloadBank`; `scopeSnapshot` (kruhový buffer 1024 vzorků výstupu za DSP řetězcem, bez zámku — roztržení snímku je na pozadí neviditelné, zámek v audio cestě by byl horší); diagnostika (`activeVoices`, `masterPeakL/R`, `pedalCC`, `dspLoadPeak`, ringy, underruny) |
| **DSP Chain** | `dsp_state.h` obousměrně přes `Param::id`; `IParamPage::resetToDefaults()` |
| **Audio** | `audio->start(...)`, `setAudioBlockSize` (stop → `setBlockSize` → start) |
| **MIDI** | `listPorts()` cachovaně, otevírání **podle jména**, ne indexu |
| **Loader** | `BankLoadProgress` atomiky, viz [F-loader](F-loader.md) |
| **Logger** | Subscriber → `LogRingBuffer`; background flush RT ringu po 10 ms |

---

## Nálezy revize

### Otevřené

Žádné.

### Mimo rozsah (čeká na rozhodnutí)

- **Mic-mix fadery** — čekají na engine ([plan2do](../plan2do.md)); generický renderer je nakreslí sám
- **Mapování HW kontrolerů** — struktura je připravená
- **Setlist** — řetěz bank pro rychlé přepínání
- **Jemné doladění hodnot** — podržení prstu → jemný režim
