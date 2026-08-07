# 6 · Panel na Raspberry Pi

Čelní panel nástroje má běžet **z holé konzole** — bez X11, bez Waylandu, bez
desktopu. Tahle kapitola je o tom, co k tomu bylo potřeba, jak to postavit a
čím to doladit, když je na pasivně chlazeném Pi těsno.

> **Stav ověření.** Překlad a běh na Windows ověřený. Na Raspberry Pi zatím
> **neověřený** — cesta je napsaná a build soubory ji nesou, ale první spuštění
> na reálném zařízení teprve proběhne. Kde je něco jen předpoklad, je to
> v textu označené.

---

## Proč to nešlo předtím

Panel běžel nad GLFW, a **GLFW okno z holé konzole nevytvoří** — potřebuje
běžící display server. Na přístroji, který má po zapnutí ukázat panel a nic
jiného, je to špatná vrstva.

SDL3 to řeší backendem **KMSDRM**: kreslí přímo na framebuffer přes rozhraní
jádra, bez jakéhokoli okenního systému. A protože tatáž vrstva pokrývá i macOS
(Cocoa) a Windows (Win32), neudržují se dvě větve téhož.

Platformově podmíněné zůstalo přesně to, co se čekalo: **jedna verze GLSL** a
pár řádků kolem okna. Tělo snímku je společné.

---

## Balíčky a k čemu jsou

Nejde o výčet z hlavy — SDL zapne KMSDRM jen když projdou **všechny tři**
podmínky v jeho `sdlchecks.cmake`:

```cmake
if(PC_LIBDRM_FOUND AND PC_GBM_FOUND AND HAVE_OPENGL_EGL)
```

| balíček | co to je a proč |
|---|---|
| **`libdrm-dev`** | *Direct Rendering Manager* — rozhraní na jádro, přes které se sáhne na grafiku bez X. Vyjmenuje displeje, nastaví režim a umí **page flip**, tedy prohození bufferů synchronizované s vsync. To je to, co u nás dělá `SDL_GL_SwapWindow`. |
| **`libgbm-dev`** | *Generic Buffer Management* — alokátor bufferů, do kterých GPU kreslí a které jde poslat na výstup. Spojka mezi EGL (co kreslí) a DRM (co zobrazuje). |
| **`libegl1-mesa-dev`** | EGL nahrazuje GLX. GLX je vázaný na X server, EGL ne — přes něj vznikne GL kontext nad GBM bufferem. |
| **`libgles2-mesa-dev`** | Hlavičky a knihovna GLES. V3D na Pi poskytuje OpenGL **ES**, ne desktopové GL; linkuje se `GLESv2`. |
| **`libudev-dev`** | Vyjmenování a hlídání vstupních zařízení. Bez něj by dotyk možná fungoval po startu, ale připojení panelu za běhu by se neprojevilo. |
| **`pkg-config`** | Není kosmetika: `libdrm` a `gbm` se hledají **výhradně** přes `pkg_check_modules`. Bez pkg-config se KMSDRM tiše nepostaví, i kdyby knihovny byly nainstalované. |
| **`libasound2-dev`** | ALSA — ale kvůli **miniaudio a RtMidi**, ne kvůli SDL. |

```bash
sudo apt install -y build-essential cmake git curl tar python3 pkg-config \
                    libasound2-dev \
                    libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev \
                    libudev-dev
```

### Co potřeba není

**Žádné X11.** `xorg-dev` a `libglfw3-dev` z dřívějška odpadají — byly kvůli
GLFW, které bez X serveru neexistuje.

**Žádné zvukové dev balíčky pro SDL.** `SDL_AUDIO` je při buildu vypnuté; zvuk
si nástroj řeší sám. Vedlejší efekt: SDL si nepřitáhne PipeWire ani
PulseAudio, což přesně odpovídá doporučení z [kapitoly 3](03-raspberry-pi-5.md).

### Jedna volba, která se dělá při buildu

Přepínač `--video-driver x11` je ladicí cesta pro Pi s desktopem. Funguje **jen
když je SDL postavené s X11 backendem**, a to vyžaduje `xorg-dev` **v době
překladu**:

- **jen přístroj** → X11 balíčky neinstalovat, build je rychlejší a binárka menší
- **i ladění na Pi s desktopem** → `sudo apt install -y xorg-dev`, jinak
  `--video-driver x11` skončí hláškou „not available"

---

## Oprávnění

Uživatel musí být kromě `audio` také ve skupinách **`video`**, **`render`**
(kvůli `/dev/dri/card*`) a **`input`** (kvůli `/dev/input/event*`). Bez toho SDL
ovladač najde, ale k zařízením se nedostane.

```bash
sudo usermod -aG audio,video,render,input "$USER"
# odhlásit a přihlásit, pak ověřit:
id -nG
```

---

## Boot do konzole

