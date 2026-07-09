# Registro test RT — GUI PEG/SDL su i.MX8M Plus

> **File vivo**: aggiornato a ogni esperimento sul target o modifica rilevante nel codice.
> Ultimo aggiornamento: **2026-07-09** (Test 6 completato — Opzione D DRM dumb buffer + fix touch evdev)

Documentazione di supporto (approfondimenti):

| File | Contenuto |
|------|-----------|
| `pipeline_peg_sdl_drm_rt.md` | Architettura pipeline, thread, ruolo LVGL |
| `analisi_metriche_gui_rt.md` | Significato di `calls`, `reqMBps`, `updateMs`, `effMBps`, `maxRectPx` |
| `interferenza_cpu_ddr_idle_vs_interazione.md` | Argomento CPU/DDR vs GPU per la tesi |
| `riduzione_framebuffer_esperimenti.md` | Opzioni A–E (formato texture, LockTexture, DRM diretto, …) |
| `osservazioni` / `test-perf` | Note raw su `perf stat`, `htop`, `gc/meminfo` |

---

## Obiettivo

Ridurre **interferenza real-time** (jitter, traffico DDR, carico CPU) della GUI — **non** aumentare gli FPS.

Stack: **PEG** disegna su framebuffer software → `uploadDirtyRegion()` → *(path attivo Test 6)* `blitDirtyRegion()` → dumb buffer DRM → `drmModePageFlip()` | *(path 5b)* `SDL_UpdateTexture()` → `SDL_RenderPresent()` → DRM/KMS. LVGL è inizializzato ma **non** gestisce widget/display.

---

## Configurazione di riferimento

| Parametro | Valore |
|-----------|--------|
| Target | i.MX8M Plus, Linux PREEMPT_RT, Yocto |
| Build | `PEG_USE_LVGL`, deploy in `/opt/Squeeze/` |
| Bpp | 16 (RGB565) |
| Risoluzione baseline misure `[RT]` | ~**960×640** effettivi su schermo (da log `maxRectPx`; `rtos.ini` repo era 1024×768) |
| Risoluzione attuale sul target | **1024×600 viewport** (`XRes=1024` `YRes=768`, `XView=1024` `YView=600`) — Test 6 |
| Macro attive in `PegLib.pro` | `EMBEDDED_HMI_RT_STATS` ✅, `EMBEDDED_HMI_RT_DIAG` ✅, `EMBEDDED_HMI_RT_NATIVE_TEXTURE` ✅ (Test 5b produzione) |
| Macro disattivate | `EMBEDDED_HMI_RT_DRM_DIRECT` (Test 6 rollback), `EMBEDDED_HMI_RT_SAFE` (coalescing) |

### Buffer per risoluzione @ 16 bpp

| Risoluzione | Pixel | Buffer PEG (MiB) | Note |
|-------------|------:|-----------------:|------|
| 1024×768 | 786 432 | 1,50 | baseline `rtos.ini` originale |
| 1024×600 (viewport attuale target) | 614 400 | 1,17 | `XView=1024` `YView=600` su `/opt/Squeeze/rtos.ini` |
| **800×600** | 480 000 | **0,92** | Opzione 1 attuale (−39% vs 1024×768) |
| 640×480 | 307 200 | 0,59 | step più aggressivo (da provare) |

### Protocollo misura standard

Per ogni test, due scenari sul target:

1. **Idle** — GUI avviata, nessuna interazione
2. **Drag grafico** — trascinamento touch sul grafico (scenario critico)

Metriche da raccogliere:

- `[RT] uploadDirtyRegion`: `calls`, `reqMBps`, `updateMs`, `effMBps`, `maxRectPx`
- `htop`/`top`: %CPU e RES di `PegExec`
- Opzionale: `perf stat` (vedi `osservazioni`)

Verifica deploy:

```bash
strings libPegLib.so | grep '\[RT\]'
./PegExec 2>&1 | head   # log avvio: [RT] framebuffer PEG ...
```

---

## Riepilogo rapido esperimenti

