# GUI

Oblast `app/gui/` implementuje **čelní panel nástroje Ithaca Legacy** nad Dear ImGui (backend SDL3 + OpenGL 3.3, na Raspberry Pi OpenGL ES 3 přes KMSDRM). Není to desktopová aplikace, která umí běžet i na Pi — je to panel zabudovaný v nástroji, ovládaný prstem.

Z toho plyne jediná tvrdá podmínka, ze které se odvozuje skoro všechno ostatní: prst potřebuje ~9 mm. Nástroj cílí na **dva panely** a obojí má prakticky stejnou hustotu, takže ta podmínka je na obou táž — **74 px**:

| panel | rozlišení | DPI | tělo stránky |
|---|---|---|---|
| 7,0" | 1280×720 | 210 | 1224 × 458 |
| 4,3" | 800×480 | 217 | 744 × 286 |

Rozdíl mezi nimi **není v měřítku, ale v ploše**. Škálování prvků by tedy byla špatná odpověď; správná je jiné rozvržení — viz [Profily displeje](#profily-displeje).

Životní cyklus řídí `main()`: načti persistovaný `GuiState` → SDL init a okno → GL kontext → ImGui a zabudovaná písma → `AppContext` (engine, audio, MIDI; banka se načítá asynchronně) → **render loop** → finální `saveState` → shutdown v opačném pořadí.

Provozní stránka (balíčky, oprávnění, boot do konzole, ladicí páky) je v [6 · Panel na Raspberry Pi](../prirucka/06-panel-a-rpi.md).

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
| `main.cpp` | CLI, SDL/ImGui init, render loop, modální overlay, debounce, tempo, shutdown |
| `screen.cpp` | Shell: rámeček, záložky, dispatch stránek, pozadí s vlnou, spořič, patička |
| `splash.{h,cpp}` | Úvodní obrazovka |
| `pages.h` | Deklarace stránek + `Rect` |
| `page_play.cpp` | PLAY — stav i prohlížeč bank |
| `page_bank.cpp` | BANK — procházení adresářů |
| `page_params.cpp` | Generický renderer `IParamPage` + stránka DSP s podzáložkami |
| `page_sys.cpp` | SYS — MIDI port a kanály, audio buffer, profily |
| `page_log.cpp` | LOG |
| `widgets.h` | Znakové primitivy kreslené přes `ImDrawList` |
| `theme.h` | Paleta, písma, `apply_theme()`, `load_fonts()` |
| `layout.h` | Rozměry + `splitRow` (viz níže) |
| `motion.h` | Tlumený doběh (integruje se po pevných podkrocích, viz níže) |
| `frame_stats.h` | Statistika snímků — percentily z histogramu + geometrie |
| `glow_auto.h` | Regulátor dosahu záře pod tlakem |
| `pace.h` | Přepínání tempa překreslování |
| `app_context.{h,cpp}` | Vlastník engine/audio/MIDI + `PanelState` |
| `dsp_state.h` | Snapshot/obnova/porovnání `IParamPage` — most `DspChain` ↔ `GuiState` |
| `state_binding.h` | Most `GuiState` ↔ `Engine` |
| `persistence.{h,cpp}` | `state.json`, schema v7 |
| `embedded_fonts.cpp` | Zabudovaná písma |
| `log_subscriber.{h,cpp}` | Kruhový buffer log eventů |

**Testy:** `test_persistence`, `test_gui_dsp_state`, `test_gui_state_binding`, `test_log_subscriber`, `test_frame_stats`, `test_glow_auto`, `test_pace`, `test_gui_render_golden`.

---

## Jak se panel měří a posuzuje bez displeje

Tohle je infrastruktura, na které stojí všechno ostatní: vývoj běží na PC, cílový panel je jinde, a **měření času je na desktopu bezcenné**.

### Otisk snímku (`test_gui_render_golden`)

ImGui žádný backend nepotřebuje — `Render()` vyprodukuje `ImDrawData` i do prázdna. Otisk vertex a index bufferu je tedy přesný popis toho, co by se nakreslilo: **když se otisk nezmění, nezměnil se ani obraz**. 15 scénářů (7 stránek × dvě rozlišení + spořič).

Vše, co by četlo skutečný stroj (MIDI porty, adresáře s bankami), je předvyplněné pevnými hodnotami. Baseline je **lokální** (build adresář), ne commitnutá: rasterizace písma se liší mezi překladači a sdílená baseline by hlásila falešné poplachy.

> Chytilo to chybu, kterou by nic jiného nenašlo: šířka buňky spočítaná jako `q.x - p.x` místo `row.cell` je matematicky totéž, ale ve floatu ne — čtverce záložek se posunuly o zlomek pixelu **při nezměněných počtech vertexů**.

### Software rasterizér

`ImDrawData` jsou obyčejné trojúhelníky s barvou a UV, takže si je jde složit do obrázku a **panel si prohlédnout bez panelu**. Zapíná se `ITHACA_GOLDEN_DUMP=<scénář>`, `tools/ppm2png.py` z výsledku udělá PNG.

Režimy náhledu `ITHACA_GLOW` (jiný dosah záře) a `ITHACA_WAVE_STEP` (skok v historii místo hladkého tvaru) umí vyrobit stav, který na běžícím panelu nenastane. Otisk se v nich **neporovnává** — je to prohlížení, ne ověřování.

### Statistika snímků (`frame_stats.h`)

Sbírají se **dvě různé veličiny, každá odpovídá na jinou otázku**:

| | povaha | k čemu |
|---|---|---|
| **čas** (`cpu`, `present`) | zašuměný | až na Pi; agreguje se do log-histogramu a čtou se percentily |
| **geometrie** (`vtx`, `idx`) | **nezašuměná** | čistá funkce toho, co kreslíme — účinek optimalizace jde ověřit exaktně i na PC |

Průměr se záměrně nepočítá: jedna špička z plánovače ho posune tak, že přestane cokoli znamenat. Naměřeno: `cpu p50 = 0,40 ms`, `p95 = 0,57 ms`, jeden výkyv 153 ms se projevil jen v `max` a `late`.

**Kde vést hranici:** `cpu_ms` končí u `ImGui::Render()`, ne u swapu. První pokus měřil až za `SwapBuffers` a vyšel `p50 = p95 = 18,1 ms`, tedy přesně perioda snímku — ovladač na Windows na vsync neblokuje ve `SwapBuffers`, ale už v dřívějším GL volání. Rozpoznat to šlo právě podle histogramu: `p50 == p95 == jedna přihrádka` znamená hodiny, ne práci.

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

Pod obsahem je **patička**: štítek `ITHACA LEGACY [USER]` / `[FACTORY]` vlevo, kontrolky `UNDERRUN · CLIP · LOG` uprostřed, audio režim vpravo. Značka za štítkem říká, na kterém profilu nástroj jede — údaj, který chceš vidět, aniž bys kvůli němu lezl na SYS, a štítek je jinak mrtvé místo. Kontrolky měly dřív vlastní pás, který ukrajoval 36 px výšky na každé stránce — na 7" panelu citelně.

Patička sedí u spodní hrany se **stejným odsazením, jaké má pás záložek od horní**. Dřív měla vlastní pásmo pevné výšky a text se kreslil u jeho horního okraje, takže pod ním zbývalo 20 px navíc a patička opticky plavala nad spodkem displeje.

### Stránky

**PLAY** — stav i prohlížeč bank zároveň. Výtah: vybraná banka uprostřed inverzním polem, sousedi tlumeně. Sedí na **svislém středu displeje**, ne na středu vlastní plochy (ta je nesymetrická).

Dole osm sloupců na společné účaři: `VOICES · RESO · RING · RING RESO · PEAK dB · DSP · SUSTAIN` + dvojice MIDI lamp. Stav streamovacích ringů je tady, ne na SYS: odečítá se **při hraní** (podle něj se pozná, že banka nestíhá z disku), a SYS je stránka na nastavování, kam se za hraní neleze. Popisky jsou zkrácené — na osm sloupců není místo na `MAIN RINGS`.

Čísla se překreslují **osmkrát za vteřinu**, ne každý snímek: při 60 fps se hodnota mění rychleji, než ji stihneš přečíst, a zároveň to drží stejnou živost jako vlna v pozadí, takže se ty dvě věci na obrazovce nerozcházejí. `VOICES`, `RING`, `PEAK dB` a `DSP` drží maximum za okno (u špičky je zajímavá právě ona), `SUSTAIN` poslední hodnotu — max-hold by po puštění pedálu ještě chvíli ukazoval 127.

**BANK** — procházení adresářů. Na panelu není klávesnice, takže cestu nelze napsat; bez `bank_search_dir` se prochází od adresáře startu. Zobrazují se jen adresáře, které vypadají jako banka — rozpoznání je **levná sonda** (existuje uvnitř `m###/` nebo `.ithaca`?), ne plný sken: procházení musí být okamžité. **RELOAD dostává celý řádek** u spodní hrany: je to jediná akce stránky a úzké tlačítko v rohu se prstem hledá hůř než pás přes celou šířku.

**TONE / RESO / DSP** — jedou přes `pageParams`, generický renderer `IParamPage`. DSP má druhý řádek záložek pro čtyři stage a jeho ON/OFF přepínač i volič IR stojí v **jednom roztaženém řádku**.

**SYS** — MIDI port, MIDI kanály, audio buffer, profily. Vše, co se nastaví jednou; proto je tu i RESET, na PLAY by se dal trefit omylem. Rozteč řádků se počítá z **dostupné** výšky (`SysMetrics`), ne z konstant — kolik na SYS zbyde, závisí na výšce záložek, a ta na šířce displeje.

Úroveň logu ani stav ringů tu **záměrně nejsou**: log patří na stránku LOG, kde potřeba ho přepnout vzniká, a ringy se odečítají za hraní, takže patří na PLAY. Uvolněná výška padne vhod kanálům.

**MIDI kanály jsou maska, ne výběr jednoho.** Šestnáct polí ve **dvou řádcích po osmi**; klepnutí rozsvítí, další klepnutí zhasne, sousedi zůstanou. OMNI není položka nabídky — je to prostě stav, kdy svítí všech šestnáct. Dvě věci tím získáme: jde nastavit libovolná **podmnožina** kanálů (třeba jen 1 a 3), což jediný volič neuměl, a na užším panelu nabídka nepřetéká do `BUFFER` pod ní. Vpravo od popisku svítí `ALL (OMNI)` / `n OF 16` / `NONE - MIDI MUTED` — prázdná maska je legitimní volba, ale bez té cedule by vypadala jako porouchané MIDI.

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

### Záři nese geometrie, ne obtahování

Původně to byly **tři obtahy přes sebe** (široký slabý, střední, úzký jasný). Tak se ale požadovaný tvar udělat nedá: každý obtah má konstantní průhlednost přes celou svou tloušťku, takže součet je schodiště, a ten nejvnitřnější je plný přes celou šířku. Výsledek se četl jako **plochý pás s hranou**, ne jako čára, která pohasíná do okolí.

`wdg::waveGlow` proto posílá **pás trojúhelníků s průhledností přímo ve vrcholech** — grafika mezi nimi interpoluje spojitě:

| vlastnost | jak |
|---|---|
| profil | Gaussova křivka, **plató v jádře** (bez plató má plná krycí oblast nulovou šířku a rasterizace ji rozředí — čára pak není vidět) |
| vzorkování | dráhy rozložené kvadraticky: hustě u jádra, řídce v ohonu |
| počet drah | dopočítá se z **dosahu** (`glowLanes`), roste s jeho logaritmem: 5–10 px → 5 drah, 32 px → 7, 1024 px → 12 |
| jas jádra | dosvětlené (`kCore = 1,45`) na úroveň, kterou skládaly tři obtahy |

Vedlejší efekty téhle změny jsou dva a oba se hodí: je to **levnější** než tři obtahy, a záře **vůbec neprochází přes ImGui rasterizaci tlustých čar** — odpadá tím závislost na tom, jestli tloušťka náhodou vyjde na celé číslo, i otázka, jak se to chová na GLES.

Dosah je **parametr** (`wave_glow`) s automatikou (`glow_auto.h`), protože je to jediná věc na panelu, která roste s **výplní** — a výplň je na V3D úzké hrdlo. Podrobnosti a doporučené hodnoty v [6 · Panel na Raspberry Pi](../prirucka/06-panel-a-rpi.md#ladicí-páky).

### Čtyři chybné modely, které tomu předcházely

Stojí za zaznamenání, protože každý vypadal rozumně:

1. **Kreslit průběh vzorků.** Neklidné při nokturnu.
2. **Normalizovat špičku RMS referencí.** Špička je u hudby několikanásobek RMS, takže podíl trvale seděl na dorazu limiteru — vypadalo to jako clipping. Řešení: dvě nezávislé reference.
3. **Zvuk přičítat k výchylce.** Znaménková špička přeskakuje mezi + a − každý frame → zubatá čára se schody. Řešení: zvuk moduluje *amplitudu*.
4. **Tvarovat záři obtahy.** Viz výše — konstantní průhlednost přes tloušťku nedá spojitý spád.

A dvě chyby výpočtu:

- Prostorové vyhlazení historie se počítalo modulem přes kruhový buffer, takže na levém okraji sáhlo „před nejnovější vzorek" a přeteklo na konec kruhu (data stará dvě sekundy) — vlna se tam tvrdě lámala. Index se teď **ořezává**.
- Totéž vyhlazení používalo **obdélníkové okno**, které má na skok odezvu ve tvaru rampy s ostrými rohy. Při náhlém zesílení se ty rohy objevily na křivce jako zlomy. Na hladkém vstupu se to neprojeví vůbec, takže to nešlo poznat ani pohledem na běžící panel — musel se vyrobit skok (`ITHACA_WAVE_STEP`). Okno je teď **Hannovo**, šířka volená tak, aby síla vyhlazení zůstala stejná (obojí má efektivní šířku ~2,0 vzorku).

### Cena

Naměřeno: panel bez vlny má 600–1200 vertexů, s vlnou 7 000–8 200 — **vlna je 85–90 % veškeré geometrie**. Proto se všechny páky týkají jí.

Na desktopu je rozdíl mezi zapnutou a vypnutou vlnou v šumu. Na Pi to bude jinak, a hlavní roli tam nehraje počet vertexů, ale **vyplněná plocha** — ta roste s druhou mocninou dosahu záře.

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
| `snapshotPages` / `applyPagesState` | Pole stránek — pohání USER profil (uložení i obnovu) |
| `dspStateFromChain` | Chain → state, každý frame (panely mění stage přímo) |
| `applyDspStateToChain` | State → chain při startu; chybějící klíč se přeskočí |

> **Pořadí:** musí běžet až po `engine.init()` — ten volá `dsp_.prepare()` a teprve tam Convolver naplní seznam IR.

---

## Profily FACTORY a USER

Nástroj zná dva profily parametrů:

| | Kde žije | Kdy se mění |
|---|---|---|
| **FACTORY** | zapečený v binárce (`Param::def` na každé stránce) | nikdy — je to záchranná síť |
| **USER** | sekce `defaults` v `state.json`, pokrývá **všechny** stránky včetně DSP řetězce | sám při ukončení programu, nebo tlačítkem |

**USER profil se ukládá sám při ukončení** — čím nástroj vypneš, s tím ho zase zapneš. Tlačítko `SET CURRENT AS USER PROFILE` na SYS dělá totéž, jen hned; hodí se, když si chceš stav pojistit ještě před hraním.

`RESET TO FACTORY PROFILE` vrátí **celý** řetězec včetně DSP a `RESONANCE LAYER`. Poloviční reset by u tlačítka s tímhle názvem lhal. Zároveň přepíše i USER profil — jinak by ho při ukončení přepsal automatický ukládač a v souboru by byly dvě různé verze podle toho, jestli mezitím uživatel na něco sáhl.

Ve štítku v patičce svítí `[FACTORY]`, dokud se hodnoty rovnají továrním, jinak `[USER]`. Porovnává se jen s továrním profilem: USER se ukládá sám, takže cokoli jiného než tovární hodnoty **už je** uživatelovo nastavení a třetí stav „rozpracováno" by nic neřekl.

Snapshot je klíčovaný jménem stránky a `Param::id`, takže nová stage nebo nový parametr projde **bez jediné změny v kódu**.

## `state_binding.h`

`engineConfigFromState` **validuje** persistované hodnoty (`sample_rate ≤ 0` → 48000, `block_size` clamp `[32, 8192]`, dB→lineár) a validované zapisuje **zpět do `state`** — jinak by GUI ukazovalo něco jiného, než čím engine jede.

---

## Persistence (schema v7)

Dvě generické sekce stejného tvaru `<prefix><STRÁNKA>.<Param::id>`:

```
"dsp.CONVOLVER.mix": 0.4,          <- živý stav
"defaults.CONVOLVER.mix": 0.25,    <- uživatelský default
```

`parseFlatJson` rozparsuje soubor v jednom průchodu a hlavně **umí klíče vyjmenovat**, což je pro obě sekce nutné — jejich jména persistence dopředu nezná.

Escape je plný včetně `\uXXXX` a surrogate párů; control znaky < 0x20 se zapisují escapované. Čárka se píše **před** každý řádek, takže prázdná mapa nenechá visící čárku.

Přijímá schema **3–7**. Migrace v3/v4 → v5 mapuje staré ploché klíče (včetně ještě staršího `bbe_*` → ENHANCER); v5 → v6 je čistě aditivní (starý soubor prostě sekci `defaults` nemá a `RESET` jede továrně); v6 → v7 odvodí `midi_channel_mask` ze starého `midi_channel` (−1 → 0xFFFF, *n* → 1<<*n*), takže se nastavení kanálu neztratí.

Persistence debounce porovnává **celý** `GuiState` (`operator== = default`); geometrie okna je z porovnání vyřazena, protože se mění při každém posunu.

---

## Profily displeje

`layout::Screen` se nastavuje jednou za snímek z velikosti displeje (`setScreen`). Compact profil se zapíná pod 1024×600 a mění **rozvržení**, ne měřítko:

| | Wide (1280×720) | Compact (800×480) |
|---|---|---|
| záložky | čtverec 172 px | obdélník 56 px (čtverec by měl 104 px = 23 % výšky) |
| PLAY | 7 sloupců stavu | dvě řádky, 4 + 3 |
| akční pás | `touch` (74 px) | `touch × 0,62` |
| výtah | středěný podle **obrazovky** | podle **vlastní plochy** (na úzkém panelu padne střed obrazovky až na spodní hranu a byla by vidět jediná položka) |

Na malém panelu se dřív rozpadaly tři stránky, a všechna tři selhání byla **tichá** — nic na obrazovce nenaznačovalo, že se něco nevejde:

- **SYS**: rozteč řádků vyšla 31 px, ale spodní mez buňky 36 px → řádky přes sebe, dotyk 29 px místo 74. Buňka se teď nikdy nevejde do menší rozteče, než má.
- **DSP a RESO**: na čtyři parametry zbylo 136 px, vešly se dva a smyčka na třetím dělala `break` — parametry prostě zmizely. Pás se teď smí stlačit pod `param_h_min` až na podlahu čitelnosti, a když se nevejdou ani tak, svítí cedule `+N MORE - NOT ENOUGH HEIGHT`.
- **PLAY**: osm sloupců po 93 px, do kterých se `128/256` nevejde.

---

## Tempo překreslování

Panel je přístroj, ne hra. Ambientní vizualizér 60 fps nepotřebuje a **poloviční tempo je poloviční práce**.

| parametr | co dělá |
|---|---|
| `frame_divider` | dělitel vsync (1 = každý, 2 = 30 fps na 60 Hz) |
| `frame_divider_idle` | dělitel v klidu; nahoru **okamžitě**, dolů až po 2 s |

Záměrně dělitel přes `SDL_GL_SetSwapInterval`, ne vlastní časovač se `sleep`: obraz zůstává synchronizovaný s panelem a netrhá se.

**Prerekvizita, bez které to nejde:** všechno vyhlazování vlny muselo přejít z „na snímek" na „na čas". Koeficienty jako `0,10` nebo `0,995` se dřív aplikovaly jednou za překreslení, takže při 12 fps by se vlna sama zpomalila pětkrát. Konstanty jsou přepočítané z původních při 60 fps (`τ = −(1/60)/ln(1−k)`) — při 60 fps tedy vypadá všechno stejně a liší se to teprve tam, kde bylo původní chování špatné.

Ověřeno měřením: 60 fps před a po převodu se liší **max o 1/255 na 19 kanálech z 2 764 800**. 60 vs 30 fps po převodu: liší se 0,43 % kanálů, z toho 96 % o ±1–2 — vzor je na stejném místě.

Totéž potkalo `motion::Settle`: explicitní Euler ořezával `dt` na 0,05 s kvůli stabilitě, takže při 12 fps by pružina dosedala 1,7× déle. Integruje se teď po **pevných podkrocích** — stabilita i skutečné tempo zároveň.

Rozhodování „děje se něco?" je na jednom místě: `panelAnimating(PanelState)` (čistě stavová část, testovatelná bez enginu) → `AppContext::busy()` (+ hlasy, rezonance, pedál, nedávné noty, load banky) → `PaceControl` (drží prodlevu).

---

## Režim panelu

```bash
./build/ithaca-gui --fullscreen [--bank-dir <cesta>] [--log-level <lvl>]
```

Okno bez dekorací přes celou plochu — **záměrně ne exkluzivní fullscreen**: ten přepíná mód obrazovky a na Pi komplikuje přepnutí na konzoli, když se něco pokazí.

Na **KMSDRM** žádný okenní manažer neexistuje a okno je vždy jedno přes celou obrazovku, takže je celá větev s pozicí, výčtem monitorů a off-screen clampem pod `#ifndef ITHACA_GLES` a `--fullscreen` se tam jen zaloguje jako no-op.

Dvě pasti, které to má ošetřené: geometrie okna se v režimu panelu (a na KMSDRM vždy) **nepersistuje** — jinak by se uložilo rozlišení přístroje a při příštím spuštění na PC by se okno otevřelo obří; a `renderScreen` dostává rozměry parametrem místo z `GuiState`.

---

## Křížové odkazy

| Oblast | Co GUI ovládá / čte |
|---|---|
| **Engine** | `engineConfigFromState` → `init`; `applyStateToEngine`; async `reloadBank`; `scopeSnapshot` (kruhový buffer 1024 vzorků výstupu za DSP řetězcem, bez zámku — roztržení snímku je na pozadí neviditelné, zámek v audio cestě by byl horší); diagnostika (`activeVoices`, `masterPeakL/R`, `pedalCC`, `dspLoadPeak`, ringy, underruny) |
| **DSP Chain** | `dsp_state.h` obousměrně přes `Param::id`; `IParamPage::resetToDefaults()` |
| **Audio** | `audio->start(...)`, `setAudioBlockSize` (stop → `setBlockSize` → start) |
| **MIDI** | `listPorts()` cachovaně, otevírání **podle jména**, ne indexu; `setChannelMask()` (bit i = kanál i+1, 0xFFFF = OMNI) |
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
