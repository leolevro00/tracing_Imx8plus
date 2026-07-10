# Registro test RT — GUI PEG/SDL su i.MX8M Plus

> **File vivo**: aggiornato a ogni esperimento sul target o modifica rilevante nel codice.
> Ultimo aggiornamento: **2026-07-10** (ristrutturazione tabella + titoli TEST)

---

## Documentazione di supporto (approfondimenti)

| File | Contenuto |
|------|-----------|
| `pipeline_peg_sdl_drm_rt.md` | Architettura pipeline, thread, ruolo LVGL |
| `analisi_metriche_gui_rt.md` | Significato di `calls`, `reqMBps`, `updateMs`, `effMBps`, `maxRectPx` |
| `interferenza_cpu_ddr_idle_vs_interazione.md` | Argomento CPU/DDR vs GPU per la tesi |
| `riduzione_framebuffer_esperimenti.md` | Opzioni A–E (formato texture, LockTexture, DRM diretto, …) |
| `osservazioni` / `test-perf` | Note raw su `perf stat`, `htop`, `gc/meminfo` |

---

## Obiettivo

> Ridurre **interferenza real-time** (jitter, traffico DDR, carico CPU) della GUI — **non** aumentare gli FPS.

Stack: **PEG** disegna su framebuffer software → `uploadDirtyRegion()` → SDL/KMSDRM (`SDL_UpdateTexture` + `SDL_RenderPresent`) → DRM/KMS. Path produzione: **Test 0 baseline**. LVGL inizializzato ma **non** gestisce widget/display.

---

## Configurazione di riferimento

| Parametro | Valore |
|-----------|--------|
| Target | i.MX8M Plus, Linux PREEMPT_RT, Yocto |
| Build | `PEG_USE_LVGL`, deploy in `/opt/Squeeze/` |
| Bpp | 16 (RGB565) |
| Risoluzione baseline misure `[RT]` | ~**960×640** effettivi su schermo (da log `maxRectPx`; `rtos.ini` repo era 1024×768) |
| Risoluzione attuale sul target | **1024×600 viewport** (`XRes=1024` `YRes=768`, `XView=1024` `YView=600`) su `/opt/Squeeze/rtos.ini` |
| Macro attive in `PegLib.pro` | `EMBEDDED_HMI_RT_STATS` ✅, `EMBEDDED_HMI_RT_DIAG` ✅ — **Test 0 baseline** (no `RT_NATIVE_TEXTURE`, no `RT_DRM_DIRECT`) |
| Macro disattivate | `EMBEDDED_HMI_RT_NATIVE_TEXTURE` (Test 5b rollback), `EMBEDDED_HMI_RT_DRM_DIRECT` (Test 6) |

---

### Buffer per risoluzione @ 16 bpp

| Risoluzione | Pixel | Buffer PEG (MiB) | Note |
|-------------|------:|-----------------:|------|
| 1024×768 | 786 432 | 1,50 | baseline `rtos.ini` originale |
| 1024×600 (viewport attuale target) | 614 400 | 1,17 | `XView=1024` `YView=600` su `/opt/Squeeze/rtos.ini` |
| **800×600** | 480 000 | **0,92** | Opzione 1 attuale (−39% vs 1024×768) |
| 640×480 | 307 200 | 0,59 | step più aggressivo (da provare) |

---

### Protocollo misura standard

Per ogni test, due scenari sul target:

| # | Scenario | Descrizione |
|---|----------|-------------|
| 1 | **Idle** | GUI avviata, nessuna interazione |
| 2 | **Drag grafico** | trascinamento touch sul grafico (scenario critico) |

**Metriche da raccogliere:**

- `[RT] uploadDirtyRegion`: `calls`, `reqMBps`, `updateMs`, `effMBps`, `maxRectPx`
- `htop`/`top`: %CPU e RES di `PegExec`
- Opzionale: `perf stat` (vedi `osservazioni`)

**Verifica deploy:**

```bash
strings libPegLib.so | grep '\[RT\]'
./PegExec 2>&1 | head   # log avvio: [RT] framebuffer PEG ...
```

#### Ordine di avvio PegExec / Lnk (critico per `nanosleep` max)

> **Scoperta 2026-07-10:** spike RT alti (100–150 µs) compaiono se **Lnk** (task RT / PerfMonitor) parte **subito dopo** PegExec. Se si attende **30–60 s** prima di avviare Lnk, i picchi sopra 100 µs **non si ripetono** (valori in linea con i test storici idle, es. ~73 µs su run lunghi senza touch).

**Causa probabile:** nei primi secondi PegExec è in **warm-up** (init SDL/KMSDRM, texture, primi present, thread PEG refresh/timer, possibile animazione GUI). Lnk e PegExec contendono su **CPU + bus DDR** → jitter sul `nanosleep` / `rtc_handler_us`. Al reboot automatico entrambi partono quasi insieme → scenario peggiore riprodotto senza accorgersene.

**Protocollo obbligatorio per misure RT comparabili:**

| Step | Azione |
|------|--------|
| 1 | Avviare **PegExec** (o attendere che sia su dopo reboot) |
| 2 | Attendere **≥ 30 s** (meglio **60 s**) — GUI visibile, init SDL/DRM completato |
| 3 | Avviare **Lnk** / PerfMonitor |
| 4 | Attendere altri **~10 s** prima di considerare valida la finestra di misura |
| 5 | Registrare scenario: **idle** (no touch) o **drag grafico** (touch) |

**Al reboot automatico** (PegExec + Lnk insieme): le prime migliaia di attivazioni possono mostrare max nanosleep in salita (es. 81 → 107 → 137) **anche senza touch** — non confondere con regressione di codice o `rtos.ini`.

**Verifica rapida:**

```bash
# A — solo warm-up PegExec, Lnk ritardato (atteso: max basso)
# Avvia PegExec, sleep 60, poi Lnk

# B — avvio ravvicinato (atteso: spike 100–150 µs nei primi secondi)
# Avvia PegExec, subito dopo Lnk
```

**Per test lunghi / script di avvio:** valutare `sleep 30` (o più) nello script che lancia Lnk dopo PegExec, almeno durante le campagne di misura RT.

**Nota:** questa trappola spiega confronti incoerenti tra commit identici, tra reboot e avvio manuale da terminale, e tra misure “ieri ok / oggi no” con lo stesso codice.

---

<a id="stato-test"></a>

## Stato test — tabella unica