| # | Esperimento | Esito | Impatto RT |
|---|-------------|-------|------------|
| 0 | Baseline + strumentazione `EMBEDDED_HMI_RT_STATS` | ✅ misure raccolte | — (riferimento) |
| 1 | `flushPresent(false)` | ❌ nessun beneficio misurabile | Basso |
| 2 | Coalescing motion (`EMBEDDED_HMI_RT_SAFE`) | ❌ nessun miglioramento su drag | Basso |
| 3 | SW surface present (`EMBEDDED_HMI_SW_SURFACE_PRESENT`) | ❌ crash / latenza peggiorata → **rollback completo** | Negativo |
| 4 | Riduzione risoluzione **800×600** (Opzione 1) | ✅ **miglioramento misurabile** | **Alto** |
| 5 | Diagnosi formato texture SDL (Opzione A) | ✅ **mismatch RGB565 confermato** | **Alto** (spiega `effMBps` basso) |
| **5b** | Texture ARGB8888 nativa + conversione esplicita | ✅ **miglioramento misurabile** | **Alto** |
| **6** | DRM dumb buffer RGB565 diretto (Opzione D POC) | ⚠️ **misto** — RT migliorato, upload GUI peggiore | **Alto** (`rtc_handler_us` 112 µs, obiettivo < 100) |
| 6 | `SDL_LockTexture` streaming (Opzione B) | ⬜ non eseguito | — |
| 7 | Double buffering KMSDRM hint (Opzione C) | ⬜ non eseguito | — |

---

## Cronologia dettagliata

### Test 0 — Baseline strumentazione (lug 2026)

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

**Conclusione:** collo di bottiglia principale = **copia CPU→texture** (`SDL_UpdateTexture`), non saturazione GPU. Scenario critico = **drag grafico** (~21% core in upload, ~24 MB/s).

---

### Test 1 — `flushPresent(false)` (lug 2026)

**Modifica:** `peg_video_window->flushPresent(false)` invece di `true`.

**Risultato:** ❌ **nessun beneficio apprezzabile**.

**Motivo:** `SDL_RENDERER_PRESENTVSYNC` + `SDL_VIDEO_DOUBLE_BUFFER=1` → il vsync/pageflip limita già a ~60 Hz; il rate limiter software era ridondante.

**Stato codice:** ripristinato / non considerato prioritario.

---

### Test 2 — Coalescing eventi motion (lug 2026)

**Modifica:** `EMBEDDED_HMI_RT_SAFE` — in `processEvents()` tiene solo l’ultimo `SDL_FINGERMOTION` / `SDL_MOUSEMOTION` per batch di eventi SDL.

**Risultato:** ❌ **nessun miglioramento misurabile** sul drag grafico (`calls` resta ~33/s, `reqMBps` ~24).

**Ipotesi:** il redraw è guidato da timer/PEG (~30 Hz), non dal flood di eventi SDL motion.

**Stato codice:** macro **commentata** in `PegLib.pro`; codice presente ma non compilato.

---

### Test 3 — Present via window surface (`EMBEDDED_HMI_SW_SURFACE_PRESENT`) (lug 2026)

**Obiettivo:** eliminare texture/renderer, presentare con `SDL_UpdateWindowSurfaceRects`.

**Risultato:**

1. Prima versione: crash `Surface already associated with window` + segfault
2. Dopo fix (`SDL_GetWindowPixelFormat` prima di `GetWindowSurface`): avvio ok
3. Su target con drag grafico: **latenza massima peggiorata** (screenshot jitter)

**Esito finale:** ❌ **rollback completo** — macro e codice rimossi.

---

### Test 4 — Riduzione risoluzione 800×600, Opzione 1 (2026-07-09) — ✅ SUCCESSO

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

#### B) Confronto scroll grafico — baseline 1024×600 viewport vs 800×600

| Metrica | Baseline (1024×600 viewport, Test 0) | **800×600** | Variazione |
|---------|---------------------------------------|-------------|------------|
| `reqMBps` (scroll) | ~24 MB/s | ~10–14 MB/s | **≈ −42% … −58%** |
| `updateMs` (scroll) | ~208–220 ms/s | ~115–152 ms/s | **≈ −27% … −45%** |
| `maxRectPx` | 485 051 (~79% di 614 400) | 368 347 (~77% di 480 000) | area assoluta **−24%** |
| `calls/s` | ~33 | ~27–33 | simile |

La % di schermo sporco resta ~77–79%, ma con meno pixel assoluti il traffico e il tempo in `SDL_UpdateTexture()` scendono in modo netto.

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

**Conclusione:** l’Opzione 1 (riduzione risoluzione via `rtos.ini`) è la prima modifica con **beneficio RT misurabile** su pipeline GUI (`reqMBps`, `updateMs`), CPU `PegExec` e task RTC (`rtc_handler_us`, traffico bus). Il ritardo percepito durante lo scroll del grafico risulta migliore.

**Limiti / note:**

- L’UI è progettata per risoluzioni più alte: verificare leggibilità e layout a 800×600 prima di produzione.
- Per cambiare risoluzione su embedded: `XRes`/`YRes` devono coincidere con il framebuffer reale; non usare `XView`/`YView` diversi da `XRes`/`YRes`.
- Prossimo step opzionale: Opzione A (formato texture SDL) per ulteriore riduzione.