Desktop obsadí KMS a SDL backend `kmsdrm` pak selže. Pi musí bootovat do
konzole:

```bash
sudo raspi-config      # System Options → Boot / Auto Login → Console
```

Do `/boot/firmware/cmdline.txt` (jeden řádek, položky oddělené mezerou):

```
vt.global_cursor_default=0 consoleblank=0
```

- `vt.global_cursor_default=0` — pod panelem nebliká textový kurzor
- `consoleblank=0` — obrazovka po nečinnosti nezhasne (spořič si panel řeší sám)

---

## Build a ověření

```bash
make fetch-third-party     # doctest, nlohmann, miniaudio, RtMidi, ImGui, SDL3
make build
```

Volba `ITHACA_GUI_GLES` se na Linux/ARM zapíná **sama**. Zapíná
`IMGUI_IMPL_OPENGL_ES3` na targetu `imgui` — tam se překládá backend, takže
definovat to jen na aplikaci by rozešlo hlavičky.

Ověření, že se KMSDRM opravdu postavil, má dvě místa:

```bash
# 1. při konfiguraci
cmake -S . -B build 2>&1 | grep -i kmsdrm

# 2. za běhu — tenhle řádek je v kódu právě proto, aby se nemuselo hádat
./build/ithaca-gui --log-level info | head -3
#   [gui] [INFO]: Video driver: kmsdrm
```

---

## Ladicí páky

Všechny se persistují ve `state.json`, takže stačí zadat jednou.

### `--wave-glow <f>` — dosah záře vlny

Vlna v pozadí je jediná věc na panelu, která roste s **výplní**, a výplň je na
V3D to úzké hrdlo. Násobitel dosahu:

| hodnota | vertexů/snímek | jak to vypadá |
|---|---|---|
| `0` | 2 072 (−71 %) | holá čára, viditelné schody |
| `0.15` | 4 632 (−36 %) | tenká čistá linka, žádné halo |
| **`0.4`** | 5 912 (−18 %) | **doporučeno pro Pi** — pořád vypadá jako záměr |
| `1` | 7 192 | výchozí |
| `2` | 8 216 (+14 %) | zřetelné halo |

Zajímavé je, že **cena neroste s poloměrem lineárně**: mezi 0,15 a 4 je poloměr
27× větší, ale vertexů jen dvojnásobek — počet drah profilu roste logaritmicky.
Co roste kvadraticky, je vyplněná plocha, tedy zátěž GPU. Takže:

- šetřit **CPU** → jít na `0` (jen tam spadne počet drah na minimum)
- šetřit **GPU / výplň** → stačí `0.15`–`0.4` a vzhled zůstane kultivovaný

### `--wave-glow-budget <ms>` — automatika

Strop **periody snímku** (ne času kreslení). Když se překročí, regulátor dosah
sám sníží. Na panelu 60 Hz je nominál 16,7 ms, takže ~25 znamená zmeškaný snímek.

Měří se perioda schválně: počet drah roste s dosahem logaritmicky, zatímco
vyplněná plocha jeho druhou mocninou. Desetkrát širší záře stojí na CPU skoro
totéž a na GPU stonásobek — regulátor postavený na CPU čase by na ni téměř
nereagoval.

Regulace je záměrně hloupá a pomalá: diskrétní kroky, dolů po 3 s překročení,
nahoru až po 20 s klidu a jen pod polovinou rozpočtu. Záře, která dýchá se
zátěží, by byla horší než záře trvale menší — a dýchala by právě při hraní.

### `--frame-divider <n>` a `--frame-divider-idle <n>`

Dělitel snímkové frekvence panelu. `2` = 30 fps na 60 Hz, tedy **poloviční
práce**. Panel je přístroj, ne hra; ambientní vizualizér 60 fps nepotřebuje.

`--frame-divider-idle` klesne ještě níž, když nástroj mlčí a nikdo se ho
nedotýká. Nahoru se přepíná **okamžitě** (odpověď na dotek), dolů až po dvou
vteřinách klidu.

```bash
./ithaca-gui --frame-divider 2 --frame-divider-idle 4   # 30 fps / 15 fps
```

Doporučení pro Pi: `--wave-glow 0.4 --frame-divider 2 --frame-divider-idle 4`.

### `--video-driver <jméno>`

Vynutí backend (`kmsdrm`, `x11`, `wayland`). Na Pi je výchozí `kmsdrm` a
**vynucuje se schválně**: autodetekce by byla past — kdyby byl nainstalovaný
desktop a někdo přihlášený, SDL by sáhlo po Waylandu a panel by se otevřel do
okna na ploše místo na displej přístroje.

---

## Diagnostika za běhu

Statistika snímků jde do logu jednou za minutu na úrovni `debug` — zapíná se
**přepnutím úrovně na stránce LOG**, tedy bez rekompilace i na hotovém přístroji:

```
3604f 60.1fps | cpu p50=0.40 p95=0.57 max=163.51 ms late=2 | present p50=18.10 ms | vtx=7020 max=7476 idx=29148 cmd=2
```

| pole | co znamená |
|---|---|
| `cpu` | **naše** práce: události, logika panelu, stavba draw listu. Končí u `ImGui::Render()`. |
| `present` | vše za tím včetně čekání na vsync. Při 60 Hz a děliteli 1 má být ~16,7 ms. |
| `late` | snímky přes rozpočet — zajímavé je, když roste |
| `vtx` / `idx` | geometrie; **nezašuměná**, na rozdíl od času |

Percentily, ne průměr: jedna špička z plánovače průměr posune tak, že přestane
cokoli znamenat. Proto se drží `max` zvlášť.

Změna tempa se loguje taky (`Pace: delitel 2 -> 4`), takže jde poznat, že
úsporný režim naběhl a hlavně že se z něj vrací včas.

---

## Panel 4,3" 800×480

Nástroj cílí na **dva panely**, a rozdíl mezi nimi není v měřítku, ale v ploše:

| panel | rozlišení | DPI | tělo stránky |
|---|---|---|---|
| 7,0" | 1280×720 | 210 | 1224 × 458 |
| 4,3" | 800×480 | **217** | 744 × 286 |

Hustota je prakticky stejná, takže dotykový cíl 74 px platí na obou a **nic se
nezmenšuje**. Škálování (`--ui-scale`) by byl špatný nástroj; správná odpověď je
jiné **rozvržení**. Compact profil se zapíná sám podle velikosti displeje a mění:

- záložky z čtverců na obdélníky (čtverec by měl 104 px, tedy 23 % výšky)
- stavový řádek na PLAY do dvou řádek (7 sloupců po 106 px by čísla nepobralo)
- výtah středěný podle vlastní plochy, ne podle středu obrazovky

### DSI panel a dotyk

Pro Waveshare 4,3" jede dotyk po **interní I²C0**, která je ve výchozím stavu
vypnutá. Do `/boot/firmware/config.txt`:

```
dtparam=i2c_vc=on
dtoverlay=WS_4inchDSI480x800_Touch
```

> **Neověřeno.** `i2c_vc` je první podezřelý, kdyby po zapnutí dotyku zmizel
> zvuk — DAC jede po `i2c_arm` (GPIO2/GPIO3), takže ke kolizi by dojít nemělo,
> ale ověřit se to musí na reálném kusu.

---

## systemd

```ini
[Unit]
Description=ithaca-legacy panel
After=sound.target

[Service]
Type=simple
User=ithaca
SupplementaryGroups=audio video render input
Environment=SDL_VIDEODRIVER=kmsdrm
WorkingDirectory=/home/ithaca/ithaca-legacy
ExecStart=/home/ithaca/ithaca-legacy/build/ithaca-gui \
          --bank-dir /home/ithaca/banks \
          --wave-glow 0.4 --frame-divider 2 --frame-divider-idle 4
LimitRTPRIO=99
LimitMEMLOCK=infinity
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

`SupplementaryGroups` řeší přístup k DRM i evdev bez ladění `limits.conf`.
`LimitRTPRIO` a `LimitMEMLOCK` přepíšou per-process limity bez ohledu na
`/etc/security/limits.conf`.

---

## Když to nejede

| projev | co zkusit |
|---|---|
| `SDL_Init: no available video device` | Neběží KMSDRM. Zkontroluj, že Pi bootuje do konzole (ne do desktopu) a že build našel `libdrm`/`gbm` — `cmake -S . -B build 2>&1 \| grep -i kmsdrm`. |
| `Video driver: wayland` místo `kmsdrm` | Běží desktop. Buď přepnout boot do konzole, nebo pro test `--video-driver kmsdrm` (a čekat selhání, dokud desktop drží KMS). |
| okno se otevře, ale nereaguje na dotyk | Skupina `input`, a při buildu `libudev-dev`. |
| `--video-driver x11` hlásí „not available" | SDL bylo postavené bez X11. Doinstalovat `xorg-dev` a přeložit znovu. |
| trhá se to | `--frame-divider 2`; když nepomůže, `--wave-glow 0.4`. Nejdřív se ale podívej do logu na `late` a `present`. |
| teplota / throttling | `vcgencmd measure_temp; vcgencmd get_throttled` — a hlavně ověř, že se nezhoršil sloupec **DSP** na stránce PLAY. Grafika se nesmí zlepšit na úkor audia. |

---

## Křížové odkazy

| Kam | Proč |
|---|---|
| [3 · Raspberry Pi 5](03-raspberry-pi-5.md) | systém, DAC, RT oprávnění, CPU governor — vše, co není panel |
| [4 · Konfigurace](04-konfigurace.md) | `state.json`, kde ty přepínače žijí trvale |
| [H · GUI](../reference/H-gui.md) | jak je panel postavený uvnitř a proč tak |