> **Questa è la tabella di riferimento.** I dettagli di ogni voce sono nelle sezioni **TEST 0**, **TEST 4**, … **TEST 7** sotto ([cronologia](#cronologia-dettagliata)).

### Test eseguiti (cronologia)

| Test | Descrizione | Esito sintetico | Dettaglio |
|------|-------------|-----------------|-----------|
| **0** | Baseline SDL RGB565 + `EMBEDDED_HMI_RT_STATS` | 📌 **Riferimento** GUI + RT | [→ TEST 0](#test-0) |
| **4** | Riduzione risoluzione **800×600** (Opzione 1) | ✅ GUI −27…45% · RT **97 µs** (−20%) | [→ TEST 4](#test-4) |
| **5** | Diagnosi formato texture SDL (Opzione A) | ✅ Diagnosi mismatch RGB565 · nessun fix runtime | [→ TEST 5](#test-5) |
| **5b** | Texture ARGB8888 nativa + conversione esplicita | ✅ GUI +150% · ❌ RT **191 µs** → **rollback** | [→ TEST 5b](#test-5b) |
| **6** | DRM dumb buffer RGB565 (Opzione D POC) | ⚠️ GUI peggiore · RT 112 µs → **rollback** | [→ TEST 6](#test-6) |
| **DCC** | Framebuffer Compression / Prefetch (fase 0) | ❌ **Non applicabile** su i.MX8MP LCDIF | [→ TEST DCC](#test-dcc) |
| **7** | Pixel clock display (kernel / DRM) | ⏸️ **Sospeso** — solo via BSP Yocto (fase 0 ✅) | [→ TEST 7](#test-7) |

**Produzione attuale:** Test **0** baseline (`RT_NATIVE_TEXTURE` e `RT_DRM_DIRECT` disattivate).

### Prossimi test

| # | Test | Stato |
|---|------|-------|
| 1 | **Ottimizzazione immagini** — formati indicizzati/compressi; cache LVGL | ⬜ **prossimo** |
| 2 | **Font Unicode** — solo range caratteri necessari nei font embedded | ⬜ da fare |
| — | **Profondità di colore (Bpp)** | ✅ **già fatto** ([dettaglio Bpp](#bpp-gia-fatto)) |

### Archiviati / non prioritari

| Test | Descrizione | Stato |
|------|-------------|-------|
| **7** | Pixel clock display — abbassare timing LVDS | ⏸️ **Sospeso** — fase 0 OK; proseguimento solo con patch **BSP Yocto** ([TEST 7](#test-7)) |
| B | `SDL_LockTexture` streaming (Opzione B) | ⬜ non eseguito |
| C | Double buffering KMSDRM (già `SDL_VIDEO_DOUBLE_BUFFER=1`) | ⬜ nessun margine |

### Come leggere gli esiti (due assi indipendenti)

| Asse | Metriche | Cosa misura |
|------|----------|-------------|
| **GUI** | `effMBps`, `updateMs`, `reqMBps` | Upload interfaccia (`uploadDirtyRegion` → SDL) |
| **RT** | `rtc_handler_us`, `nanosleep` max | Latenza task RT; obiettivo **< 100 µs** |

> Un test può migliorare la **GUI** senza migliorare il **RT**, e viceversa.

---

<a id="cronologia-dettagliata"></a>

## Cronologia dettagliata

> Ogni test inizia con un titolo **TEST N** in evidenza. Usa la [tabella stato](#stato-test) per saltare alla sezione.

---

<a id="test-0"></a>

## TEST 0 — Baseline strumentazione (lug 2026)

**Stato:** 📌 Riferimento · **Macro:** solo `EMBEDDED_HMI_RT_STATS` · [← Tabella](#stato-test)

---

**Modifica:** macro `EMBEDDED_HMI_RT_STATS` in `PegLib/peglvglwindow.cpp` — log ogni ~1 s su stderr.

**Risultati `[RT]` (risoluzione ~960×640, 16 bpp):**

| Scenario | calls/s | reqMBps | updateMs/s | effMBps | maxRectPx | % schermo |
|----------|--------:|--------:|-----------:|--------:|----------:|----------:|
| Idle | 7–9 | 0,75–1,0 | 22–30 | 33–36 | ~66 912 | ~11% |
| Touch generico | 14–20 | 5–8 | 60–97 | 80–96 | fino a 614 400 | 100% |
| Popup avviso | 4–8 | 0,67–3,2 | 16–37 | 39–86 | 370 688 | ~60% |
| Drag grafico (stabile) | ~33 | ~24 | 208–220 | 112–117 | 485 051 | ~79% |

**Esempi log drag grafico:**

```text
[RT] uploadDirtyRegion: calls=33 req=24.74MB reqMBps=24.09 updateMs=216.366 effMBps=114.4 maxRectPx=485051
[RT] uploadDirtyRegion: calls=33 req=24.37MB reqMBps=23.75 updateMs=207.857 effMBps=117.3 maxRectPx=485051
```

**CPU / memoria (`htop`):**

| Condizione | %CPU PegExec | RES |
|------------|-------------:|----:|
| Idle | ~4–5% | ~196 MB (stabile) |
| Interazione | ~17–18% | ~196 MB (stabile) |

**`perf` (qualitativo, vedi `osservazioni`):** durante touch aumentano `mem_access`, `l2d_cache_wb`, `bus_access`, `imx8_ddr0/write-accesses`. GPU `gc/meminfo` stabile (~8–9 MB SYSTEM Used). `read-accesses` DDR non affidabile (sempre 0).

> **Conclusione:** collo di bottiglia principale = **copia CPU→texture** (`SDL_UpdateTexture`), non saturazione GPU. Scenario critico = **drag grafico** (~21% core in upload, ~24 MB/s).

**Pipeline e buffer (Test 0):** SDL video + KMSDRM + renderer OpenGL ES2. Il framebuffer dove PEG disegna è **sempre uno** (`g_pyBitmap`, allocato da `createSurface()`). Sopra di esso il path baseline aggiunge texture SDL, back buffer GLES e doppio buffering scanout gestito da SDL. Dettaglio conteggio buffer e confronto con il Test 6 → sezione [Buffer: Test 6 vs Test 0](#buffer-test-6-utilizza-meno-buffer-del-test-0) sotto.

---

<a id="test-4"></a>

## TEST 4 — Riduzione risoluzione 800×600, Opzione 1 (2026-07-09)

**Stato:** ✅ Successo GUI + RT · **Macro:** `RT_STATS` · [← Tabella](#stato-test)

---

**Modifica:**

- `Files/avn8mp/rtos.ini`: `XRes=800`, `YRes=600` (senza `XView`/`YView` attivi)
- `EMBEDDED_HMI_RT_STATS` per misure a confronto
- Log avvio risoluzione in `peg_run.cpp`

**Config sul target:**

```ini
XRes=800
YRes=600
Bpp=16
```

**Framebuffer:** 800×600 @ 16 bpp = 0,92 MiB

---

#### A) Metriche GUI `[RT] uploadDirtyRegion` — scroll grafico

| Fase | calls/s | reqMBps | updateMs/s | effMBps | maxRectPx | % schermo (800×600) |
|------|--------:|--------:|-----------:|--------:|----------:|--------------------:|
| Idle / leggero | 4–5 | ~0,45–0,59 | ~11–14 | ~40–50 | ~76 050–82 719 | ~16% |
| **Scroll grafico** | **27–33** | **~10–14** | **~115–152** | **~89–94** | **368 347** | **~77%** |

**Esempi log scroll (800×600):**

```text
[RT] uploadDirtyRegion: calls=33 req=14.01MB reqMBps=13.63 updateMs=152.147 effMBps=92.1 maxRectPx=368347
[RT] uploadDirtyRegion: calls=30 req=12.45MB reqMBps=12.14 updateMs=135.220 effMBps=92.0 maxRectPx=368347
[RT] uploadDirtyRegion: calls=27 req=10.23MB reqMBps=10.01 updateMs=114.580 effMBps=89.3 maxRectPx=368347
```

---

#### B) Confronto scroll grafico — baseline 1024×600 viewport vs 800×600

| Metrica | Baseline (1024×600 viewport, Test 0) | **800×600** | Variazione |
|---------|---------------------------------------|-------------|------------|
| `reqMBps` (scroll) | ~24 MB/s | ~10–14 MB/s | **≈ −42% … −58%** |
| `updateMs` (scroll) | ~208–220 ms/s | ~115–152 ms/s | **≈ −27% … −45%** |
| `maxRectPx` | 485 051 (~79% di 614 400) | 368 347 (~77% di 480 000) | area assoluta **−24%** |
| `calls/s` | ~33 | ~27–33 | simile |

> La % di schermo sporco resta ~77–79%, ma con meno pixel assoluti il traffico e il tempo in `SDL_UpdateTexture()` scendono in modo netto.

---

#### C) `top` / `ps` — confronto stesso protocollo (2026-07-09)

| Condizione | Config | %CPU `PegExec` | %CPU `Lnk` | RSS `PegExec` |
|------------|--------|---------------:|-----------:|--------------:|
| Interfaccia attiva, no touch | 1024×600 viewport | **7,3%** | 20,1% | 195 744 KB |
| Scroll grafico | 1024×600 viewport | **30,4%** | 21,8% | 196 256 KB |
| Interfaccia attiva, no touch | **800×600** | **4,0%** | 19,2% | 195 784 KB |
| Scroll grafico | **800×600** | **15,2%** | 20,1% | 195 784 KB |

| Δ scroll − idle (`PegExec`) | 1024×600 viewport | 800×600 |
|-----------------------------|--------------------:|--------:|
| Incremento CPU | **+23,1 pp** (7,3→30,4%) | **+11,2 pp** (4,0→15,2%) |

**`ps` (800×600):**

```text
PegExec  VSZ=1777300  RSS=195784  %MEM=29.8
Lnk      VSZ=415484   RSS=131836  %MEM=20.1
```

**`ps` (baseline 1024×600 viewport):**

```text
PegExec  VSZ=1778356  RSS=195744  %MEM=29.8
Lnk      VSZ=415484   RSS=131952  %MEM=20.1
```

**Interpretazione:**

- La **RAM residente** di `PegExec` non cambia con la risoluzione (~196 MiB): il guadagno RT viene da meno **banda/tempo di copia** a runtime.
- `Lnk` resta ~19–20% CPU sia a riposo sia in scroll → carico **non legato alla GUI**.
- Il salto idle→scroll su `PegExec` si dimezza circa a 800×600 (+11 pp vs +23 pp).

---

#### D) Metriche RT task — worst case durante scroll GUI

**800×600** (`nanosleep` + `rtc_handler_us` su CPU3):

```text
nanosleep: min=12, max=97  (ultime ~44000 attivazioni)
[WORST rtc_handler_us] iter=27071  rtc_handler_us=97 us
  L2 miss=16.45%  bus_access=5543  bus_cycles=222208  CPI=3.07
```

**Baseline 1024×600 viewport** (stesso scenario scroll):

```text
[WORST rtc_handler_us] iter=10144  rtc_handler_us=122 us
  L2 miss=21.21%  bus_access=24755  bus_cycles=853688  CPI=2.39
```

| Metrica RT worst case | Baseline 1024×600 | 800×600 | Variazione |
|-----------------------|------------------:|--------:|-----------:|
| `rtc_handler_us` max | **122 µs** | **97 µs** | **−20%** |
| `bus_access` (iterazione worst) | 24 755 | 5 543 | **−78%** |
| `bus_cycles` | 853 688 | 222 208 | **−74%** |
| `cpu_cycles` | 1 702 797 | 439 805 | **−74%** |
| L2 cache miss % | 21,2% | 16,5% | −4,7 pp |

> **Conclusione:** l'Opzione 1 (riduzione risoluzione via `rtos.ini`) è la prima modifica con **beneficio su entrambi gli assi**: pipeline GUI (`reqMBps`, `updateMs`, `effMBps`) **e** task RTC (`rtc_handler_us` 122→97 µs, traffico bus). Il ritardo percepito durante lo scroll del grafico risulta migliore.

**Esito sintetico:**

| Asse | Esito Test 4 |
|------|--------------|
| **GUI** (`effMBps` / `updateMs`) | ✅ Miglioramento misurabile |
| **RT** (`rtc_handler_us`) | ✅ Miglioramento misurabile (97 µs, obiettivo <100 quasi raggiunto) |

**Limiti / note:**

- L'UI è progettata per risoluzioni più alte: verificare leggibilità e layout a 800×600 prima di produzione.
- Per cambiare risoluzione su embedded: `XRes`/`YRes` devono coincidere con il framebuffer reale; non usare `XView`/`YView` diversi da `XRes`/`YRes`.
- Prossimo step opzionale: Opzione A (formato texture SDL) per ulteriore riduzione.

**Stato:** config **800×600** validata sul target.

---

---

<a id="test-5"></a>

## TEST 5 — Diagnosi pipeline SDL / formato texture (Opzione A) (2026-07-09)

**Stato:** ✅ Completato (solo diagnosi) · **Macro:** `RT_DIAG` · [← Tabella](#stato-test)

---

| Campo | Valore |
|-------|--------|
| **Obiettivo** | verificare mismatch formato texture / renderer e configurazione buffer KMSDRM. |
| **Modifica** | macro `EMBEDDED_HMI_RT_DIAG` + `rtDiagLogSdlPipeline()` in `peglvglwindow.cpp` (solo log, nessun cambio pipeline). |

**Log raccolti sul target (2026-07-09):**

```text
[RT] diag: SDL video driver=KMSDRM
[RT] diag: env SDL_VIDEO_DOUBLE_BUFFER=1
[RT] diag: env SDL_KMSDRM_REQUIRE_DRM_MASTER=0
[RT] diag: window pixel format=SDL_PIXELFORMAT_ARGB8888 (0x16362004)
[RT] diag: PEG framebuffer bpp=16 requested_texture=SDL_PIXELFORMAT_RGB565 (0x15151002)
[RT] diag: renderer name=opengles2 flags=0xE max=8192x8192 native_formats=9
[RT] diag:   native_format[0]=SDL_PIXELFORMAT_ABGR8888 (0x16762004)
[RT] diag:   native_format[1]=SDL_PIXELFORMAT_ARGB8888 (0x16362004)
[RT] diag:   native_format[2]=SDL_PIXELFORMAT_RGB888 (0x16161804)
[RT] diag:   native_format[3]=SDL_PIXELFORMAT_BGR888 (0x16561804)
[RT] diag:   native_format[4..8]=YV12, IYUV, NV12, NV21, EXTERNAL_OES
[RT] diag: texture actual=SDL_PIXELFORMAT_RGB565 (0x15151002) access=STREAMING size=1024x600
[RT] diag: requested format native for renderer=NO
[RT] diag: ATTENZIONE possibile SDL_ConvertPixels nascosta in SDL_UpdateTexture
[RT] diag: fine report pipeline SDL (una tantum all'avvio)
```

**Config `rtos.ini` al momento del test:** `XRes=1024` `YRes=768` `XView=1024` `YView=600` (framebuffer 1024×600).

---

#### Tabella risultati

| Verifica | Risultato | Note |
|----------|-----------|------|
| Video driver | **KMSDRM** | OK |
| `SDL_VIDEO_DOUBLE_BUFFER` | **1** | double buffering scanout (non triple) |
| `SDL_KMSDRM_REQUIRE_DRM_MASTER` | **0** | come in codice |
| Finestra SDL | **ARGB8888** | 32 bpp lato display/window |
| Framebuffer PEG | **16 bpp RGB565** | invariato |
| Texture richiesta / effettiva | **RGB565** / **RGB565** | `SDL_CreateTexture` accetta il formato richiesto |
| Renderer | **opengles2** | flags `0xE` (accelerated + vsync + target) |
| **RGB565 tra formati nativi** | **NO** | nativi: ABGR/ARGB/RGB/BGR888 + YUV |
| Conversione nascosta probabile | **SÌ** | SDL avvisa esplicitamente nel log |

---

#### Conclusione

> **Confermato:** su i.MX8MP con renderer **OpenGL ES2**, la texture **RGB565 non è un formato nativo**. `SDL_UpdateTexture()` da framebuffer PEG 16 bpp probabilmente esegue **`SDL_ConvertPixels` nascosta** verso un formato GLES (es. ARGB8888) prima dell'upload GPU.

Questo spiega perché:

- `effMBps` in scroll resta ~90–115 MB/s (lento per una memcpy pura)
- ridurre risoluzione (Test 4) aiuta ma non elimina il costo di conversione
- **Opzione B (`LockTexture`) da sola** probabilmente **non basta** (stesso mismatch formato)

#### Verifica empirica: al plane DRM arrivano **32 bpp** (non 16) — 2026-07-10

La diagnosi SDL (finestra ARGB8888, texture RGB565 non nativa su GLES) indica che il path GPU compone a 32 bpp, ma fino a questa data non era misurato **cosa** finisce sul framebuffer di scanout verso il pannello.

**Domanda:** con `Bpp=16` in `rtos.ini`, SDL passa al display controller **16 o 32 bpp**?

**Risposta sul target avn8mp:** **32 bpp** sul framebuffer di scanout attivo (`fb0`), mentre PEG resta a 16 bpp in RAM.

**Comandi** (con **PegExec in esecuzione** e GUI visibile; con PegExec fermo `modetest` mostra `fb=0` e i path sysfs sotto `card1` possono essere vuoti):

```bash
# Bpp del framebuffer Linux esposto da imx-drm (scanout verso LVDS)
cat /sys/class/graphics/fb0/bits_per_pixel
# → 32

# Plane DRM: formati supportati dal hardware (capacità, non formato usato da SDL)
modetest -M imx-drm -p 2>/dev/null | grep formats
# → formats: XR24 AR24 RG16 XB24 AB24 AR15 XR15
#    XR24/AR24 = 32 bpp   |   RG16 = RGB565 = 16 bpp (nativo sul plane, ma non usato da SDL)

# Connector attivo
cat /sys/class/drm/card1-LVDS-1/status
# → connected
```

**Interpretazione:**

```text
PEG RGB565 (16 bpp, RAM)  →  SDL/GLES (conversione + compositing)  →  fb0 scanout (32 bpp)  →  LCDIF/LVDS
         ~1,17 MiB @ 1024×600                              ~2,34 MiB @ 1024×600
```

| Pezzo | Bpp | Evidenza |
|-------|----:|----------|
| Framebuffer PEG | **16** | `Bpp=16` in `rtos.ini`, log `[RT] rtos.ini … @ 16 bpp` |
| Texture SDL (Test 0) | 16 richiesta, conversione nascosta probabile | `[RT] diag` Test 5 |
| **Scanout display (`fb0`)** | **32** | `cat …/fb0/bits_per_pixel` → **32** |
| Plane imx-drm (capacità) | 16 **e** 32 | `RG16` + `XR24` in `modetest` — hardware ok con RGB565, ma SDL non lo usa |

> **Chicca:** il plane DRM **sa** fare RG16 (RGB565), ma con **SDL + OpenGL ES2 + KMSDRM** il buffer che legge il LCDIF è **`fb0` a 32 bpp** — circa **il doppio** dei byte sul bus display rispetto a un path RGB565 diretto (es. Test 6 dumb buffer). Il mismatch non è solo “texture vs PEG”, è **PEG 16 bpp → scanout 32 bpp**.

**Nota:** `find /sys/class/drm/card1 -name bits_per_pixel` può restare vuoto con SDL/KMSDRM; usare **`/sys/class/graphics/fb0/bits_per_pixel`** come riferimento pratico.

---

#### Prossimo esperimento consigliato (Test 5b)

| Opzione | Descrizione | Sforzo |
|---------|-------------|--------|
| **5b-1** | Texture **ARGB8888** nativa + conversione esplicita RGB565→ARGB8888 in `uploadDirtyRegion` | medio — misurare se più veloce della conversione nascosta SDL |
| **5b-2** | Texture **RGB888** nativa + conversione 16→24 bpp | medio — 50% più byte ma formato nativo |
| Opzione D | DRM dumb buffer RGB565 diretto | alto — bypass GPU/SDL texture |

**Opzione C (triple→double):** già a **double** (`SDL_VIDEO_DOUBLE_BUFFER=1`) — nessun margine su questo fronte.

**Stato codice:** diagnosi attiva; **nessuna modifica funzionale** alla pipeline oltre al log.

**Esito sintetico:**

| Asse | Esito Test 5 |
|------|--------------|
| **GUI** / **RT** | Solo diagnosi — spiega perché `effMBps` baseline era basso; nessun miglioramento runtime |

---

---

<a id="test-5b"></a>

## TEST 5b — Texture ARGB8888 nativa + conversione esplicita (2026-07-09)

**Stato:** ✅ GUI / ❌ RT → rollback · **Macro:** `RT_NATIVE_TEXTURE` (ora disattivata) · [← Tabella](#stato-test)

---

**Modifica:** macro `EMBEDDED_HMI_RT_NATIVE_TEXTURE` in `PegLib.pro`. Con framebuffer PEG 16 bpp (RGB565):

- `SDL_CreateTexture(..., ARGB8888, STREAMING)` invece di RGB565
- in `uploadDirtyRegion()`: `SDL_ConvertPixels(RGB565 → ARGB8888)` su buffer staging riusabile, poi `SDL_UpdateTexture` con pitch `rect.w * 4`
- metriche `[RT]`: `reqMBps` conta byte **ARGB8888** (4 bpp) per confronto onesto del costo upload texture

| Campo | Valore |
|-------|--------|
| **Build/macro** | `EMBEDDED_HMI_RT_STATS` + `EMBEDDED_HMI_RT_DIAG` + `EMBEDDED_HMI_RT_NATIVE_TEXTURE` |
| **Config target** | produzione **1024×600 viewport** (`XRes=1024` `YRes=768`, `XView=1024` `YView=600`) — stesso scenario del Test 0 baseline |
| **Scenario** | idle \| drag grafico (scroll) — protocollo standard |

| Scenario | calls/s | reqMBps | updateMs/s | effMBps | maxRectPx | %CPU | RES |
|----------|--------:|--------:|-----------:|--------:|----------:|-----:|----:|
| idle | 4–5 | 2,9–3,7 | 11–15 | 256–270 | 223 040 | _n/d_ | _n/d_ |
| drag grafico | **33** | **~47,5** | **165–170** | **287–295** | **485 051** | _n/d_ | _n/d_ |

**Log esempio idle:**

```text
[RT] uploadDirtyRegion: calls=4 req=2.91MB reqMBps=2.90 updateMs=11.050 effMBps=263.4 maxRectPx=223040
[RT] uploadDirtyRegion: calls=5 req=3.76MB reqMBps=3.71 updateMs=14.167 effMBps=265.5 maxRectPx=223040
```

**Log esempio drag grafico:**

```text
[RT] uploadDirtyRegion: calls=33 req=48.74MB reqMBps=47.49 updateMs=165.666 effMBps=294.2 maxRectPx=485051
[RT] uploadDirtyRegion: calls=33 req=49.17MB reqMBps=47.85 updateMs=169.560 effMBps=287.5 maxRectPx=485051
```

**Confronto vs Test 0 baseline** (stesso `maxRectPx=485051`, `calls=33`, viewport ~1024×600):

| Metrica | Test 0 (RGB565 texture) | Test 5b (ARGB8888 nativo) | Variazione |
|---------|------------------------:|--------------------------:|-----------:|
| `reqMBps` | ~24 | ~47,5 | +98% (atteso: 4 bpp vs 2 bpp) |
| `updateMs/s` | ~208–220 | **~165–170** | **−22…−26%** |
| `effMBps` | ~112–117 | **~287–295** | **+150%** |
| Banda pixel sorgente equivalente | ~24 MB/s RGB565 | ~23,8 MB/s RGB565 (`47,5/2`) | ≈ uguale |

**Metriche RT task — worst case durante scroll** (CPU3):

**Misura A** (2026-07-09, prima sessione):

```text
[WORST rtc_handler_us] iter=6365  rtc_handler_us=123 us
  L2 miss=26.99%  bus_access=8803  bus_cycles=361523  CPI=5.14
```

**Misura B** (2026-07-10, ripetizione Test 5b stessa build — scroll grafico):

```text
[WORST rtc_handler_us] iter=27974  rtc_handler_us=191 us
  L2 miss=19.35%  bus_access=24288  bus_cycles=935307  CPI=2.58
```

| Metrica RT worst case | Test 0 baseline | Test 5b (misura A) | Test 5b (misura B) | Note |
|-----------------------|------------------:|-------------------:|-------------------:|------|
| `rtc_handler_us` max | **122 µs** | 123 µs | **191 µs** | Misura B: **+57% vs baseline**, **+55% vs misura A** |
| `bus_access` | 24 755 | **8 803** | **24 288** | Misura A: −64%. Misura B: ≈ baseline (il calo bus non è stabile) |
| `bus_cycles` | 853 688 | 361 523 | 935 307 | Misura B: traffico bus simile al baseline |

> **Attenzione:** il miglioramento `bus_access` della misura A **non** si è ripetuto nella misura B. Il worst-case `rtc_handler_us` nella misura B (**191 µs**) è **peggio del baseline** (122 µs) e **peggio del Test 6** (112 µs). Possibile variabilità run-to-run, durata scroll, carico sistema o condizione di misura — **da ripetere** con protocollo identico.

**Avvio build 5b (verifica deploy):**

```text
[RT] native_texture: Test 5b ARGB8888 + SDL_ConvertPixels esplicito da RGB565
[RT] diag: renderer name=opengles2 ... texture actual=SDL_PIXELFORMAT_ARGB8888 ... 1024x600
[RT] diag: requested format native for renderer=YES
```

> **Conclusione:** ✅ **Test 5b positivo sull'asse GUI. ❌ Test 5b negativo o instabile sull'asse RT.**
>
> - ✅ **GUI (trasferimento dati):** `effMBps` +150%, `updateMs/s` −22% — miglioramento **misurabile** e **ripetibile** sulla velocità di upload.
> - ❌ **RT (ritardo nanosleep):** worst case **191 µs** (misura B) — **peggioramento netto** vs baseline 122 µs e vs misura A 123 µs. Obiettivo < 100 µs **non raggiunto**; il 5b **non** può considerarsi neutro sul RT.
> - ⚠️ `bus_access` −64% (misura A) **non** confermato in misura B (24 288 ≈ baseline): il traffico bus da solo non spiega il ritardo RT.

**Esito sintetico:**

| Asse | Esito Test 5b |
|------|---------------|
| **GUI** (`effMBps` / `updateMs`) | ✅ Miglioramento misurabile |
| **RT** (`rtc_handler_us`) | ❌ **Peggioramento** (worst 191 µs; misura A 123 µs ≈ flat) |

**Raccomandazione:** tenere `EMBEDDED_HMI_RT_NATIVE_TEXTURE` per il **beneficio GUI** (`effMBps`, `updateMs`), ma **non** considerare il 5b una soluzione RT — serve un'altra leva (isolamento CPU, riduzione dirty, risoluzione, ecc.) per avvicinarsi a < 100 µs. Ripetere misura RT con protocollo fisso (durata scroll, stesso scenario) per quantificare la variabilità.

**Stato codice:** macro **attiva** in `PegLib.pro`.

---

---

<a id="test-6"></a>

## TEST 6 — Opzione D: DRM dumb buffer RGB565 diretto (2026-07-09)

**Stato:** ⚠️ Misurato → rollback · **Macro:** `RT_DRM_DIRECT` (disattivata) · [← Tabella](#stato-test)

---

| Campo | Valore |
|-------|--------|
| **Obiettivo** | bypassare SDL texture + GPU; una sola copia RGB565 PEG → scanout. Target: `rtc_handler_us` **< 100 µs** a 1024×600 produzione. |
| **Modifica** | macro `EMBEDDED_HMI_RT_DRM_DIRECT` in `PegLib.pro` (disattiva `EMBEDDED_HMI_RT_NATIVE_TEXTURE`). |

**File nuovi:**

- `pegdrmoutput.cpp` — probe KMS su tutti i `/dev/dri/card*`, 2 dumb buffer RGB565, `drmModeSetCrtc`, `drmModePageFlip`
- `pegdrm_evdev.cpp` — touch via `/dev/input/event*` (SDL solo `EVENTS`+`TIMER`)

**Fix post-POC (stessa build di misura):**

- Selezione device DRM: `drmModeGetResources` su ogni card (card0 Vivante → card1 `imx-drm`)
- Touch evdev: edge detection press/release su `SYN_REPORT` (bug: solo motion, no click PEG)

**Pipeline:**

```text
PEG RGB565 → blitDirtyRegion (memcpy) → dumb buffer back → pageFlip → display
Touch → evdev → PEG mouse mapping
```

#### Buffer: Test 6 utilizza meno buffer del Test 0

> Confronto architetturale tra **Test 0** (baseline SDL RGB565, senza `EMBEDDED_HMI_RT_NATIVE_TEXTURE`) e **Test 6** (`EMBEDDED_HMI_RT_DRM_DIRECT`). Riferimento codice: repo `pegenstein`, cartella `PegLib/`.

**Regola comune a entrambi i test**

Il framebuffer software dove PEG disegna **non si può eliminare**: è un solo buffer in RAM, RGB565, condiviso tra i due path.

- Allocazione: `peg_run.cpp` → `createSurface()` → `g_pyBitmap` / `PEG_SetFrameBuffer()`
- Implementazione: `peglvglwindow.cpp` → `m_framebuffer` (`calloc` full screen)

**Test 0 — buffer nel path SDL/KMSDRM (baseline)**

| # | Buffer | Variabile / API | Dove |
|---|--------|-----------------|------|
| 1 | Framebuffer PEG | `g_pyBitmap` → `m_framebuffer` | RAM CPU |
| 2 | Texture SDL streaming | `m_texture` (`SDL_CreateTexture`, `SDL_PIXELFORMAT_RGB565`) | memoria GPU/GEM |
| 3 | Back buffer renderer GLES | `m_renderer` (`SDL_CreateRenderer`, driver `opengles2`) | GPU |
| 4–5 | Doppio buffering scanout display | `SDL_VIDEO_DOUBLE_BUFFER=1` in `configureEmbeddedSdlVideo()` | KMSDRM (gestito da SDL) |

**Totale Test 0: ~5 buffer** (1 PEG + 1 texture + 1 back buffer GPU + 2 scanout).

Catena dati Test 0:

```text
g_pyBitmap (RGB565)
  → SDL_UpdateTexture(m_texture)
  → SDL_RenderCopy(m_renderer, m_texture, …)
  → SDL_RenderPresent()
  → 2 FB scanout KMSDRM
  → display
```

Citazioni codice (Test 0):

```cpp
// peglvglwindow.h — oggetti SDL del path baseline
SDL_Window *m_window;
SDL_Renderer *m_renderer;
SDL_Texture *m_texture;
unsigned char *m_framebuffer;   // = g_pyBitmap

// peglvglwindow.cpp — texture RGB565 (Test 0: senza EMBEDDED_HMI_RT_NATIVE_TEXTURE)
m_texture = SDL_CreateTexture(m_renderer, pixelFormat, SDL_TEXTUREACCESS_STREAMING, m_width, m_height);

// peglvglwindow.cpp — doppio buffering scanout
SDL_setenv("SDL_VIDEO_DOUBLE_BUFFER", "1", 0);
SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
```

> **Nota (Test 5):** con RGB565 la texture SDL può innescare una `SDL_ConvertPixels()` nascosta verso un formato nativo GLES2 — buffer staging aggiuntivo **interno a SDL**, non visibile nel nostro codice ma presente nel path baseline.

**Test 6 — buffer nel path DRM dumb diretto**

| # | Buffer | Variabile / API | Dove |
|---|--------|-----------------|------|
| 1 | Framebuffer PEG | `g_pyBitmap` → `m_framebuffer` | RAM CPU (identico al Test 0) |
| 2 | Dumb buffer scanout *front* | `m_buffers[0]` (`drm_mode_create_dumb`) | memoria DRM/scanout |
| 3 | Dumb buffer scanout *back* | `m_buffers[1]` | memoria DRM/scanout |

**Totale Test 6: 3 buffer** (1 PEG + 2 scanout).

Catena dati Test 6:

```text
g_pyBitmap (RGB565)
  → blitDirtyRegion() — memcpy CPU
  → m_buffers[m_backIndex] (dumb RGB565)
  → drmModePageFlip()
  → display
```

Citazioni codice (Test 6):

```cpp
// peglvglwindow.cpp — niente SDL video/texture/GPU
if (SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0)  // solo eventi + timer
m_drmOutput = new PegDrmOutput();
m_drmOutput->initialize(w, h, bits);

// pegdrmoutput.h — esattamente 2 dumb buffer per il pageflip
} m_buffers[2];   // double buffering scanout

// pegdrmoutput.cpp — allocazione dei due buffer
if (!createBuffer(m_buffers[0]) || !createBuffer(m_buffers[1]))

// peglvglwindow.cpp — upload senza texture SDL
m_drmOutput->blitDirtyRegion(m_framebuffer, pitch, left, top, right, bottom);
```

**Riepilogo confronto Test 6 vs Test 0**

| Aspetto | Test 0 | Test 6 |
|---------|--------|--------|
| **Numero totale di buffer** | **~5** | **3** |
| Framebuffer PEG | 1 | 1 (invariato) |
| Buffer scanout sul display | 2 (via SDL/KMSDRM) | 2 (via DRM dumb) |
| Texture SDL | 1 | **0 — eliminata** |
| Back buffer GPU (GLES) | 1 | **0 — eliminato** |
| Lavoro GPU (`RenderCopy`, texture upload) | sì | **no** (copia su CPU + pageflip DRM) |
| RSS `PegExec` misurato | ~196 MiB | ~180 MiB (−8%) |

```text
Test 0:  [PEG FB] → [texture SDL] → [back buffer GLES] → [scanout ×2] → display
              1            2                 3                  4–5

Test 6:  [PEG FB] ──memcpy──→ [dumb back] ⇄ pageflip ⇄ [dumb front] → display
              1                      2                    3
```

**Cosa significa in pratica**

- **Sì:** il Test 6 utilizza **meno buffer** del Test 0 — rimuove la texture SDL e il back buffer del renderer OpenGL ES2 (da ~5 a 3 buffer contati nel codice).
- **No:** non elimina il framebuffer PEG né il doppio buffering sul display (servono sempre 2 buffer scanout per il pageflip senza tearing).
- **Trade-off misurato:** meno buffer e meno lavoro GPU, ma il carico si sposta su **CPU** (`memcpy` verso dumb buffer + attesa `pageFlip`) — per questo `updateMs`, `effMBps` e `%CPU` scroll risultano **peggiori** del Test 0 nonostante meno buffer (vedi tabella risultati sotto).

| Campo | Valore |
|-------|--------|
| **Build/macro** | `EMBEDDED_HMI_RT_STATS` + `EMBEDDED_HMI_RT_DRM_DIRECT` (no `RT_NATIVE_TEXTURE`, no `RT_DIAG` SDL) |
| **Config target** | produzione `XRes=1024 YRes=768` + `XView=1024 YView=600`, `Bpp=16` |
| **Scenario** | idle (nessun touch) \| scroll grafico (drag touch) — protocollo standard |

| Scenario | calls/s | reqMBps | updateMs/s | effMBps | maxRectPx | %CPU PegExec | RES PegExec | rtc_handler_us |
|----------|--------:|--------:|-----------:|--------:|----------:|-------------:|------------:|---------------:|
| idle (no touch) | **29–33** | **~18–24** | **247–282** | **82–90** | **485 051** | **7,9%** | **180 MB** | _n/d_ |
| scroll grafico | **4–6** | **~1,6–2,4** | **30–46** | **50–58** | **212 544** (max 301 644) | **38,4%** | **180 MB** | **112 µs** worst |

**Log esempio idle (grafico animato, nessun touch):**

```text
[RT] uploadDirtyRegion: calls=33 req=24.58MB reqMBps=23.96 updateMs=282.332 effMBps=87.0 maxRectPx=485051
[RT] uploadDirtyRegion: calls=33 req=24.37MB reqMBps=23.75 updateMs=270.092 effMBps=90.2 maxRectPx=485051
```

**Log esempio scroll grafico:**

```text
[RT] uploadDirtyRegion: calls=6 req=2.41MB reqMBps=1.85 updateMs=45.602 effMBps=52.9 maxRectPx=212544
[RT] uploadDirtyRegion: calls=4 req=1.60MB reqMBps=1.58 updateMs=30.354 effMBps=52.8 maxRectPx=212544
[RT] uploadDirtyRegion: calls=4 req=1.79MB reqMBps=1.77 updateMs=31.093 effMBps=57.6 maxRectPx=301644
```

**Metriche RT task — worst case durante scroll** (CPU3):

```text
[WORST rtc_handler_us] iter=1357  rtc_handler_us=112 us
  L2 miss=26.87%  bus_access=8771  bus_cycles=298112  CPI=4.24
```

---

#### CPU / memoria — `top` (2026-07-09)

| Condizione | %CPU `PegExec` | %CPU `Lnk` | RES `PegExec` | Stato `PegExec` |
|------------|---------------:|-----------:|--------------:|:----------------|
| Interfaccia attiva, no touch | **7,9%** | **19,5%** | **179 956 KB** (~27,5% MEM) | S (sleep) |
| Scroll grafico (interazione) | **38,4%** | **20,9%** | **179 956 KB** | R (running) |

| Δ scroll − idle (`PegExec`) | Test 6 (DRM) | Test 4 baseline 1024×600 (SDL) |
|-----------------------------|-------------:|-------------------------------:|
| Incremento CPU | **+30,5 pp** (7,9→38,4%) | +23,1 pp (7,3→30,4%) |

**`top` (estratto Test 6):**

```text
# idle — no touch
  1630 root  19.5  20.1  415484 131960  S  Lnk
  1580 root   7.9  27.5 1357288 179956  S  PegExec

# scroll grafico
  1580 root  38.4  27.5 1357288 179956  R  PegExec
  1630 root  20.9  20.1  415484 131960  S  Lnk
```

**Interpretazione CPU/RAM:**

- `PegExec` RES **~180 MiB** vs **~196 MiB** del path SDL (Test 4): −8% RAM — atteso senza contesto SDL video/renderer/GPU.
- A riposo CPU simile al baseline (+0,6 pp su `PegExec`); `Lnk` invariato ~19–21%.
- In **scroll** `PegExec` sale a **38,4%** (+30,5 pp) — **peggio del baseline SDL** (+23 pp) e del 5b atteso (~30% a 1024×600). Coerente con `updateMs` alto e memcpy CPU-bound verso dumb buffer.

---

**Confronto scroll / area grande** (`maxRectPx≈485051`, stesso viewport 1024×600):

| Metrica | Test 0 (SDL RGB565) | Test 5b (ARGB nativo) | **Test 6 (DRM dumb)** | Test 6 vs 5b |
|---------|--------------------:|----------------------:|----------------------:|-------------:|
| `reqMBps` | ~24 | ~47,5 | **~24** | −50% (atteso: RGB565 vs ARGB) |
| `updateMs/s` | ~208–220 | **~165–170** | **~247–282** (idle anim.) | **+50…+70%** peggiore |
| `effMBps` | ~112–117 | **~287–295** | **~82–90** | **−70%** |
| `rtc_handler_us` max | 122 µs | 123 µs | **112 µs** | **−9 µs** |
| `bus_access` | 24 755 | **8 803** | **8 771** | ≈ uguale |

**Confronto scroll attivo** (`maxRectPx≈212k–302k`, area dirty più piccola durante pan):

| Metrica | Test 5b idle (223k px) | **Test 6 scroll** | Note |
|---------|----------------------:|------------------:|------|
| `updateMs/s` | 11–15 | **30–46** | D ancora più lento a parità di area simile |
| `effMBps` | 256–270 | **50–58** | memcpy→dumb buffer + pageflip vs GPU texture |

**Osservazioni:**

- ✅ **Unico miglioramento RT misurato su `rtc_handler_us`:** 112 µs vs 122–123 µs (−8…−9%), ma **obiettivo < 100 µs non raggiunto** (mancano ~12 µs).
- ✅ `bus_access` in linea con Test 5b (~8,8k), molto sotto baseline Test 0 (~24,8k).
- ❌ **Costo GUI alto:** in idle con grafico animato (`maxRectPx=485051`) `updateMs` ~270–282 ms — **peggio del Test 0** (~220 ms) e molto peggio del 5b (~170 ms scroll).
- ❌ **CPU scroll:** `PegExec` **38,4%** in interazione vs **30,4%** baseline SDL a parità viewport — il bypass GPU non alleggerisce la CPU, anzi la carica di più.
- Durante lo **scroll** l'area dirty si riduce (`maxRectPx≈212k`) e `updateMs` scende a ~30–46 ms — pattern **invertito** rispetto a 5b (dove lo scroll era lo scenario peggiore); probabile diverso partizionamento dirty PEG + attesa `pageFlip` DRM.
- Il touch funziona dopo fix evdev (`m_reportedDown` su `SYN_REPORT`).

> **Conclusione:** ⚠️ **Test 6 misto — assi opposti.**
>
> - ❌ **GUI (trasferimento dati):** `effMBps` e `updateMs` **peggiori** del Test 5b e del baseline — non candidato per produzione display.
> - ⚠️ **RT (ritardo nanosleep):** `rtc_handler_us` **112 µs** (−9 µs vs 5b) — unico miglioramento RT misurato, ma **obiettivo < 100 µs non raggiunto** (mancano ~12 µs).
>
> Opzione D migliora leggermente il worst-case RT task e mantiene il basso traffico bus del 5b, ma penalizza fortemente l'upload display. Non sostituisce il 5b.

**Riduzione framebuffer / bypass pipeline display (lezione Test 6):**

> La strada della **diminuzione dei framebuffer** (eliminare layer intermedio texture/GPU o ridurre il doppio buffering scanout) si è rivelata **molto complicata** nel nostro stack: il framebuffer software PEG (`g_pyBitmap`) è intrinseco al motore di disegno PEG e non è rimovibile senza riscrivere `screen.cpp`, i driver DIB (`dib16.cpp` / `PEG_DoubleBufferingRefresh`) e gran parte della catena GUI. Il Test 6 (Opzione D) ha dimostrato che bypassare SDL/GPU è **tecnicamente fattibile** come POC (`pegdrmoutput.cpp`), ma con costi GUI alti e integrazione touch/display non banale. **Non è impossibile**, ma richiede un **approfondimento futuro** dedicato (refactor architetturale display, non una semplice macro): valutare solo se le leve kernel/driver (DCC, pixel clock, Bpp) non bastano a raggiungere l'obiettivo RT < 100 µs.

**Esito sintetico:**

| Asse | Esito Test 6 |
|------|--------------|
| **GUI** (`effMBps` / `updateMs`) | ❌ Peggioramento |
| **RT** (`rtc_handler_us`) | ⚠️ Leggero miglioramento (112 µs), obiettivo <100 non raggiunto |

**Raccomandazione attuale:** **rollback produzione a Test 0** (`EMBEDDED_HMI_RT_NATIVE_TEXTURE` disattivata) per il display — migliore compromesso RT misurato rispetto al 5b; tenere il codice Opzione D per esperimenti futuri.

**Stato codice:** macro `EMBEDDED_HMI_RT_DRM_DIRECT` **disattivata**; `EMBEDDED_HMI_RT_NATIVE_TEXTURE` **disattivata** (baseline Test 0); codice sorgente Opzione D conservato.

**Verifica deploy:**

```bash
strings libPegLib.so | grep '\[RT\] drm'
strings libPegLib.so | grep 'KMS disponibile'
./PegExec 2>&1 | grep -E '\[RT\] drm|\[RT\] evdev|\[RT\] drm_direct'
```

**Atteso in avvio:**

```text
[RT] drm_direct: Opzione D — output DRM dumb RGB565, SDL solo eventi
[RT] drm: probe /dev/dri/card0 → driver vivante ...
[RT] drm: KMS disponibile su /dev/dri/card1
[RT] drm: connector=... mode=... buffer=1024x600 RGB565
[RT] evdev: touch multitouch (...) da /dev/input/eventN
```

**Rollback:** commentare `EMBEDDED_HMI_RT_DRM_DIRECT`, riattivare `EMBEDDED_HMI_RT_NATIVE_TEXTURE`, rebuild.

---

#### Fallimento avvio — `drmModeGetResources: Operation not supported` + segfault (2026-07-09)

Su i.MX8MP **`/dev/dri/card0`** può essere il nodo GPU (Vivante) senza KMS legacy; il display è spesso su **`card1`** (driver `imx-drm`). La prima versione apriva `card0` senza verificare `GetResources` → `EOPNOTSUPP` e possibile crash in `drmDropMaster` al teardown.

**Fix in `pegdrmoutput.cpp`:**

- Prova tutti i nodi da `drmGetDevices` + fallback `card0`..`card2`
- Tiene solo il fd per cui `drmModeGetResources` riesce
- Log `probe /dev/dri/cardN → driver imx-drm` (o altro)
- `drmDropMaster` solo se `drmSetMaster` è riuscito
- `lv_deinit()` in `shutdown()` per uscita pulita su init fallita

**Log atteso dopo fix:**

```text
[RT] drm: probe /dev/dri/card0 → driver vivante ...
[RT] drm: /dev/dri/card0 drmModeGetResources: Operation not supported
[RT] drm: probe /dev/dri/card1 → driver imx-drm ...
[RT] drm: KMS disponibile su /dev/dri/card1
[RT] drm: connector=... mode=1024x600 ...
```

**Verifica deploy:** `strings libPegLib.so | grep 'KMS disponibile'`

**Rollback:** commentare `EMBEDDED_HMI_RT_DRM_DIRECT`, riattivare `EMBEDDED_HMI_RT_NATIVE_TEXTURE`, rebuild.

---

#### Nota avvio — `ERROR: Could not restore CRTC`

Il log di diagnosi **non modifica** DRM/SDL: è solo `fprintf`. L'errore `Could not restore CRTC` compare tipicamente in **teardown** SDL/KMSDRM (uscita crash o chiusura display), non è causato dai log `[RT] diag:`.

Se l'interfaccia **si avvia a metà e poi si blocca**:

1. Verificare `rtos.ini` coerente (per test stabili: `XRes=800` `YRes=600` senza `XView`/`YView`, oppure produzione 1024×600 che prima funzionava)
2. Riavviare la scheda se DRM è in stato sporco dopo un crash
3. Controllare che `PegExec` e `libPegLib.so` siano della **stessa build**
4. Il warning `XView/YView != XRes/YRes` è informativo su 1024×600 viewport — non dovrebbe da solo bloccare l'avvio, ma su embedded conviene allineare i valori

---

<a id="test-7"></a>

## TEST 7 — Pixel clock display (kernel / DRM)

**Stato:** ⏸️ **Sospeso** (2026-07-10) — fase 0 ✅ · fasi 2–3 solo via **BSP Yocto** · **Repo app:** nessuna modifica · [← Tabella](#stato-test)

> **Decisione:** il test **non prosegue** nel percorso `pegenstein`. Diagnosi completata; ulteriore abbassamento del pixel clock richiede modifica device tree (e eventuale kernel) nel repo Yocto, rebuild immagine e reboot. Riprendere quando si lavora sul BSP.

---

> **Obiettivo:** ridurre la **banda DDR** verso il display abbassando il **pixel clock** (meno pixel/s letti dal LCDIF → meno traffico bus, potenziale beneficio RT).
>
> **Dove si agisce:** principalmente **kernel + device tree + BSP Yocto** — **non** in `PegLib` / `rtos.ini` (la risoluzione logica PEG resta 1024×600; cambia il **timing** fisico del pannello).
>
> **Piattaforma avn8mp:** `lcdifv3` → `imx8mp-ldb` (LVDS) → pannello (`card1-LVDS-1` connected). Kernel: `6.6.23-rt28`.

### Perché non è banale su i.MX8MP

Il driver **LDB** su 8MP spesso **forza** pixel clock a valori fissi:

| Canale LVDS | Clock tipico forzato dal driver |
|-------------|--------------------------------|
| Single | **74,25 MHz** |
| Dual | **74,25** o **148,5 MHz** |

Per usare un clock **più basso** (es. 60 MHz, 50 MHz) servono in genere:

1. Commentare il *mode fixup* in `drivers/gpu/drm/imx/imx8mp-ldb.c`
2. Aggiornare **device tree** (`assigned-clock-rates` su `media_blk_ctrl` — valore = **pixel_clock × 7**)
3. Eventualmente aggiungere una riga in `imx_pll1443x_tbl` (`drivers/clk/imx/clk-pll14xx.c`) se il PLL video non ha il punto giusto

Riferimenti: [Toradex — Display timings](https://developer.toradex.com/linux-bsp/application-development/multimedia/display-output-resolution-and-timings-linux/), [Variscite imx8mp-lvds-calculator](https://github.com/varigit/imx8mp-lvds-calculator).

**Formula:** `pixel_clock ≈ htotal × vtotal × refresh_Hz` (es. 1024×600 @ 60 Hz → dipende da blanking nel mode).

### Fasi del test

| Fase | Cosa | Dove | Stato |
|------|------|------|-------|
| **0** | Misurare mode e clock **attuali** | Target | ✅ 2026-07-10 |
| **1** | Mode alternativo via `modetest` | Target | ❌ **Saltata** — un solo mode |
| **2** | Abbassare clock via DT + patch LDB + rebuild immagine | Yocto/BSP | ⏸️ **Sospeso** |
| **3** | Misura RT comparativa (protocollo standard + delay Lnk 30 s) | Target | ⏸️ **Sospeso** (dopo fase 2) |

### Fase 0 — comandi sul target

```bash
modetest -M imx-drm -c 2>/dev/null
modetest -M imx-drm -p 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null
grep -iE 'lcdif|ldb|disp|pix|video_pll' /sys/kernel/debug/clk/clk_summary 2>/dev/null | head -40
cat /sys/class/drm/card1-LVDS-1/modes 2>/dev/null
```

### Fase 0 — risultati target avn8mp (2026-07-10)

**Contesto misura:** PegExec **non** in esecuzione (CRTC `fb=0`, DPMS=`Off`). Clock tree coerente col mode dichiarato.

#### Connector `LVDS-1` (id **35**, encoder **34**)

| Campo | Valore misurato |
|-------|-----------------|
| Risoluzione attiva | **1024×600** |
| Refresh (Hz) | **64,31** (non 60 Hz standard) |
| Pixel clock (mode DRM) | **49 500 kHz** (**49,5 MHz**) |
| Timing | htot **1214**, vtot **634** (hdisp 1024, vdisp 600) |
| Mode disponibili | **1** solo (`preferred, driver`) |
| Verifica formula | 1214 × 634 × 64,31 ≈ **49,5 MHz** ✓ |
| LVDS | **Single** (inferito: `media_ldb` = 346,5 MHz = 49,5 × **7**) |

#### Plane primario (id **31**, CRTC **33**)

| Formati scanout | `XR24` `AR24` **`RG16`** `XB24` `AB24` `AR15` |
|-----------------|-----------------------------------------------|

Il plane supporta **RG16** (RGB565), ma con SDL+GLES lo scanout effettivo resta **32 bpp** su `fb0` (vedi [TEST 5](#test-5)).

#### Clock tree (`clk_summary`)

| Clock | Frequenza | Note |
|-------|-----------|------|
| `video_pll1` | 1 039,5 MHz | PLL video |
| `media_ldb` | **346,5 MHz** | = pixel_clock × 7 |
| `media_disp2_pix` | **49,5 MHz** | **Pixel clock attivo** (path LCDIF → LVDS) |
| `media_disp1_pix` | 1 039,5 MHz | Non usato su questo output |

#### Banda teorica scanout (32 bpp, come `fb0` oggi)

`49,5 MHz × 4 byte/pixel` ≈ **198 MB/s** massimo teorico verso il pannello.

#### Conclusioni fase 0

1. **Un solo mode** → **fase 1 impossibile** senza aggiungere mode nel kernel/DT.
2. Il pannello gira già a **49,5 MHz**, **sotto** i 74,25 MHz tipici del fixup LDB single — timing **custom** da device tree, non il profilo “standard” 74,25 MHz.
3. Per abbassare ulteriormente il pixel clock serve **fase 2 BSP**: nuovo `display-timings` nel DT (es. 40–45 MHz @ ~50–55 Hz), eventuale patch `imx8mp-ldb.c` se il driver sovrascrive il clock, rebuild kernel+DTB.
4. **Prerequisito:** datasheet pannello — verificare range pixel clock e refresh tollerati prima di patch.
5. **Rischio:** clock troppo basso → flicker, artefatti, pannello nero; provare step piccoli (es. 45 MHz, poi 40 MHz).

**Decisione (2026-07-10):** test **archiviato** nel percorso HMI. Proseguimento **solo** modificando il BSP (DTB / kernel). Prossimo test attivo: **ottimizzazione immagini**.

### Fase 1 — prova soft (senza rebuild kernel)

**Esito:** ❌ **Non applicabile** — `modetest -c` espone **un solo mode** (1024×600 @ 64,31 Hz, 49,5 MHz). Nessun mode alternativo da selezionare con `modetest -s`.

### Fase 2 — patch BSP (da coordinare con chi gestisce Yocto)

Checklist tipica (non nel repo `pegenstein`):

- [ ] Backup DTB attuale
- [ ] Patch `imx8mp-ldb.c` — disabilitare fixup 74,25 / 148,5 MHz
- [ ] DT overlay: `assigned-clock-rates = <(pixel_clock_Hz * 7)>` su `&media_blk_ctrl`
- [ ] Se necessario: nuova riga `PLL_1443X_RATE` in `clk-pll14xx.c`
- [ ] Rebuild kernel + DTB + deploy + reboot
- [ ] Verifica: `modetest -c` mostra il nuovo clock; pannello stabile

### Fase 3 — misura RT

Stesso protocollo di [Test 0](#test-0):

1. PegExec avviato → attendere **≥ 30 s**
2. Avviare Lnk / PerfMonitor
3. Scenari: **idle** (no touch) e **drag grafico**
4. Confrontare `rtc_handler_us` worst, `nanosleep` max, `[RT] reqMBps`, `bus_access` vs baseline

**Attenzione:** clock troppo basso → flicker o pannello instabile; aumentare gradualmente.

### Cosa NON aspettarsi

- **Non** riduce i byte/pixel (resta **32 bpp** su `fb0` con SDL — vedi [TEST 5](#test-5))
- **Non** sostituisce riduzione dirty region o risoluzione PEG
- Beneficio RT: **meno letture DDR/s** dal LCDIF allo stesso formato → effetto da **quantificare** in fase 3

---

## Prossimi test in coda

> **Tabella aggiornata in cima al documento:** [Stato test — tabella unica](#stato-test) (sezione **Prossimi test**).
>
> Non duplicare qui — modificare solo la tabella in alto.

---

<a id="test-dcc"></a>

## TEST DCC — Framebuffer Compression / Prefetch (fase 0, i.MX8MP)

**Stato:** ❌ Non applicabile · **Data diagnosi:** 2026-07-10 · [← Tabella](#stato-test)

---

> **Obiettivo:** ridurre la **banda DDR** verso il display (meno byte letti dal controller scanout → meno interferenza sul task RT).
>
> **Attenzione piattaforma:** su **i.MX8M Plus** il path display in uso è **LCDIF V8** (`fsl,imx8mp-lcdif`) + driver **`imx-drm`** (card1 nel nostro setup). Il LCDIF legge framebuffer **lineare** da DDR verso FIFO interno. La **compressione framebuffer (DEC400 / DTRC / tiled)** documentata da NXP riguarda soprattutto **i.MX8MQ + DCSS**, non è documentata come feature del LCDIF 8MP. Il **prefetch engine (DPRC/PRG)** dei patch kernel recenti riguarda **imx8-dc** (famiglia i.MX8QXP/QM), non il nostro LCDIF.
>
> **Conclusione preliminare:** il Test DCC **non** è un toggle in `PegLib` o `rtos.ini` — va verificato **sul target** se il BSP/kernel espone compressione; se assente, il test si chiude come «non applicabile su 8MP LCDIF» e si passa al **pixel clock** (punto 4).

#### Fase 0 — comandi da eseguire sul target (prima di qualsiasi patch kernel)

Eseguire con PegExec **fermo** (o dopo `killall PegExec`) per non disturbare DRM:

```bash
# Kernel e BSP
uname -r
cat /proc/device-tree/compatible | tr '\0' '\n' | head -3

# Nodi DRM
ls -la /dev/dri/
cat /sys/class/drm/card*/device/uevent 2>/dev/null

# Driver display (atteso: imx-drm su card1, vivante su card0)
for c in /sys/class/drm/card*; do
  echo "=== $(basename $c) ==="
  cat $c/device/uevent 2>/dev/null | grep -E 'DRIVER|OF_NAME'
done

# modetest (pacchetto libdrm-tests, se presente)
modetest -M imx-drm -p 2>/dev/null | head -80
# oppure senza -M:
modetest -p 2>/dev/null | head -80

# Formati pixel e modifier (cercare compressione / tiled)
modetest -M imx-drm -p 2>/dev/null | grep -iE 'format|modifier|ARGB|RGB565|XRGB'

# Log kernel display
dmesg | grep -iE 'lcdif|imx-drm|dcss|dec400|dtrc|fbc|compress|prefetch' | tail -40
```

**Cosa cercare nei risultati:**

| Esito | Interpretazione | Prossimo passo |
|-------|-----------------|--------------|
| Solo formati lineari (`XR24`, `RG16`, `AR24`…) senza modifier compressi | **Nessun DCC esposto** dal driver attuale | Chiudere Test DCC su 8MP; passare a **Test pixel clock** (#4) |
| Property plane `FB_MOD_*` / formati tiled / `modetest` elenca modifier non-zero | Possibile supporto compressione | Fase 1: documentazione BSP NXP + DT/kernel per abilitarlo |
| Kernel vecchio, solo `fsl,imx8mp-lcdif` senza menzione FBC | Serve **upgrade BSP** o patch Yocto | Valutare con chi gestisce l'immagine Yocto (fuori repo `pegenstein`) |

#### Fase 1 — se la fase 0 mostra supporto (ipotetico)

Modifiche tipiche ( **non** nel repo applicazione):

- **Device tree:** nodi `dec400`, `dprc`, `prg` o property panel (dipende da BSP)
- **Kernel:** `CONFIG_DRM_IMX_*`, driver LCDIF con path compresso
- **Userspace:** framebuffer con modifier compresso (SDL/KMSDRM **probabilmente non supporta** senso comune — potrebbe richiedere path DRM diretto tipo Test 6)

#### Fase 2 — misura RT (solo se DCC attivabile)

Stesso protocollo standard + **ordine avvio Lnk ≥ 30 s dopo PegExec**:

| Scenario | Metriche |
|----------|----------|
| DCC off (baseline) | `rtc_handler_us` worst, `nanosleep` max, `[RT] reqMBps`, `perf` `bus_access` |
| DCC on | stesso protocollo, confronto |

**Nota SDL:** anche con DCC nel kernel, **PegExec → SDL_UpdateTexture → KMSDRM** potrebbe continuare a usare buffer **non compressi** in RAM; il beneficio DDR sarebbe solo sul tratto **scanout LCDIF** (ultimo miglio), non sull'upload CPU→texture.

#### Stato Test DCC

| Fase | Stato |
|------|-------|
| 0 — Diagnosi `modetest` / `dmesg` su avn8mp | ✅ **completata 2026-07-10** — DCC **non disponibile** |
| 1 — Abilitazione kernel/DT | ❌ **non applicabile** |
| 2 — Misura RT comparativa | ❌ **saltata** |

#### Fase 0 — risultati target avn8mp (2026-07-10)

**Kernel:** `6.6.23-rt28-g6d8254487cee` (PREEMPT_RT)

**DRM:**
```text
/dev/dri/card0   — GPU Vivante (renderD128)
/dev/dri/card1   — imx-drm (display)
```

**`modetest -M imx-drm -p`:**
```text
CRTC id=33  fb=0  size 0x0
Plane id=31  type=Primary  zpos=0 (unico plane)
formats: XR24 AR24 RG16 XB24 AB24 AR15 XR15
```
- Solo formati **lineari** standard (ARGB8888, RGB565, …)
- **Nessun** modifier compresso / tiled / `FB_MOD_*`
- **Un solo** plane Primary (no overlay con path compresso alternativo)

**`dmesg`:**
```text
imx-drm display-subsystem: bound imx-lcdifv3-crtc.0 (ops lcdifv3_crtc_ops)
imx-drm display-subsystem: bound ldb-display-controller (ops imx8mp_ldb_ops)
[drm] Initialized imx-drm 1.0.0 for display-subsystem on minor 1
```
- Pipeline: **LCDIFv3** → **LDB (LVDS)** → panel
- **Nessuna** menzione di `dec400`, `dtrc`, `fbc`, `compress`, `prefetch` nel boot

**Conclusione fase 0:** su questo BSP/kernel il driver **imx-drm + lcdifv3** espone solo scanout **lineare da DDR**. Non c'è API DRM per abilitare DCC/Prefetch come su i.MX8MQ+DCSS. Il Test DCC **non è realizzabile** senza cambio hardware/SOC o un BSP futuro che aggiunga supporto (non documentato su 8MP LCDIF).

---

<a id="bpp-gia-fatto"></a>

### Profondità di colore (Bpp): già fatto + come modificare

**Stato attuale:** lo stack gira già a **16 bpp RGB565** (`Bpp=16` in `rtos.ini`). È la profondità minima praticabile sul path embedded attuale (SDL/KMSDRM + `PEG_USE_LVGL`); l'alternativa **24 bpp** è presente nel file ma **commentata** — raddoppierebbe i byte per pixel del framebuffer PEG e dell'upload.

**Cosa è stato verificato:**

- Config produzione / test: `Bpp=16`, `ForceBPP=True`
- Log avvio: `[RT] rtos.ini XRes=… YRes=… → framebuffer effettivo … @ 16 bpp = X.XX MiB` (`peg_run.cpp`)
- Test 4 (800×600) ha ulteriormente ridotto il traffico pixel mantenendo 16 bpp

**Modifica rapida del Bpp (senza rebuild)** — solo `rtos.ini` + riavvio `PegExec`:

| File | Azione |
|------|--------|
| **Target** | `/opt/Squeeze/rtos.ini` |
| **Repo** | `pegenstein/Files/avn8mp/rtos.ini` |

```ini
[PEG]
Bpp=16          ; ← attuale (RGB565, 2 byte/pixel)
;Bpp=24         ; ← alternativa: 3 byte/pixel, più banda DDR (solo test comparativo)
ForceBPP=True   ; obbligatorio: forza il valore da ini
```

**Valori supportati sul path embedded (`PEG_USE_LVGL`):**

| `Bpp` | Driver PEG | `createSurface` SDL | Note |
|------:|------------|---------------------|------|
| **16** | ✅ `dib16` / RGB565 | ✅ | **produzione attuale** |
| **24** | ✅ `dib24` | ✅ | +50% byte vs 16 bpp |
| 8 / 1 / 4 | parziale in `peg_run` | ❌ | non usabile su embedded attuale |
| 32 | commentato in `peg_run` | ✅ in `peglvglwindow` | non esposto da ini |

**Procedura deploy:**

1. Modificare `Bpp=` in `rtos.ini`
2. Copiare `rtos.ini` in `/opt/Squeeze/`
3. Riavviare `PegExec` (nessun rebuild di `libPegLib.so` necessario)
4. Verificare: `./PegExec 2>&1 | grep '\[RT\] rtos.ini'`

**Esempio impatto buffer @ 1024×600:**

| `Bpp` | Byte/pixel | Framebuffer PEG |
|------:|-----------:|----------------:|
| 16 | 2 | **~1,17 MiB** |
| 24 | 3 | **~1,76 MiB** (+50%) |

> Per ridurre ulteriormente la banda pixel oltre il 16 bpp, le leve rimanenti sono **risoluzione** (Test 4 ✅), **DCC/Prefetch** (punto 3) e **ottimizzazione asset** (punto 6) — non scendere sotto 16 bpp senza refactor del pipeline colore PEG/SDL.

---

## Note operative

- **Deploy:** copiare sempre `libPegLib.so*`, `PegExec` **e** `rtos.ini` aggiornato in `/opt/Squeeze/`
- **Avvio RT:** non avviare Lnk subito dopo PegExec — attendere **≥ 30 s** (vedi protocollo sopra)
- **Trappola:** log `[RT]` visibili con `.so` vecchio anche se macro commentata → verificare con `strings`
- **Diagnosi SDL:** `strings libPegLib.so | grep '\[RT\] diag'` — se assente, macro `EMBEDDED_HMI_RT_DIAG` non compilata
- **LVGL:** irrilevante per il percorso display; non ottimizzare cache LVGL per questo problema

---

## Template nuova voce

Copiare e compilare per ogni nuovo test:

```markdown
<a id="test-n"></a>

## TEST N — Titolo (YYYY-MM-DD)

**Stato:** … · **Macro:** … · [← Tabella](#stato-test)

---

**Modifica:** …
**Build/macro:** …
**Scenario:** idle | drag grafico | popup | altro

| Scenario | calls/s | reqMBps | updateMs/s | effMBps | maxRectPx | %CPU | RES |
|----------|--------:|--------:|-----------:|--------:|----------:|-----:|----:|
| … | | | | | | | |

**Log esempio:**
\`\`\`text
(incollare 2–3 righe [RT])
\`\`\`

**Conclusione:** ✅ / ❌ / ⏳ — …
**Stato codice:** attivo | commentato | rollback
```