**Stato:** config **800×600** validata sul target.

---

### Test 5 — Diagnosi pipeline SDL / formato texture (Opzione A) (2026-07-09) — ✅ COMPLETATO

**Obiettivo:** verificare mismatch formato texture / renderer e configurazione buffer KMSDRM.

**Modifica:** macro `EMBEDDED_HMI_RT_DIAG` + `rtDiagLogSdlPipeline()` in `peglvglwindow.cpp` (solo log, nessun cambio pipeline).

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

#### Conclusione

**Confermato:** su i.MX8MP con renderer **OpenGL ES2**, la texture **RGB565 non è un formato nativo**. `SDL_UpdateTexture()` da framebuffer PEG 16 bpp probabilmente esegue **`SDL_ConvertPixels` nascosta** verso un formato GLES (es. ARGB8888) prima dell’upload GPU.

Questo spiega perché:

- `effMBps` in scroll resta ~90–115 MB/s (lento per una memcpy pura)
- ridurre risoluzione (Test 4) aiuta ma non elimina il costo di conversione
- **Opzione B (`LockTexture`) da sola** probabilmente **non basta** (stesso mismatch formato)

#### Prossimo esperimento consigliato (Test 5b)

| Opzione | Descrizione | Sforzo |
|---------|-------------|--------|
| **5b-1** | Texture **ARGB8888** nativa + conversione esplicita RGB565→ARGB8888 in `uploadDirtyRegion` | medio — misurare se più veloce della conversione nascosta SDL |
| **5b-2** | Texture **RGB888** nativa + conversione 16→24 bpp | medio — 50% più byte ma formato nativo |
| Opzione D | DRM dumb buffer RGB565 diretto | alto — bypass GPU/SDL texture |

**Opzione C (triple→double):** già a **double** (`SDL_VIDEO_DOUBLE_BUFFER=1`) — nessun margine su questo fronte.

**Stato codice:** diagnosi attiva; **nessuna modifica funzionale** alla pipeline oltre al log.

---

### Test 5b — Texture ARGB8888 nativa + conversione esplicita (2026-07-09)

**Modifica:** macro `EMBEDDED_HMI_RT_NATIVE_TEXTURE` in `PegLib.pro`. Con framebuffer PEG 16 bpp (RGB565):
- `SDL_CreateTexture(..., ARGB8888, STREAMING)` invece di RGB565
- in `uploadDirtyRegion()`: `SDL_ConvertPixels(RGB565 → ARGB8888)` su buffer staging riusabile, poi `SDL_UpdateTexture` con pitch `rect.w * 4`
- metriche `[RT]`: `reqMBps` conta byte **ARGB8888** (4 bpp) per confronto onesto del costo upload texture

**Build/macro:** `EMBEDDED_HMI_RT_STATS` + `EMBEDDED_HMI_RT_DIAG` + `EMBEDDED_HMI_RT_NATIVE_TEXTURE`

**Config target:** produzione **1024×600 viewport** (`XRes=1024` `YRes=768`, `XView=1024` `YView=600`) — stesso scenario del Test 0 baseline

**Scenario:** idle | drag grafico (scroll) — protocollo standard

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

```text
[WORST rtc_handler_us] iter=6365  rtc_handler_us=123 us
  L2 miss=26.99%  bus_access=8803  bus_cycles=361523  CPI=5.14
```

| Metrica RT worst case | Test 0 baseline scroll | Test 5b scroll | Variazione |
|-----------------------|----------------------:|---------------:|-----------:|
| `rtc_handler_us` max | **122 µs** | **123 µs** | ≈ uguale |
| `bus_access` | 24 755 | **8 803** | **−64%** |
| `bus_cycles` | 853 688 | **361 523** | **−58%** |

**Conclusione:** ✅ **Test 5b positivo.** Texture ARGB8888 nativa + `SDL_ConvertPixels` esplicito riduce il tempo CPU in upload (`updateMs/s` −22%) e raddoppia il throughput effettivo (`effMBps`), a parità di area dirty aggiornata. Il traffico bus DDR in scroll cala sensibilmente (−64% `bus_access`) anche se il worst-case `rtc_handler_us` resta ~122–123 µs. `reqMBps` raddoppia come previsto (conteggio 4 bpp) ma non è regressione: i pixel sorgente sono gli stessi.

**Raccomandazione:** tenere `EMBEDDED_HMI_RT_NATIVE_TEXTURE` attivo in produzione. Prossimo step opzionale: combinare con risoluzione piena 1024×768 allineata (`XRes=YRes=1024/768`, senza `XView`/`YView`) e misurare impatto vs viewport 1024×600.

**Stato codice:** macro **attiva** in `PegLib.pro`.

---

### Test 6 — Opzione D: DRM dumb buffer RGB565 diretto (2026-07-09) — ✅ misurato

**Obiettivo:** bypassare SDL texture + GPU; una sola copia RGB565 PEG → scanout. Target: `rtc_handler_us` **< 100 µs** a 1024×600 produzione.

**Modifica:** macro `EMBEDDED_HMI_RT_DRM_DIRECT` in `PegLib.pro` (disattiva `EMBEDDED_HMI_RT_NATIVE_TEXTURE`).

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

**Build/macro:** `EMBEDDED_HMI_RT_STATS` + `EMBEDDED_HMI_RT_DRM_DIRECT` (no `RT_NATIVE_TEXTURE`, no `RT_DIAG` SDL)

**Config target:** produzione `XRes=1024 YRes=768` + `XView=1024 YView=600`, `Bpp=16`

**Scenario:** idle (nessun touch) | scroll grafico (drag touch) — protocollo standard

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
- Durante lo **scroll** l’area dirty si riduce (`maxRectPx≈212k`) e `updateMs` scende a ~30–46 ms — pattern **invertito** rispetto a 5b (dove lo scroll era lo scenario peggiore); probabile diverso partizionamento dirty PEG + attesa `pageFlip` DRM.
- Il touch funziona dopo fix evdev (`m_reportedDown` su `SYN_REPORT`).

**Conclusione:** ⚠️ **Test 6 parziale.** Opzione D migliora leggermente il worst-case RT task e mantiene il basso traffico bus del 5b, ma **penalizza fortemente il tempo di upload display** (`updateMs`, `effMBps`). Non è candidata a sostituire il 5b per il solo obiettivo GUI, salvo ulteriori ottimizzazioni (dirty più piccoli, async flip, zero full-chart idle).

**Raccomandazione attuale:** **rollback produzione a Test 5b** (`EMBEDDED_HMI_RT_NATIVE_TEXTURE`) per il display; tenere il codice Opzione D per esperimenti. Prossimi step D: profilare `blitDirtyRegion` + `drmModePageFlip`, capire perché idle animato marca ~79% schermo dirty, valutare `DMA`/`imx-drm` plane overlay.

**Stato codice:** macro **disattivata** in `PegLib.pro` (rollback a Test 5b); codice sorgente conservato per esperimenti futuri.

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

#### Nota avvio — `ERROR: Could not restore CRTC`

Il log di diagnosi **non modifica** DRM/SDL: è solo `fprintf`. L’errore `Could not restore CRTC` compare tipicamente in **teardown** SDL/KMSDRM (uscita crash o chiusura display), non è causato dai log `[RT] diag:`.

Se l’interfaccia **si avvia a metà e poi si blocca**:

1. Verificare `rtos.ini` coerente (per test stabili: `XRes=800` `YRes=600` senza `XView`/`YView`, oppure produzione 1024×600 che prima funzionava)
2. Riavviare la scheda se DRM è in stato sporco dopo un crash
3. Controllare che `PegExec` e `libPegLib.so` siano della **stessa build**
4. Il warning `XView/YView != XRes/YRes` è informativo su 1024×600 viewport — non dovrebbe da solo bloccare l’avvio, ma su embedded conviene allineare i valori

---

## Prossimi test in coda

1. ~~**Test 5b**~~ — ✅ completato (texture ARGB8888 nativa)
2. ~~**Test 6 / Opzione D**~~ — ✅ misurato (112 µs RT, upload GUI peggiore → rollback a 5b consigliato)
3. **Test 6b** — profilare Opzione D: `pageFlip` blocking, dirty idle 485k px, DMA/plane overlay imx-drm
4. **Combinazione 5b + risoluzione** — allineare `XRes=YRes` a 1024×600 senza `XView`/`YView` (layout permitting)
5. **Popup** — log coordinate `maxRect` (x,y,w,h)

---

## Note operative

- **Deploy:** copiare sempre `libPegLib.so*`, `PegExec` **e** `rtos.ini` aggiornato in `/opt/Squeeze/`
- **Trappola:** log `[RT]` visibili con `.so` vecchio anche se macro commentata → verificare con `strings`
- **Diagnosi SDL:** `strings libPegLib.so | grep '\[RT\] diag'` — se assente, macro `EMBEDDED_HMI_RT_DIAG` non compilata
- **LVGL:** irrilevante per il percorso display; non ottimizzare cache LVGL per questo problema

---

## Template nuova voce

Copiare e compilare per ogni nuovo test:

```markdown
### Test N — Titolo (YYYY-MM-DD)

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
