# Registro test RT — GUI PEG/SDL su i.MX8M Plus

> **File vivo**: aggiornato a ogni esperimento sul target o modifica rilevante nel codice.
> Ultimo aggiornamento: **2026-07-21** (cgroups `cpu` non disponibile su avn8mp; ruolo LVGL; differenze strutturali; inventario file; banda DDR Test 0 vs 6)
---

## Documentazione di supporto (approfondimenti)

| File | Contenuto |
|------|-----------|
| `pipeline_peg_sdl_drm_rt.md` | Architettura pipeline, thread, ruolo LVGL |
| `analisi_metriche_gui_rt.md` | Significato di `calls`, `reqMBps`, `effMBps`, `maxRectPx`; **`updateMs` per test** → [sezione dedicata](#significato-updateMs) in questo registro |
| `interferenza_cpu_ddr_idle_vs_interazione.md` | Analisi interferenza CPU/DDR vs GPU |
| `riduzione_framebuffer_esperimenti.md` | Opzioni A–E (formato texture, LockTexture, DRM diretto, …) |
| `osservazioni` / `test-perf` / `valori_perf.txt` | Note raw `perf` / `htop`; dump DDR cycles → [banda MB/s](#banda-ddr-perf) |
| [DIAGNOSTICA TEST 6](#diagnostica-test-6) | **In questo file** — come attivare/disattivare ogni macro di trace (`[CAD]`, `[RT]`, crash) |
| [Banda DDR da `perf`](#banda-ddr-perf) | **In questo file** — metriche `read/write-cycles`, formula MB/s, riposo vs scroll |
| [Differenze strutturali interfacce](#differenze-strutturali-interfacce) | **In questo file** — confronto Qt vs SDL/Test 0 vs DRM/Test 6 |
| [Ruolo reale di LVGL](#ruolo-reale-lvgl) | **In questo file** — perché `PEG_USE_LVGL` non significa “GUI LVGL”; cosa fanno `lv_init` / `pumpLvgl` / `lv_timer_handler` |
| [Cgroups CPU non disponibile](#cgroups-cpu-non-disponibile) | **In questo file** — perché non si può fare quota 10 ms con `cpu.max` su avn8mp (dimostrazione comandi/output) |
| [File modificati vs Test 0](#file-modificati-vs-test-0) | **In questo file** — inventario completo dei sorgenti toccati da Test 0 → Test 6 (pegenstein, pressbrakepeg, kvuib, doc) |

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
| Framebuffer PEG (allocazione) | **1024×768** @ 16 bpp ≈ **1,50 MiB** (+ 1 riga slack, come path legacy) — pitch driver DIB16 |
| Framebuffer scanout DRM (Test 6) | **1024×600** @ RGB565 dumb buffer ≈ **1,17 MiB** — solo righe visibili (`YView`) |
| Macro attive in `PegLib.pro` | `EMBEDDED_HMI_RT_STATS` ✅, `EMBEDDED_HMI_RT_DIAG` ✅, **`EMBEDDED_HMI_RT_DRM_DIRECT`** ✅ (Test 6, branch `experiment/test6-drm`) |
| Macro disattivate | `EMBEDDED_HMI_RT_NATIVE_TEXTURE` (Test 5b rollback), `EMBEDDED_HMI_RT_STREAMING_LOCK` (Test B rollback) |

---

### Buffer per risoluzione @ 16 bpp

| Risoluzione | Pixel | Buffer PEG (MiB) | Note |
|-------------|------:|-----------------:|------|
| 1024×768 | 786 432 | 1,50 | baseline `rtos.ini` originale |
| 1024×768 (buffer PEG logico, pitch driver) | 786 432 | 1,50 | `XRes×YRes` — **allocazione corretta** dal 2026-07-13 pomeriggio |
| 1024×600 (viewport / scanout DRM) | 614 400 | 1,17 | `XView×YView` — solo display; **non** dimensionare il buffer PEG così |
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

<a id="significato-updateMs"></a>

#### Significato di `updateMs` — cosa cronometra in ogni test

> **Definizione comune** (tutti i test con `EMBEDDED_HMI_RT_STATS`): `updateMs` è la **somma del tempo CPU** (in millisecondi) speso **dentro** la fase di upload della dirty region in `uploadDirtyRegion()`, **accumulata nell’ultima finestra di report (~1 s)**. Non è la durata di un singolo frame né il tempo totale della pipeline display.
>
> Implementazione: `peglvglwindow.cpp` — `rtT0` immediatamente prima dell’upload, `rtT1` subito dopo; `s_rtStatsUpdateNs += (rtT1 - rtT0)`.

**Formula correlata:**

```text
effMBps = req_MB / (updateMs / 1000)   → throughput solo nel tempo “dentro” l’upload
reqMBps = req_MB / durata_finestra_s    → byte al secondo di calendario (include pause tra upload)
```

| Test | Macro / path | Cosa include `updateMs` | Cosa **non** include |
|------|--------------|-------------------------|----------------------|
| **0** | baseline SDL | **`SDL_UpdateTexture()`** — copia framebuffer PEG → texture SDL (RGB565; conversione nascosta possibile in SDL) | `SDL_RenderCopy`, `SDL_RenderPresent` (`flushPresent`) |
| **1** | `flushPresent(false)` | Identico al **Test 0** (stesso cronometro su `SDL_UpdateTexture`) | come Test 0 |
| **2** | coalescing motion | Identico al **Test 0** | come Test 0 |
| **3** | SW surface (rollback) | *(non in produzione)* upload verso surface SDL — stesso concetto di copia dirty | present SDL |
| **4** | risoluzione 800×600 | Identico al **Test 0** (stesso path SDL; meno pixel → `updateMs` più basso) | come Test 0 |
| **5** | solo diagnosi | Identico al **Test 0** (nessun cambio pipeline) | come Test 0 |
| **5b** | `RT_NATIVE_TEXTURE` | **`SDL_ConvertPixels(RGB565→ARGB8888)`** + **`SDL_UpdateTexture()`** (texture ARGB8888 nativa) · `req` conta byte **ARGB** (4 bpp) | `RenderCopy`, `RenderPresent` |
| **6** | `RT_DRM_DIRECT` | **`PegDrmOutput::blitDirtyRegion()`** — `memcpy` RGB565 PEG → dumb buffer back DRM, incluso catch-up zone stale e dirty corrente | **`syncBackFromPeg()`** (copia integrale pre-flip), **`drmModePageFlip()`**, drain coda dirty in `flushPresent()` |
| **B** | `RT_STREAMING_LOCK` (rollback) | **`SDL_LockTexture()`** + `memcpy` riga-per-riga + **`SDL_UnlockTexture()`** | `RenderCopy`, `RenderPresent` |

**Attenzione al confronto Test 0 vs Test 6:**

- Il **nome** della metrica è lo stesso, ma il **contenuto** del cronometro cambia (`SDL_UpdateTexture` vs `blitDirtyRegion`).
- In **Test 6**, `updateMs` **sottostima** il carico CPU reale in scroll: ogni pageflip esegue anche **`syncBackFromPeg()`** (~viewport intero, es. 1,17 MiB @ 1024×600) **fuori** da `updateMs`. Spiega perché `updateMs` può scendere del ~70% ma `%CPU` in `top` resta ~30%.
- In **Test 0**, il present (`RenderCopy` + `RenderPresent`) è anch’esso **fuori** da `updateMs`; il lavoro GPU/scanout 32 bpp non è contabilizzato in questa metrica.

**Verifica deploy path attivo:**

```bash
strings /opt/Squeeze/libPegLib.so | grep -E 'drm_direct|native_texture|SDL_LockTexture path'
# Test 6:  [RT] drm_direct: Opzione D ...
# Test 5b: [RT] native_texture: Test 5b ...
# Test B:  [RT] upload path: SDL_LockTexture + memcpy ...
# Test 0:  (nessuna delle stringhe sopra)
```

**Verifica deploy:**

```bash
strings libPegLib.so | grep '\[RT\]'
./PegExec 2>&1 | head   # log avvio: [RT] framebuffer PEG ..., crashdiag, PegLib build ...
strings libPegLib.so | grep -E 'crashdiag|framebuffer PEG|PegLib build'
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

<a id="rt-metodologia-scheduler"></a>

#### Metodologia misura RT — proxy scheduler (`COM RTC Handler`, CPU3)

> **Cosa si misura davvero:** non il ritardo di *ogni* thread del sistema, ma il **ritardo di risveglio del `nanosleep`** nel thread **`COM RTC Handler`** su **CPU3**. Questo thread fa da **scheduler RT** per gli altri task real-time sulla stessa CPU.

**Perché basta (per ora) misurare solo lui**

In un programma di test **minimale** (scheduler + un solo worker bloccato su semaforo) è stato osservato che:

- se il **`nanosleep` dello scheduler non slitta**, anche il **semaforo** su cui attende l’altro thread **non slitta**;
- il ritardo del risveglio scheduler è quindi un **proxy conservativo** dell’interferenza GUI/DDR sulla catena RT su CPU3.

Per le campagne GUI (Test 0–6) la metrica registrata resta:

| Metrica | Significato |
|---------|-------------|
| **`nanosleep` max/min** | latenza cumulata del ciclo scheduler (log ogni N attivazioni) |
| **`rtc_handler_us` worst** | istanza peggiore del handler RTC, con contatori PMU (`bus_access`, L2 miss, IPC, …) |
| **Conteggio > 100 µs** | spike oltre soglia obiettivo produzione |

**Obiettivo:** `rtc_handler_us` / `nanosleep` max **< 100 µs** (spike occasionali accettabili se rare, es. < 0,001% delle attivazioni).

**Processo strumentato:** `./Enk` (Lnk / PerfMonitor) — il monitor aggancia il thread **`COM RTC Handler`**, non l’intero albero `./Enk`.

**Thread su CPU3 — cosa include / esclude** (screenshot `htop`, ottimizzatore in run):

| Thread | CPU | Priorità (PR) | Misura RT? | Note |
|--------|-----|---------------|------------|------|
| **`COM RTC Handler`** | **3** | **−65** | ✅ **Sì — metrica principale** | scheduler RT, `nanosleep` |
| PLC Supervisor | 3 | −66 | ⬜ candidato estensione | RT, CPU3 |
| PLCScheduler | 3 | −65 | ⬜ candidato estensione | RT, CPU3 |
| MainRegol | 3 | −63 | ⬜ candidato estensione | RT, CPU3 |
| MainSlave | 3 | −61 | ⬜ candidato estensione | RT, CPU3 |
| PLC FAST | 3 | −62 | ⬜ candidato estensione | RT, CPU3 |
| PLC SLOW | 3 | −59 | ⬜ candidato estensione | RT, CPU3 |
| NCRun | 3 | −57 | ⬜ candidato estensione | RT, CPU3 |
| VerifyCommand | 3 | −43 | ⬜ candidato estensione | RT, CPU3 |
| **OSC Server** | **0** | 20 | ❌ **No** | non RT, altra CPU |
| PlcSMDisplay | 2 | 20 | ❌ No | non su CPU3 |
| PLC Log | 2 | 20 | ❌ No | non su CPU3 |
| `./Enk` (main) | 1 | 20 | ❌ No | processo host, non scheduler |

**Verifica affinità prima di ogni campagna:**

```bash
taskset -cp $(pidof Enk)    # o pidof Lnk — verificare quale wrapper usate sul target
# atteso per i thread RT: mask CPU3 = 0x8

# in htop: F2 → Columns, oppure filtrare per CPU 3 e PR negativo
```

**Estensione futura (opzionale — completare il quadro):**

Misurare anche il **ritardo di risveglio dei worker RT** bloccati su **semaforo** (es. `MainRegol`, `NCRun`, `PLC FAST`), sempre **solo thread RT su CPU3**:

1. timestamp uscita da `nanosleep` nello scheduler (`COM RTC Handler`);
2. timestamp post su semaforo + timestamp risveglio worker;
3. delta **semaphore wake latency** = ritardo oltre il periodo nominale del ciclo RT.

Se il proxy scheduler resta < 100 µs e in futuro un worker mostrasse spike isolati, la causa sarebbe **downstream** (priorità, lock interni PLC) e non interferenza GUI — utile per separare i due problemi.

> **Regola pratica campagne GUI:** finché **`COM RTC Handler` ≤ 100 µs** con **0 spike** su centinaia di migliaia di attivazioni (es. ottimizzatore ≥ 10 min, 73k+ att.), la GUI Test 6 **non degrada la catena RT** misurata finora.

---

<a id="stato-test"></a>

## Stato test — tabella unica

> **Questa è la tabella di riferimento.** I dettagli dei test eseguiti sono nelle sezioni **TEST 0** … **TEST 6** / **B** sotto ([cronologia](#cronologia-dettagliata)). Le leve ancora aperte (DCC, pixel clock, Bpp, asset, LVGL, font) sono in [**Possibili migliorie**](#possibili-migliorie).

### Test eseguiti (cronologia)

| Test | Descrizione | Esito sintetico | Dettaglio |
|------|-------------|-----------------|-----------|
| **0** | Baseline SDL RGB565 + `EMBEDDED_HMI_RT_STATS` | 📌 **Riferimento** GUI + RT | [→ TEST 0](#test-0) |
| **4** | Riduzione risoluzione **800×600** (Opzione 1) | ✅ GUI −27…45% · RT **97 µs** (−20%) | [→ TEST 4](#test-4) |
| **5** | Diagnosi formato texture SDL (Opzione A) | ✅ Diagnosi mismatch RGB565 · nessun fix runtime | [→ TEST 5](#test-5) |
| **5b** | Texture ARGB8888 nativa + conversione esplicita | ✅ GUI +150% · ❌ RT **191 µs** → **rollback** | [→ TEST 5b](#test-5b) |
| **6** | DRM dumb buffer RGB565 (Opzione D POC) | ✅ RT · ✅ GUI · ✅ stabilità **8ª build** · campagna **4 h** **0 crash** (2026-07-15) | [→ TEST 6](#test-6) |
| **B** | `SDL_LockTexture` streaming (Opzione B) | ❌ ≈ Test 0 · **rollback** | [→ TEST B](#test-b) |

**Produzione attuale:** Test **6** su branch `experiment/test6-drm` — RT/GUI OK; stabilità **validata** (build **8ª**, campagna **4 h** senza crash, **1** spike RT / **4,15 M** att.). Test **0** resta fallback noto.

### Possibili migliorie (sintesi)

| # | Voce | Stato | Dettaglio |
|---|------|-------|-----------|
| 1 | Framebuffer Compression (DCC / Prefetch) | ❌ **Non applicabile** su i.MX8MP LCDIF | [→ §1](#pm-dcc) |
| 2 | Pixel clock display (kernel / DRM) | ⏸️ **Sospeso** — solo via BSP Yocto (fase 0 ✅) | [→ §2](#pm-pixel-clock) |
| 3 | Profondità di colore (Bpp) | ✅ PEG 16 · scanout 16 **solo Test 6** (SDL=32) | [→ §3](#pm-bpp) · [scanout](#bpp-scanout-drm-16) |
| 4 | Ottimizzazione immagini (indicizzate / compressi) | ✅ **Già applicata** — audit 2026-07-20; residuo ~tens of KB | [→ §4](#pm-immagini) |
| 5 | Ridurre cache LVGL | ❌ **Non pertinente** (UI disegnata da PEG) | [→ §5](#pm-lvgl-cache) |
| 6 | Font — solo caratteri Unicode necessari | 🟡 **Parziale** — pulizia ridondanze Chs ✅; subset Unicode ancora aperto | [→ §6](#pm-font) |

Elenco completo con cosa è stato fatto/testato: [**Possibili migliorie**](#possibili-migliorie).

### Come leggere gli esiti (due assi indipendenti)

| Asse | Metriche | Cosa misura |
|------|----------|-------------|
| **GUI** | `effMBps`, `updateMs`, `reqMBps` | Upload interfaccia (`uploadDirtyRegion` → SDL/DRM) |
| **RT** | `rtc_handler_us`, `nanosleep` max | Ritardo risveglio **`COM RTC Handler`** (scheduler RT, CPU3) — vedi [metodologia](#rt-metodologia-scheduler); obiettivo **< 100 µs** |

> Un test può migliorare la **GUI** senza migliorare il **RT**, e viceversa.

---

<a id="possibili-migliorie"></a>

## Possibili migliorie

Leve di ottimizzazione (banda DDR, flash, RAM asset) **oltre** ai test di pipeline già chiusi in cronologia. Per ogni punto: obiettivo, cosa è stato fatto/testato, cosa si può o non si può fare.

---

<a id="pm-dcc"></a>

### 1 — Framebuffer Compression (DCC / Prefetch) nel driver DRM/KMS

**Obiettivo:** ridurre in modo significativo la banda memoria verso lo scanout display.

| | |
|--|--|
| **Stato** | ❌ **Non applicabile** su i.MX8MP (LCDIFv3) con il BSP attuale |
| **Dove si agisce** | Kernel / DRM — **non** in PegLib / `rtos.ini` |
| **Fatto / testato** | Fase 0 sul target avn8mp (2026-07-10): `modetest`, formati plane, `dmesg` imx-drm. Pipeline **LCDIFv3 → LDB LVDS**; solo formati **lineari** (`XR24`, `RG16`, …). Nessun modifier compresso / DCC / Prefetch esposto (a differenza di i.MX8MQ+DCSS). |
| **Si può fare?** | **No** su questo hardware/BSP, senza cambio SoC o un supporto kernel non documentato su 8MP LCDIF. |
| **Dettaglio storico** | [TEST DCC (cronologia)](#test-dcc) |

---

<a id="pm-pixel-clock"></a>

### 2 — Limitare la frequenza di clock del display (pixel clock) via kernel

**Obiettivo:** ridurre banda DDR di scanout abbassando pixel/s verso il pannello (possibile beneficio RT).

| | |
|--|--|
| **Stato** | ⏸️ **Sospeso** — diagnosi fase 0 completata; prosecuzione solo in **BSP Yocto** |
| **Dove si agisce** | Device tree + eventuale patch LDB / PLL — **non** in PegLib |
| **Fatto / testato** | Fase 0 (2026-07-10): mode unico **1024×600 @ 64,31 Hz**, pixel clock già **49,5 MHz** (`media_disp2_pix`), sotto i 74,25 MHz tipici del fixup LDB. Un solo mode → niente `modetest` alternativo senza DT. |
| **Si può fare?** | **Sì, ma solo via BSP:** nuovo `display-timings` (es. 40–45 MHz), eventuale patch `imx8mp-ldb.c`, rebuild kernel+DTB, verifica datasheet pannello. **Non** richiede ricompilazione della sola applicazione HMI. |
| **Dettaglio storico** | [TEST 7 (cronologia)](#test-7) |

---

<a id="pm-bpp"></a>

### 3 — Ridurre la profondità di colore del driver video (Bpp)

**Obiettivo:** meno byte/pixel → meno footprint framebuffer e meno banda upload/scanout.

| | |
|--|--|
| **Stato** | ✅ PEG **16 bpp**; scanout DRM **16 bpp solo in Test 6** (SDL resta **32**) |
| **Dove si agisce** | `rtos.ini` (`Bpp=`) = PEG; scanout kernel = FB DRM `RG16` via `PegDrmOutput` |
| **Fatto / testato** | SDL+KMSDRM: `fb0` a **32 bpp**. Test 6: dumb buffer + `drmModeAddFB2(DRM_FORMAT_RGB565)` → LCDIF a **16**. |
| **Si può fare?** | **Scendere sotto 16 bpp: no**. Per 16 bpp **sul driver**: path DRM diretto (non basta `Bpp=16` da solo sul path SDL). |
| **Dettaglio** | [Profondità di colore (Bpp)](#bpp-gia-fatto) · [Scanout DRM a 16 bpp](#bpp-scanout-drm-16) |

---

<a id="pm-immagini"></a>

### 4 — Ottimizzare le immagini (formati indicizzati / compressione)

**Obiettivo:** ridurre flash e RAM degli asset grafici (icone, bitmap UI) con PegBitmap 8 bpp / RLE o equivalenti compressi.

| | |
|--|--|
| **Stato** | ✅ **Già applicata in larga parte** — audit statico **2026-07-20**; campagna di conversione di massa **non giustificata** |
| **Dove si agisce** | Asset / tool di cattura PegBitmap — **non** la profondità di scanout DRM (resta RGB565) |
| **Si può fare ancora?** | Solo residui minori (icone toolbar 16 bpp RAW ≈ **27 KB** totali). Non è una leva RT/banda display. |

#### Audit PegBitmap (kvuib + pressbrakepeg, 2026-07-20)

Ambito: inizializzazioni `PegBitmap name = { flags, bpp, w, h, … }` nei sorgenti applicazione (esclusi font `pegfont/` e third_party).

| Metrica | Valore |
|---------|-------:|
| PegBitmap trovati | **832** |
| Già **8 bpp + RLE** (`BMF_RLE`) | **796** (**95,7 %**) |
| **8 bpp RAW** (no RLE) | **7** (icone 8×8 / 16×16, payload trascurabile) |
| **16 bpp RAW** | **29** (toolbar / menu editor in `UimResIco.cpp`) |
| Payload UCHAR stimato (array bitmap UI rilevanti) | **ordine ~0,3–0,5 MB** (non multi-MB) |
| Residuo convertibile 16→8 bpp (upper bound flash) | **≈ 27 KB** (29 icone) |

**Esempio efficacia RLE già in uso:** `gbBackgroundBitmap` (585×344, 8 bpp) — raw teorico ≈ **196 KB**, payload RLE ≈ **15,7 KB** (rapporto ≈ **0,08**). Frame animazione `UimResAviFrames` (272×60) tipicamente ≈ **1,7–1,8 KB** RLE vs ≈ **15,9 KB** raw.

**File sorgente più rilevanti (payload bitmap, non font):** `UimResIco.cpp`, `UimResAviFrames.cpp`, `pegmain/pimage.cpp`, `liste/bitmap.cpp`, `Flags.cpp`, `Keys.cpp`, `Logo.cpp`.

**Conclusione:** la raccomandazione “usare formati indicizzati / compressi” è **già lo standard** dello stack PEG su questa HMI. Un’ulteriore campagna di conversione recupererebbe al più **decine di KB** di flash, con rischio look (palette) e costo di ricattura non proporzionato. Priorità memoria/flash restano i **font** ([§6](#pm-font)), non le bitmap UI.

---


<a id="pm-lvgl-cache"></a>

### 5 — Ridurre la cache LVGL

**Obiettivo (tipico in HMI LVGL-native):** liberare RAM riducendo buffer/cache di disegno e immagini di LVGL (`lv_conf.h`).

| | |
|--|--|
| **Stato** | ❌ **Non pertinente** su questo stack |
| **Perché** | L’UI è disegnata da **PEG** sul framebuffer; LVGL nel path Test 6 fa essenzialmente `lv_init` + `lv_timer_handler` e **non** gestisce widget, display né PegFont. La RAM rilevante è PEG + DRM + font/asset, non la cache LVGL. |
| **Si può fare?** | Si possono ridurre parametri LVGL, ma **non** è una leva utile per flash font, banda display o RT del `COM RTC Handler`. |

---

<a id="pm-font"></a>

### 6 — Font: includere solo i caratteri (range Unicode) strettamente necessari

**Obiettivo:** minimizzare flash (e, dove i glifi sono caricati, RAM) dei font embedded / pacchetti lingua.

| | |
|--|--|
| **Stato** | 🟡 **Parziale** |
| **Fatto / testato** | Misura footprint font sull’immagine; experiment **#2 pulizia build** (`experiment/test6-with-new-font` in kvuib): rimossi `Yahei_N.cpp` morti dalle `libPegFontChs*` (runtime CHS usa già `PegFontTypeYaHeiN.gz`). **Flash Chs: 17,82 → 11,54 MB (−6,28 MB / −6 584 848 B)**. RAM path Yahei **invariata** (glifi usati restano dal `.gz`). |
| **Si può fare ancora?** | **Subset Unicode** (ricattura solo glifi usati nelle stringhe UI) → sì, massimo guadagno a lungo termine; serve PEG Font Capture + charset. **SKU EU slim** (non installare lib CJK) → sì su immagini lab EU-only. |
| **Dettaglio** | [Experiment font #2](#experiment-font-2) |

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

> **`updateMs` in questo test:** tempo cumulato in **`SDL_UpdateTexture()`** per finestra (~1 s). Vedi [tabella per test](#significato-updateMs).

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

> **`updateMs` in questo test:** identico al **Test 0** (`SDL_UpdateTexture`); cambia solo la quantità di pixel per dirty region (risoluzione minore).

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

> **`updateMs` in questo test:** identico al **Test 0** — solo diagnosi, nessun cambio al cronometro.

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

**Come si imposta lo scanout a 16 bpp (userspace → driver):** vedi [Scanout DRM a 16 bpp (Test 6)](#bpp-scanout-drm-16).

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

> **`updateMs` in questo test:** tempo cumulato in **`SDL_ConvertPixels(RGB565→ARGB8888)`** + **`SDL_UpdateTexture()`**. `req`/`reqMBps` contano byte ARGB (4 bpp), non RGB565.

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

**Stato:** ✅ RT/GUI/stabilità validati · **Macro:** `RT_DRM_DIRECT` (branch `experiment/test6-drm`) · [← Tabella](#stato-test)

---

| Campo | Valore |
|-------|--------|
| **Obiettivo** | bypassare SDL texture + GPU; una sola copia RGB565 PEG → scanout. Target: `rtc_handler_us` **< 100 µs** a 1024×600 produzione. |
| **Modifica** | macro `EMBEDDED_HMI_RT_DRM_DIRECT` in `PegLib.pro` (disattiva `EMBEDDED_HMI_RT_NATIVE_TEXTURE`). |

**File nuovi:**

- `pegdrmoutput.cpp` — probe KMS su tutti i `/dev/dri/card*`, 2 dumb buffer RGB565, `drmModeSetCrtc`, `drmModePageFlip`
- `pegdrm_evdev.cpp` — touch via `/dev/input/event*` (SDL solo `EVENTS`+`TIMER`)

**Scanout a 16 bpp sul driver `imx-drm` / `lcdifv3`:** dumb buffer `bpp=16` + `drmModeAddFB2(..., DRM_FORMAT_RGB565, …)` — dettaglio e verifica in [Scanout DRM a 16 bpp](#bpp-scanout-drm-16).

**Fix post-POC (stessa build di misura):**

- Selezione device DRM: `drmModeGetResources` su ogni card (card0 Vivante → card1 `imx-drm`)
- Touch evdev: edge detection press/release su `SYN_REPORT` (bug: solo motion, no click PEG)

> **`updateMs` in questo test:** tempo cumulato in **`PegDrmOutput::blitDirtyRegion()`** (`memcpy` PEG → dumb buffer). **Non** include `syncBackFromPeg()` né `pageFlip()` — vedi [tabella per test](#significato-updateMs) e sezione F sotto.

---

### Ripresa Test 6 (2026-07-13) — obiettivo RT < 100 µs

Contesto ripresa su branch `experiment/test6-drm`, config produzione `XRes=1024 YRes=768` + `XView=1024 YView=600`, `Bpp=16`.

#### A) Touch non funzionante dopo reboot

**Sintomo:** GUI visibile ma nessun click; `evtest /dev/input/event1` (“ESA-VMAKD”, device virtuale) non produce eventi al tocco.

**Causa:** il probe evdev sceglieva il primo device con `ABS_X/Y`, ma dopo reboot compare prima l’iniettore virtuale ESA invece del touch ILITEK (`event2`, `INPUT_PROP_DIRECT`).

**Fix:**

- selezione a punteggio (multitouch + `INPUT_PROP_DIRECT`, penalità bustype 0)
- override: `PEGDRM_TOUCH_DEV=/dev/input/event2`
- log avvio: `[RT] evdev: touch multitouch (...) da /dev/input/event2 "ILITEK ILITEK-TP"`

#### B) Ottimizzazione memcpy: damage tracking (no sync front→back)

**Problema:** a ogni present, copia integrale **front→back** (~1,5 MiB) leggendo dal dumb buffer (memoria non cachata) → CPU alta e traffico DDR.

**Fix:** damage tracking — recupero zone “stale” dal framebuffer PEG (RAM cachata) invece di leggere il dumb buffer front.

**Log avvio (una tantum):**

```text
[RT] drm: damage tracking attivo (sync integrale PEG→back se damage>=15% o blit>=6)
```

**Risultati RT osservati:**

| Fase | `rtc_handler_us` worst |
|------|------------------------:|
| Dopo damage tracking | ~108 µs |
| Ulteriori misure | ~102 µs |
| Con affinità CPU corretta (Lnk su CPU3) | **~98 µs** ✅ |
| Stress test “grafico colorato” (punto critico) | **99 µs** ✅ |
| Stress prolungato (2026-07-13 pomeriggio) | **102 µs** worst |
| **Campagna ~30 min** (scroll grafico + navigazione, 2026-07-13 sera) | **105 µs** worst · `count_ge_100` = **2** / **432 000** attivazioni |
| **Campagna endurance ~4 h** (2026-07-15, build **8ª**) | **102 µs** worst · `count_ge_100` = **1** / **~4 152 000** attivazioni · **0 crash** |
| **Sessione Calculation** (zoom/dezoom sim2d, 2026-07-14) | **109 µs** worst · `count_ge_100` = **3** / **1 068 000** attivazioni |

> `ps` mostrava `Lnk` non pinnato (migrava tra CPU 1/2). Pin esplicito consigliato per misure RT stabili.

#### C) Artefatti “linee orizzontali” su grafico colorato + spike RT

**Sintomo:** scroll su grafico 3D colorato → linee orizzontali a schermo; RT fino a ~150 µs.

**Causa:** scroll pesante (`calls` ~50–65, `maxRectPx` ~487k, `reqMBps` ~32–48) — bounding box stale insufficiente prima del pageflip.

**Mitigazione:** sync ibrido pre-flip — se damage ≥ **15%** schermo **o** blit ≥ **6** dal flip precedente → copia integrale PEG→back prima di `drmModePageFlip`. Il sync avviene in silenzio (nessun log per frame).

**Esito:** linee orizzontali **risolte**; RT al punto critico ~**105 µs** (mancano ~5 µs al target).

#### D) Rettangoli bianchi in scroll (2026-07-13) — ✅ risolto

**Sintomo (iniziale):** durante scroll verso il basso sul grafico colorato comparivano piccoli **rettangoli bianchi** ai bordi delle forme.

**Causa probabile:** race tra thread **PegRefresh** (disegna su `g_pyBitmap`) e main loop — snapshot DRM prima che PEG avesse finito di dipingere la regione appena esposta.

**Mitigazione (codice PegLib):**

- drain coda dirty (fino a 8 `processPendingUpdates` consecutivi) prima del `pageFlip`
- **sync integrale PEG→back su ogni pageflip**
- buffer PEG a `XRes×YRes` + `LOCK_PEG` su memcpy framebuffer

**Esito validazione (2026-07-13, pomeriggio):** rettangoli bianchi **non si ripresentano più**, incluso scroll rapidissimo sul grafico colorato. ✅

#### E) CPU e banda (misure 2026-07-13)

**`top` — campagna ~30 min (2026-07-13 sera):**

| Scenario | %CPU PegExec | %CPU Lnk | %MEM PegExec | %MEM Lnk | Note |
|----------|-------------:|---------:|-------------:|---------:|------|
| Interfaccia ferma (no scroll) | **4,0%** | **19,2%** | 27,5% | 20,2% | entrambi in stato `S` |
| Scroll grafico sotto stress | **30,5%** | **21,9%** | 27,5% | 20,2% | Δ PegExec **+26,5 pp** |
| Δ scroll − idle (`PegExec`) | +26,5 pp | +2,7 pp | — | — | pattern coerente con Test 0 |

**`uploadDirtyRegion` sotto stress scroll (stessa sessione):**

| Regime | calls | req | reqMBps | updateMs | effMBps | maxRectPx |
|--------|------:|----:|--------:|---------:|--------:|----------:|
| Scroll pesante (burst) | 72–105 | 43,8–50,0 MB | 43,3–49,4 | **60,7–70,0** | 699–736 | **487 656** |
| Scroll leggero / frame parziali | 10–13 | 3,81–5,08 MB | 3,78–5,01 | **11,9–15,7** | 299–349 | 222 384 (picchi 292k–300k) |

Esempi log (scroll pesante):

```text
[RT] uploadDirtyRegion: calls=105 req=49.98MB reqMBps=49.44 updateMs=69.955 effMBps=735.7 maxRectPx=487656
[RT] uploadDirtyRegion: calls=72  req=43.83MB reqMBps=43.30 updateMs=60.739 effMBps=699.2 maxRectPx=487656
```

Esempi log (scroll leggero):

```text
[RT] uploadDirtyRegion: calls=13 req=5.08MB reqMBps=5.01 updateMs=15.730 effMBps=349.4 maxRectPx=222384
[RT] uploadDirtyRegion: calls=10 req=3.81MB reqMBps=3.78 updateMs=11.930 effMBps=299.1 maxRectPx=222384
```

GPU non usata (`gc/meminfo` stabile); carico su CPU memcpy + pageflip DRM.

<a id="banda-ddr-perf"></a>

#### E2) Banda DDR da `perf` — riposo vs scroll grafico (2026-07-20)

Misura **sistemica** del traffico memoria DDR (tutti i master: CPU, LCDIF/scanout, GPU, …), complementare a `reqMBps` / `effMBps` di `uploadDirtyRegion` (che contano solo la copia GUI PEG→DRM).

##### Comando

```bash
perf stat -a -I 100 \
  -e imx8_ddr0/read-cycles/,imx8_ddr0/write-cycles/ \
  sleep 10 > /tmp/valori_perf.txt
```

| Opzione | Ruolo |
|---------|--------|
| `-a` | tutto il sistema (non un solo processo) |
| `-I 100` | campione ogni **100 ms** |
| `read-cycles` / `write-cycles` | cicli in cui il controller DDR è occupato in lettura / scrittura |
| Output | file `valori_perf.txt` (stessa cartella di questo registro) |

##### Metriche usate

| Evento `perf` | Cosa conta | Direzione |
|---------------|------------|-----------|
| `imx8_ddr0/read-cycles/` | cicli di trasferimento in **lettura** dal DRAM | CPU/cache refill, **scanout display** (LCDIF legge il framebuffer), DMA, … |
| `imx8_ddr0/write-cycles/` | cicli di trasferimento in **scrittura** verso il DRAM | write-back cache, **memcpy PEG→dumb buffer**, pageflip, … |

Non sono contatori di “byte puri”: sono cicli del PMU DDR i.MX8. Per ottenerne una **banda in MB/s** serve un fattore di conversione documentato da NXP / `perf` vendor events.

##### Perché il calcolo è fatto così

Sui SoC i.MX8M il PMU DDR espone `read-cycles` / `write-cycles`. Nelle metriche ufficiali Linux/`perf` per la famiglia i.MX8M (es. i.MX8MN/MQ, stesso modello di contatore) i byte stimati sono:

```text
byte = cycles × 4 × 2 = cycles × 8
```

| Fattore | Significato tipico |
|--------:|--------------------|
| **×4** | 32 bit di bus dati → **4 byte** per beat |
| **×2** | DDR (double data rate): trasferimento su entrambi i fronti del clock |

Quindi, per ogni intervallo di campionamento Δt (qui ≈ 0,1 s):

```text
MB/s_read  = (read_cycles  × 8) / (Δt × 1_000_000)
MB/s_write = (write_cycles × 8) / (Δt × 1_000_000)

MB/s_totale = MB/s_read + MB/s_write
```

Esempio: `1_064_521` read-cycles in `0,100` s → `(1_064_521 × 8) / 0,100 / 1e6` ≈ **85 MB/s** in lettura.

> **MB/s** = megabyte/s (10⁶). Se servissero megabit/s: ×8. Su i.MX8MP esistono anche eventi `axid-read` / `axid-write` già in byte; qui si è usato il comando a `*-cycles` già raccolto.

##### Risultati (Test 6, target avn8mp, 2026-07-20)

Due run da ~10 s / 100 campioni (`-I 100`), stesso fattore ×8.  
File storici: riposo + scroll raccolti in `valori_perf.txt` (sessioni separate, 2026-07-20).

Per ogni intervallo da 0,1 s si calcola `MB/s_read`, `MB/s_write` e `MB/s_totale = read + write`. Poi, **separatamente** su ciascuna delle tre serie da ~100 punti: media, mediana, p95, min e max.

| Serie (MB/s) | Stat | Riposo (GUI ferma) | Scroll grafico | Δ scroll − riposo |
|--------------|------|-------------------:|---------------:|------------------:|
| **Read** | medio | **85** | **204** | **+119** (~2,4×) |
| | mediana | 79 | 209 | — |
| | p95 | 105 | 225 | — |
| | min … max | 68 … 181 | 79 … 232 | — |
| **Write** | medio | **18** | **124** | **+106** (~7×) |
| | mediana | 10 | 128 | — |
| | p95 | 35 | 144 | — |
| | min … max | 7 … 65 | 10 … 148 | — |
| **Totale** (R+W) | medio | **104** | **328** | **+224** (~3,2×) |
| | mediana | 89 | 337 | — |
| | p95 | 141 | 369 | — |
| | min … max | 75 … 247 | 89 … **380** | picco totale scroll **~380** |

> **min … max** di una riga = estremi di **quella** serie (solo read, solo write, o totale), non mischiati. Es. Write scroll 10…148 ≠ Totale scroll 89…380.

**Lettura operativa (Test 6):**

- A riposo resta una banda non nulla (~100 MB/s totali): soprattutto **letture di scanout** del display + idle di sistema (write bassa, max ~65 MB/s).
- In scroll sale soprattutto la **write** (~7× in media; max ~148 MB/s): coerente con burst di `memcpy` PEG→dumb buffer / upload dirty region.
- La **read** sale ~2,4× (media 85→204; max ~232): più traffico CPU sul framebuffer + scanout continuo.
- Il picco **totale** ~380 MB/s è R+W nello stesso intervallo da 0,1 s, non il max read sommato al max write (che sarebbero istanti diversi).
- Ordine di grandezza (~0,3 GB/s medi in scroll) su un bus LPDDR4 tipico ~12,8 GB/s peak → **utilizzo basso** in assoluto, ma il Δ vs riposo è la leva rilevante per interferenza RT (contendere la stessa DDR del task su CPU3).

##### Risultati (Test 0 / branch `lvgl-hmi`, path SDL, 2026-07-21)

Stessa formula (`cycles × 8 / Δt`), stessi ~10 s / 100 campioni.

| File | Scenario |
|------|----------|
| `valori_perf_no_iterazioni` | riposo (GUI ferma) |
| `valori_perf.txt` | scroll grafico |

| Serie (MB/s) | Stat | Riposo (GUI ferma) | Scroll grafico | Δ scroll − riposo |
|--------------|------|-------------------:|---------------:|------------------:|
| **Read** | medio | **139** | **315** | **+176** (~2,3×) |
| | mediana | 118 | 309 | — |
| | p95 | 176 | 345 | — |
| | min … max | 108 … 224 | 294 … 411 | — |
| **Write** | medio | **32** | **198** | **+165** (~6,1×) |
| | mediana | 10 | 188 | — |
| | p95 | 61 | 229 | — |
| | min … max | 7 … 107 | 181 … 281 | — |
| **Totale** (R+W) | medio | **171** | **513** | **+341** (~3,0×) |
| | mediana | 129 | 497 | — |
| | p95 | 238 | 576 | — |
| | min … max | 116 … 330 | 475 … **692** | picco totale scroll **~692** |

##### Confronto Test 0 (SDL) vs Test 6 (DRM) — stessi scenari

| Metrica (medio, MB/s) | Test 0 SDL | Test 6 DRM | Δ Test6 − Test0 |
|-----------------------|----------:|----------:|----------------:|
| Read riposo | 139 | 85 | **−54** |
| Write riposo | 32 | 18 | **−14** |
| **Totale riposo** | **171** | **104** | **−67** (~−39%) |
| Read scroll | 315 | 204 | **−111** |
| Write scroll | 198 | 124 | **−74** |
| **Totale scroll** | **513** | **328** | **−185** (~−36%) |
| Picco totale scroll | ~692 | ~380 | **−312** |

**Lettura operativa del confronto:**

- Su **Test 0 (SDL)** la DDR totale in scroll è ~**513 MB/s** medi (picco ~**692**), vs ~**328** / picco ~**380** su Test 6.
- Il calo su Test 6 (~**−36%** in scroll) è coerente con l’eliminazione della catena texture SDL / present GLES: meno copie e meno traffico bus a parità di gesto.
- Anche a **riposo** Test 6 è più basso (~104 vs ~171): meno overhead del path SDL idle.
- La **write** resta la leva che esplode di più in scroll su entrambi (~6–7×), ma il livello assoluto su Test 0 è più alto (198 vs 124 medi).

##### Relazione con le metriche GUI `[RT]`

| Metrica | Ambito | Cosa misura |
|---------|--------|-------------|
| `reqMBps` / `effMBps` | solo path `uploadDirtyRegion` | byte della copia dirty PEG→output |
| `imx8_ddr0/*-cycles` → MB/s | **tutto** il chip | somma di tutti i master sulla DDR |

Quindi: `reqMBps` ≈ 24–50 MB/s in scroll (solo GUI) può coesistere con **~330 MB/s** DDR totali (GUI + scanout + resto). Non devono coincidere numericamente.

#### F) Confronto `[RT] uploadDirtyRegion` — Test 0 vs Test 6 (2026-07-14)

> Confronto a parità di config **1024×600 viewport**, `Bpp=16`, stesso scenario (idle vs scroll grafico). Test 0 = SDL+texture+GLES; Test 6 = DRM dumb RGB565 diretto.

**Idle / interfaccia ferma (no scroll):**

| Metrica | Test 0 (baseline) | Test 6 (2026-07-14) | Δ / note |
|---------|------------------:|----------------------:|----------|
| `calls/s` | 7–9 | **10–12** | simile (refresh periodico PEG) |
| `reqMBps` | 0,75–1,0 | **4,1–5,3** | Test 6 più alto (dirty leggermente più grandi) |
| `updateMs/s` | 22–30 | **11–15** | **≈ −50%** — upload più veloce |
| `effMBps` | 33–36 | **356–373** | **≈ ×10** — memcpy diretto vs SDL |
| `maxRectPx` | ~66 912 | **237 472–337 022** | area dirty idle maggiore in Test 6 |

**Esempi log idle Test 6 (2026-07-14):**

```text
[RT] uploadDirtyRegion: calls=11 req=4.61MB reqMBps=4.57 updateMs=12.587 effMBps=371.9 maxRectPx=237472
[RT] uploadDirtyRegion: calls=10 req=4.15MB reqMBps=4.14 updateMs=11.334 effMBps=366.5 maxRectPx=237472
```

**Scroll grafico (stress):**

| Metrica | Test 0 (baseline) | Test 6 (2026-07-14) | Δ / note |
|---------|------------------:|----------------------:|----------|
| `calls/s` | **~33** | **76–84** | **×2,4** — più upload/s (pipeline più veloce) |
| `reqMBps` | **~24** | **48–50** | **×2** — più byte copiati al secondo |
| `updateMs/s` | **208–220** | **60–64** | **≈ −70%** — meno tempo in upload |
| `effMBps` | **112–117** | **776–823** | **≈ ×7** — throughput memcpy molto più alto |
| `maxRectPx` | 485 051 (~79%) | **487 656** (~79%) | area simile |

**Esempi log scroll Test 0:**

```text
[RT] uploadDirtyRegion: calls=33 req=24.74MB reqMBps=24.09 updateMs=216.366 effMBps=114.4 maxRectPx=485051
```

**Esempi log scroll Test 6 (2026-07-14):**

```text
[RT] uploadDirtyRegion: calls=84 req=50.16MB reqMBps=49.63 updateMs=63.97 effMBps=784.1 maxRectPx=487656
[RT] uploadDirtyRegion: calls=76 req=48.61MB reqMBps=48.10 updateMs=59.67 effMBps=814.6 maxRectPx=487656
```

**Interpretazione:**

| Asse | Test 0 | Test 6 | Conclusione |
|------|--------|--------|-------------|
| **Velocità upload** (`effMBps`, `updateMs`) | lento (~115 MB/s eff.) | **molto più veloce** (~800 MB/s eff.) | ✅ beneficio netto Test 6 |
| **Volume al secondo** (`reqMBps`, `calls/s`) | ~24 MB/s, 33 call/s | **~49 MB/s, 80 call/s** | Test 6 fa **più lavoro totale**/s |
| **CPU `top` scroll** | ~30,4% | ~30,5% | simile — il risparmio per upload è **assorbito** da più frame/s + redraw PEG + sync pre-flip |

> Il Test 6 **non** riduce il carico CPU in scroll perché la pipeline più veloce permette **più cicli upload/s** e copia **più byte/s**. Il guadagno si vede in **`updateMs`** (meno tempo bloccato in copia) e **`effMBps`** (memcpy 7× più efficiente), non nel `%CPU` medio di `top`. **`updateMs` Test 6 esclude `syncBackFromPeg()`** — vedi [Significato di `updateMs`](#significato-updateMs).

#### G) Affinità CPU / isolcpus

Obiettivo configurazione: **Lnk su CPU 3** con `isolcpus=3`. Verifiche:

```bash
cat /proc/cmdline | tr ' ' '\n' | grep -i isol
cat /sys/devices/system/cpu/isolated
taskset -cp $(pidof Lnk)
taskset -c 3 /path/to/Lnk          # test manuale
taskset -cp 0,1,2 $(pidof PegExec)
```

#### H) Segfault in validazione (2026-07-13) — causa identificata in `libcad2d`

**Sintomi riportati sul target** (`zsh: segmentation fault (core dumped) ./PegExec`):

| # | Scenario | Note log pre-crash |
|---|----------|-------------------|
| 1 | Primo click su bottone | — |
| 2 | Aggiunta linee al grafico | burst `uploadDirtyRegion` fino a `calls=112`, `updateMs≈70` |
| 3 | Dopo fix buffer PegLib | crash con carico moderato (`calls=15`, `updateMs≈11`) |

**Backtrace simbolizzato** (`peg_crashdiag` + addr2line su build avn8mp, 2026-07-13):

```text
PegRefreshDaemon → EtsGUIThread → PegPresentationManager::Execute
  → CPezzoFormLAlpha::Message(PegMessage const&)
  → CPPGBaseDocument::UpdateAllViews()
  → CPezzoForm::EditDraw()
  → CRecGenerico::GetCodiceRecord() const   ← CRASH
```

> **Non è nel path DRM/Test 6.** Crash nel thread GUI PEG, in **`libcad2d.so`** (`CPezzoForm::EditDraw`), non in `uploadDirtyRegion` / `pageFlip`.

**Causa diretta (aggiornamento 2026-07-13):** due problemi in `libcad2d`:

1. **Form doppia:** con modalità **L,alpha** attiva, `UpdateAllViews()` aggiorna sia `CPezzoFormLAlpha` sia la form classica `CPezzoForm`. La form classica chiama `EditDraw()` con `RecGrafPtr()` **cache stale** (puntatore non NULL ma invalido) → crash in `GetCodiceRecord()`.
2. **Null-check insufficienti** su `RecGrafPtrAt(n±1)` in `EditDraw()` (fix precedente incompleto).

**Fix libcad2d (2ª iterazione):**

| File | Fix |
|------|-----|
| `PezzoForm.cpp` | `OnUpdate()`: salta `EditDraw()` classico se esiste `GetForm_LAlpha()` |
| `PezzoForm.cpp` | `EditDraw()`: usa `RecGrafPtrAt()` con bounds-check invece di `RecGrafPtr()` cache |
| `PezzoFormLAlpha.cpp` | `EditDraw()`: null-check record precedente |
| `Ppgdoc.cpp` | `UpdateRecGrafPtr()`: bounds-check su `m_nStepAttivo` vs `NumeroElementi()` |

**Fix libcad2d (3ª iterazione — percorso mouse / aggiunta linee):**

| File | Fix |
|------|-----|
| `Pezzoview.cpp` | `InizioSequenza()`: bounds-check + `RecGrafPtrAt()` (crash su `GetFirstParam()` con cache stale) |
| `Pezzoview.cpp` | `OnLButtonUp` / `SelezionatoTratto`: accesso sicuro al record corrente |
| `PezzoFrame.cpp` | `OnPrev()`: `ID_PREV` invece di `PK_F9` (PK_F9 non gestito da `TastoGestitoDaFieldUpdate`) |

**Fix precedente (1ª iterazione):** null-check su `RecGrafPtrAt(n+1)` in `PezzoForm::EditDraw` — necessario ma non sufficiente.

```bash
# ricompilare cad2d, poi:
cp libcad2d.so* /opt/Squeeze/
```

**Fix PegLib (utili, causa separata):** buffer `XRes×YRes`, `LOCK_PEG`, `peg_crashdiag` — vedi tabella sotto.

**Tentativo debug core dump:** `gdb ./PegExec core` → **`core: No such file or directory`** in `/opt/Squeeze/`. Su Yocto/systemd il core finisce spesso in `systemd-coredump`, non nella cwd:

```bash
cat /proc/sys/kernel/core_pattern
coredumpctl list | grep -i peg
coredumpctl gdb PegExec    # dentro gdb: bt full
# oppure forzare core locale:
ulimit -c unlimited
echo 'core.%e.%p' | tee /proc/sys/kernel/core_pattern   # richiede root
```

**Causa probabile #1 — buffer PEG troppo piccolo (confermata):**

Con `XRes=1024` `YRes=768` e `XView=1024` `YView=600`, il driver DIB16 usa pitch **1024×768** (`_gBitmap->wWidth/wHeight` da `video->xd/yd`), ma `createSurface()` allocava solo **1024×600** → scritture PEG oltre la riga 599 **corrompevano l’heap** → segfault intermittente (click, redraw grafico).

**Fix applicato (2026-07-13):**

| Fix | File | Descrizione |
|-----|------|-------------|
| Buffer PEG a `XRes×YRes` | `peg_run.cpp` | `createSurface(_xres, _yres)`; display DRM resta `_xview×_yview` |
| Pitch corretto | `peglvglwindow.cpp` | `m_fbWidth` / `framePitchBytes()` per memcpy; clip dirty a `m_width/m_height` (viewport) |
| Riga slack +1 | `peglvglwindow.cpp` | alloc `(_yres+1)` righe come path embedded legacy (`EM_YRES+1`) |
| Race framebuffer | `peglvglwindow.cpp` | `LOCK_PEG` (`PEG_PresentationCriticalSection`) durante `blitDirtyRegion` / `syncBackFromPeg` |
| Teardown DRM | `pegdrmoutput.cpp` | `m_alive` nel callback page_flip; `waitFlipComplete()` prima di chiudere DRM |
| Backtrace senza core | `peg_crashdiag.cpp` | handler `SIGSEGV`/`SIGABRT`/`SIGBUS` + **`__stack_chk_fail`** → stack su stderr |

**Log avvio attesi dopo deploy fix:**

```text
[RT] crashdiag: handler SIGSEGV/SIGABRT/SIGBUS + __stack_chk_fail attivo
[RT] drm_direct: Opzione D — output DRM dumb RGB565, SDL solo eventi
[RT] PegLib build Jul 13 2026 12:xx:xx
[RT] rtos.ini XRes=1024 YRes=768 → framebuffer PEG 1024x768 @ 16 bpp = 1.50 MiB, display 1024x600 = 1.17 MiB
[RT] viewport attivo: PEG disegna su buffer 1024x768, scanout DRM copia le prime 600 righe visibili
```

**Al prossimo crash:** copiare tutto da `[RT] FATAL SIGSEGV` in giù (backtrace automatico).

**Stato:** ✅ risolto (2026-07-13) — `libcad2d.so` 3ª build deployata; campagna **~30 min** senza crash, **incluso aggiunta linee al grafico** (percorso `InizioSequenza` / `OnLButtonUp` / `OnPrev`→`ID_PREV`). Vedi [crash Calculation 2026-07-14](#test-6-crash-4) (4ª build).

#### I) Varianza RT sotto stress prolungato — aggiornamento 2026-07-13 sera

Misure precedenti avevano mostrato picchi fino a **~107 µs** (`bus_access≈18539`). Con build PegLib aggiornata (buffer `XRes×YRes`, drain dirty, sync integrale pre-flip, `LOCK_PEG`) e **`Lnk` su CPU3`, campagna **~30 min** (scroll grafico, navigazione schermate):

| Metrica | Valore |
|---------|--------|
| `rtc_handler_us` worst (sessione) | **105 µs** |
| Attivazioni totali (fine sessione) | **432 000** |
| Attivazioni con `rtc_handler_us` **> 100 µs** | **2** (≈ **0,0005%**) |
| Obiettivo | < 100 µs (2 spike su 432k — **accettabile** per produzione) |

**Peggior iterazione** (CPU3, `[WORST rtc_handler_us]`, iter **380 793**):

| Contatore | Valore |
|-----------|--------|
| `rtc_handler_us` | **105 µs** |
| L2 miss | **31,55%** (`l2d_cache_refill` / `l2d_cache`) |
| `bus_access` | 26 258 |
| `bus_cycles` | 872 366 |
| `cpu_cycles` | 1 740 115 |
| Istruzioni | 657 115 |
| IPC | **0,378** |
| CPI | **2,648** |

Verificare sempre `taskset -cp $(pidof Lnk)` → mask attesa `0x8`.

**Aggiornamento sessione 2026-07-14** (pagina **Calculation** / sim2d, zoom-dezoom + bottoni grafico):

| Metrica | Valore |
|---------|--------|
| `nanosleep` min | **11 µs** |
| `nanosleep` max | **109 µs** |
| Attivazioni totali | **1 068 000** |
| Attivazioni **> 100 µs** | **3** (≈ **0,0003%**) |
| Trigger worst case (percepito) | **dezoom** sul grafico sim2d (ridisegno pesante) |

**Peggior iterazione** (CPU3, `[WORST rtc_handler_us]`, iter **866 782**):

| Contatore | Valore | Confronto vs iter 105 µs (2026-07-13) |
|-----------|--------|----------------------------------------|
| `rtc_handler_us` | **109 µs** | +4 µs |
| L2 miss | **29,21%** | −2,3 pp |
| `bus_access` | 16 040 | −39% |
| `bus_cycles` | 879 513 | simile |
| Istruzioni | 210 404 | −68% |
| IPC | **0,120** | **molto più basso** (0,38) |
| CPI | **8,34** | **molto più alto** (2,65) |

> Il picco **109 µs** resta **accettabile** (3 spike su 1M). L’IPC **0,12** e il CPI **8,3** sulla worst iter indicano **stall memoria/cache** durante il redraw sim2d al dezoom — coerente con carico GUI, non regressione del path DRM.

#### J) Segfault pagina Calculation / sim2d (2026-07-14) — fix 4ª build `libcad2d`

<a id="test-6-crash-4"></a>

**Sintomo:** `SIGSEGV` durante zoom/dezoom e pressione bottoni sulla pagina **Calculation** (grafico sim2d pressa, vedi screenshot sessione).

**Log stderr:**

```text
[RT] FATAL SIGSEGV (11) — backtrace (11 frame):
/opt/Squeeze/libPegLib.so.1(+0xba780)
linux-vdso.so.1(__kernel_rt_sigreturn+0x0)
/opt/Squeeze/./libcad2d.so.1(+0x71ef4)
/opt/Squeeze/./libsim2d.so.1(+0x18ad4)
/opt/Squeeze/./libsim2d.so.1(+0x1e65c)
/opt/Squeeze/libPegDesktop.so.1(_ZN12CDeskToolBar7MessageERK10PegMessage+0xec)
/opt/Squeeze/libPegLib.so.1(_ZN22PegPresentationManager7ExecuteEv+0x94)
...
zsh: segmentation fault (core dumped)  ./PegExec
```

**Backtrace simbolizzato** (offset su `libcad2d.so.1.0.0` / `libsim2d.so` build avn8mp):

```text
CDeskToolBar::Message
  → CSim2DFrame::Message          (IDC_PIEGA / tasto «Bend» o toolbar)
    → CSim2DFrame::OnPiega()
      → RefreshCad2DForOtt(iActSez)
        → CDraftPieceWnd::RefreshView(short)   ← CRASH (+0x34 in funzione)
```

> **Percorso diverso** dai crash 2026-07-13 (`CPezzoForm::EditDraw` / `InizioSequenza` / `Pezzoview`). Qui il crash è nella **finestra CAD draft** (`CDraftPieceWnd`) aggiornata da sim2d durante **Piega**, non nel disegno diretto del grafico sim2d al dezoom. Il dezoom ha probabilmente causato il **picco RT**; il **segfault** è scattato in parallelo o subito dopo su azione **Piega** + refresh CAD.

**Causa diretta:**

| # | Problema | File / funzione |
|---|----------|-----------------|
| 1 | `RefreshView()` dereferenzia `m_pPezzoFrame` / `pDoc` **senza null-check** | `DraftPieceWnd.cpp` |
| 2 | `InstallPagCad2D()` può restituire `NULL` → `Add(NULL)` | `DraftPieceWnd.cpp` ctor |
| 3 | `SetGrafView()` accede a `m_pFile` / `pVista` senza guard | `PezzoDoc.cpp` |
| 4 | `g_pDraftPieceWnd` non azzerato alla distruzione → rischio use-after-free | `StdAfx.cpp` / `DraftPieceWnd` |

**Fix libcad2d (4ª iterazione — 2026-07-14):**

| File | Fix |
|------|-----|
| `DraftPieceWnd.cpp` | Null-check su `m_pPezzoFrame`, `pForm`, `pDoc` prima di `SetGrafView` |
| `DraftPieceWnd.cpp` | Verifica ritorno `SetGrafView != SUCCESS_PPG`; skip titolo se `m_pTitle` null |
| `DraftPieceWnd.cpp` | `Add(m_pPezzoFrame)` solo se `InstallPagCad2D` ≠ NULL; init `m_pPezzoFrame = NULL` |
| `DraftPieceWnd.cpp` | Distruttore `~CDraftPieceWnd()`: se `g_pDraftPieceWnd == this` → `NULL` |
| `DraftPieceWnd.h` | Dichiarazione distruttore |
| `PezzoDoc.cpp` | Guard `m_pFile`; null-check `pVista`; bounds `nView < 0` |

```bash
# ricompilare cad2d (4ª build), poi:
cp libcad2d.so* /opt/Squeeze/
md5sum /opt/Squeeze/libcad2d.so.1.0.0
```

**Test di validazione post-deploy (obbligatorio):**

1. Aprire pagina **Calculation** (sim2d)
2. Zoom / dezoom ripetuti sul grafico
3. Durante redraw, premere **Bend**, **Simulate**, **Rotate**, navigazione toolbar
4. Se presente finestra CAD draft (stato macchina IMP): spostarla e ripetere **Piega**
5. Campagna **≥ 15 min** con monitor RT (`nanosleep` max, `count_ge_100`)

**Stato:** ⏳ fix 5ª–7ª build in sorgente — **7ª validata** sul target (Calculate 40/40 OK, 2026-07-14).

**Crash 5ª iterazione — Calculate → chiusura pagina CAD (2026-07-14):**

| Campo | Dettaglio |
|-------|-----------|
| **Trigger** | Grafico pezzo al limite pieghe → **Calculate** → cambio pagina Sim2D |
| **Backtrace** | `TestChiudiPagCad2D` → `CString::CString(const CString&)` |
| **Causa** | `pDoc->m_pRecG->GetDirMatrice()` senza verificare `m_pRecG` valido |
| **Fix** | `StdAfx.cpp`: `SetRecGrafNum`/`RecGrafPtr()` + fallback primo record grafico |
| **Nota build** | `UpdateRecGrafPtr()` è **protected** — non chiamabile da `StdAfx.cpp` |

**Crash 6ª iterazione — Calculate con 40/40 elementi (2026-07-14) — ipotesi iniziale (parziale):**

| Campo | Dettaglio |
|-------|-----------|
| **Trigger** | Step **40/40**, errore **10008** (41ª linea rifiutata) → **Calculate** |
| **Sintomo** | `*** stack smashing detected ***` + SIGSEGV |
| **Ipotesi iniziale** | `LookForBends` / `prof_lin[MAX_GBEND]` — accesso oltre indice 39 |
| **Fix 6ª rev** | `sez = min(SezioniCadGrafico, MAX_GBEND)` in catena ottimizzatore; clamp in `GetDatiOttPezzo` — **necessari ma non sufficienti** |

**Crash 7ª iterazione — Calculate 40/40 — causa reale (2026-07-14) ✅ risolto:**

| Campo | Dettaglio |
|-------|-----------|
| **Trigger** | Stesso: **40/40** pieghe → **Calculate** → apertura pagina Sim2D |
| **Diagnostica** | Macro `CAD_DIAG` in `CommonConst.h` — log `[CAD] diag` su stderr con `fflush` |
| **Ultima riga prima crash (6ª build)** | `[CAD] diag OnSim2DCalcola: after CalcolaNewSituaz` |
| **Causa reale** | **`CSim2DView::PolyLinePez()`** — con 40 pieghe `i_pezzint.index_max ≈ 80` → ~**82 vertici** (profilo interno + esterno) scritti in **`pez_temp[MAX_ELEM_PERM]`** e **`rgnpez[MAX_ELEM_PERM]`** (`MAX_ELEM_PERM = MAX_GBEND×2 = **80**`, indici **0..79**) → **stack smashing** al primo redraw Sim2D |
| **Evidenza** | `LookForBends` con `ix_org=78` coerente con `index_max=80`; `CalcolaNewSituaz` **completa** prima del crash |
| **Fix 7ª** | `Sim2DView.cpp`: clamp `j` in `PolyLinePez`, cap `npezline`; `DisegnaPezzo`: mirror loop su `npezline` clampato; `Ottutens.cpp`: guard `MAX_POLI` in loop xmax di `LookForBends`; `Ottinit.cpp`: clamp `InitDatiSezione` (ancora aperto in 6ª) |
| **Esito validazione** | ✅ Calculate 40/40 **senza crash** (2026-07-14) |

**Log diagnostici esempio (percorso Calculate OK, 7ª build):**

```text
[CAD] diag OnCalcola: enter
[CAD] diag OnCalcola: elem=40 graf=40 recG=39 MAX_GBEND=40
[CAD] diag GetDatiOttPezzo: view=0 raw=40 sez=40 nElem=0
[CAD] diag OnSim2DCalcola: InitCompilatore mod=0 iActSez=1 iNumSect=1 iActPiega=0
[CAD] diag InitSolEsist: sez=0 nPieghe=40 passi_seq=39 offs=0 sezRaw=40
[CAD] diag linearizza: npini=0 npieghe=40 MAX_GBEND=40
[CAD] diag CalcolaNewSituaz: nsect=1 piega=1 passi_seq=39
[CAD] diag LookForBends: nsect=1 piega=1 rot=-1 sez=40 start=0 ix_org=78 sezRaw=40
[CAD] diag OnSim2DCalcola: after CalcolaNewSituaz
[CAD] diag PolyLinePez: index_max int=80 est=80 MAX_ELEM_PERM=80
[CAD] diag PolyLinePez: WARN clamp esterno j=80
[CAD] diag PolyLinePez: npezline=80
```

**Punti trace `[CAD] diag`:**

| Tag | File | Momento |
|-----|------|---------|
| `OnCalcola` | `PezzoFrame.cpp` | enter, conteggi, `GetInfoOttimizzatore`, `CHANGE_PAG` |
| `GetInfoOtt` / `GetDatiOttPezzo` | `Ppgdoc.cpp` | export dati ottimizzatore per vista |
| `TestChiudiPagCad2D` | `StdAfx.cpp` | chiusura pagina CAD su Calculate |
| `OnSim2DCalcola` | `Sim2DExport.cpp` | `InitCompilatore`, `CalcolaNewSituaz` |
| `InitCompilatore` / `InitSolEsist` / `InitDatiSez` | `Ottinit.cpp` | init sezione e pieghe |
| `linearizza` | `Ottpunti.cpp` | `npini`, `npieghe` |
| `LookForBends` | `Ottutens.cpp` | `ix_org`, `sez`, `passi_seq` |
| `CalcolaNewSituaz` | `Ottcomp.cpp` | prima del redraw macchina |
| `PolyLinePez` | `Sim2DView.cpp` | **punto critico** — `index_max` vs `MAX_ELEM_PERM` |

<a id="cad-diag-build"></a>

#### Diagnostica `[CAD] diag` — default off e riabilitazione build

Macro definita in `pressbrakepeg/IncPPG/CommonConst.h`:

```cpp
#ifndef CAD_DIAG_CALCULATE
#define CAD_DIAG_CALCULATE 0    // default: nessun log su stderr
#endif
```

| Stato | Comportamento |
|-------|----------------|
| **`CAD_DIAG_CALCULATE=0`** (default dal 2026-07-14) | `CAD_DIAG(...)` compilato a no-op — **stderr pulito** in produzione |
| **`CAD_DIAG_CALCULATE=1`** | ogni chiamata stampa `[CAD] diag …` su stderr con `fflush` |

> **Attenzione:** con l’ottimizzatore in esecuzione (`Optimization in progress`, dialog STOP/Continue) i log possono essere **continui e molto numerosi** — l’ottimizzatore richiama in loop `CalcolaNewSituaz`, `LookForBends`, redraw Sim2D a ogni piega provata. Usare solo per debug mirato, non in campagne RT lunghe.

**Per riabilitare il trace in futuro**, aggiungere `DEFINES += CAD_DIAG_CALCULATE` nei `.pro` delle **tre** librerie che contengono i punti trace (tutte includono `CommonConst.h`):

| File `.pro` | Libreria output |
|-------------|-----------------|
| `pressbrakepeg/cad2d/cad2d.pro` | `libcad2d.so` |
| `pressbrakepeg/sim2d/sim2d.pro` | `libsim2d.so` |
| `pressbrakepeg/ottimizzatore/ottimizzatore.pro` | `libottimizzatore.so` |

Esempio — in ciascuno dei tre `.pro`, nella sezione `DEFINES` in testa al file (accanto a `CAD2D_LIBRARY` / `SIM2D_LIBRARY` / `OTT_LIBRARY`):

```qmake
# Trace stderr percorso Calculate / ottimizzatore (debug only — molto verbose!)
DEFINES += CAD_DIAG_CALCULATE
```

**Build e deploy dopo modifica:**

```bash
# ricompilare cad2d + sim2d + ottimizzatore (qmake/make o script build avn8mp)
cp libcad2d.so* libsim2d.so* libottimizzatore.so* /opt/Squeeze/

# verifica trace attivo:
strings /opt/Squeeze/libsim2d.so | grep '\[CAD\] diag'
# atteso: stringhe tipo "[CAD] diag %s: ..." nel binario

# verifica trace disattivo (default):
strings /opt/Squeeze/libsim2d.so | grep '\[CAD\] diag'
# atteso: nessuna stringa "[CAD] diag" (macro espansa a vuoto)
```

**Alternativa senza toccare i `.pro`:** passare il define al compilatore (es. riga `QMAKE_CXXFLAGS` o variabile ambiente del build system), equivalente a:

```bash
DEFINES+=CAD_DIAG_CALCULATE
```

**Per disattivare di nuovo:** rimuovere la riga `DEFINES += CAD_DIAG_CALCULATE` dai tre `.pro` (o lasciare il default in `CommonConst.h` a `0`) e ricompilare.

#### K) Ottimizzatore in esecuzione — RT sotto carico prolongato (2026-07-14)

<a id="test-6-ottimizzatore-rt"></a>

**Scenario:** pagina **Calculation**, dialog **«Optimization in progress»** (STOP / Continue / Simulate / Confirm), programma **362ModBisDaOtt**, **17 pieghe** (stato osservato: Bend **7/17**), Test **6** (`EMBEDDED_HMI_RT_DRM_DIRECT`), build **7ª** con fix stabilità.

**Durata osservata:** **≥ 10 min** di ottimizzazione in corso, senza crash.

**Metriche RT** (Lnk / PerfMonitor, CPU3 — log ogni 1000 attivazioni):

| Attivazioni cumulative | `nanosleep` max | `nanosleep` min | Conteggio **> 100 µs** |
|------------------------|----------------:|----------------:|------------------------:|
| 72 000 | **68 µs** | 16 µs | **0** |
| 73 000 | **68 µs** | 16 µs | **0** |

```text
Attivazioni: 72000
valore massimo della nanosleep: 68
valore minimo della nanosleep: 16
i valori sopra ai 100 us: 0

Attivazioni: 73000
valore massimo della nanosleep: 68
valore minimo della nanosleep: 16
i valori sopra ai 100 us: 0
```

**Peggior iterazione `[WORST rtc_handler_us]`** (CPU3, stessa sessione ottimizzatore — iter **530**):

| Contatore | Valore | Note |
|-----------|--------|------|
| `rtc_handler_us` | **68 µs** | allineato al max `nanosleep` |
| L2 miss | **29,40%** | `l2d_cache_refill` 2412 / `l2d_cache` 8203 |
| `bus_access` | 9 657 | |
| `bus_cycles` | 225 507 | |
| `cpu_cycles` | 446 436 | |
| Istruzioni | 142 722 | |
| IPC | **0,320** | |
| CPI | **3,128** | |

```text
Core: CPU3
[WORST rtc_handler_us] iter=530  rtc_handler_us=68 us
  l2d_cache=8203  l2d_cache_refill=2412  L2 cache miss=29.4039 %
  bus_access=9657  bus_cycles=225507
  cpu_cycles=446436  istruzioni=142722  IPC=0.319692  CPI=3.128011
```

> Su questa worst iter il traffico bus (**9,6k** access) e l’IPC (**0,32**) sono **modesti** rispetto al picco dezoom documentato in sezione I (bus_access **16 040**, IPC **0,12**, CPI **8,34** @ 109 µs). Coerente con carico ottimizzatore **distribuito** e non burst di redraw Sim2D.

**Interpretazione:**

| Asse | Esito |
|------|-------|
| **RT** | ✅ **Eccellente** sotto ottimizzatore prolongato — max **68 µs**, ben sotto obiettivo **< 100 µs**, zero spike |
| **Stabilità** | ✅ Nessun crash durante la sessione (Calculate + ottimizzazione in corso) |
| **stderr** | ⚠️ Con `CAD_DIAG_CALCULATE=1` (debug crash) output **`[CAD] diag` continuo** — risolto impostando **default `0`** in `CommonConst.h` |

> Confronto: worst case precedente misurato in sessione Calculation/dezoom = **109 µs** (3 spike su 1 068k). L’ottimizzatore prolongato resta **più favorevole** (68 µs max, 0 spike a 73k) — coerente con carico CPU GUI/ottimizzatore distribuito nel tempo vs burst di redraw al dezoom.

```bash
# ricompilare tutte le lib coinvolte (7ª build), poi:
cp libcad2d.so* /opt/Squeeze/
cp libsim2d.so* /opt/Squeeze/
cp libottimizzatore.so* /opt/Squeeze/   # nome effettivo lib ott nel progetto
cp libPegLib.so* /opt/Squeeze/          # se aggiornato peg_crashdiag (__stack_chk_fail)
md5sum /opt/Squeeze/libcad2d.so.1.0.0 /opt/Squeeze/libsim2d.so.1.0.0
strings /opt/Squeeze/libsim2d.so | grep '\[CAD\] diag'
strings /opt/Squeeze/libPegLib.so | grep 'stack_chk_fail'
```

**Test di validazione post-deploy (obbligatorio):**

1. Aprire pagina **Calculation** (sim2d)
2. Zoom / dezoom ripetuti sul grafico
3. Durante redraw, premere **Bend**, **Simulate**, **Rotate**, navigazione toolbar
4. Se presente finestra CAD draft (stato macchina IMP): spostarla e ripetere **Piega**
5. **CAD pezzo:** aggiungere linee fino a **40/40** → errore 10008 su 41ª → **Calculate** → pagina Sim2D si apre **senza crash** (7ª build); poi **Add Section** → dialog 10008 → Ok → **nessun crash**, resta sulla sezione corrente (9ª build)
6. Campagna **≥ 15 min** con monitor RT (`nanosleep` max, `count_ge_100`)

**Riepilogo iterazioni fix `libcad2d` / ottimizzatore (stabilità GUI):**

| Build | Data | Percorso crash | File principali |
|-------|------|----------------|-----------------|
| 1ª | 2026-07-13 | `CPezzoForm::EditDraw` → `GetCodiceRecord` | `PezzoForm.cpp` |
| 2ª | 2026-07-13 | Form doppia L,alpha + `RecGrafPtr` stale | `PezzoForm.cpp`, `PezzoFormLAlpha.cpp`, `Ppgdoc.cpp` |
| 3ª | 2026-07-13 | `InizioSequenza` / mouse / `OnPrev`→`PK_F9` | `Pezzoview.cpp`, `PezzoFrame.cpp` |
| **4ª** | **2026-07-14** | **`CDraftPieceWnd::RefreshView`** via **`OnPiega`** (sim2d) | **`DraftPieceWnd.cpp`**, **`PezzoDoc.cpp`** |
| **5ª** | **2026-07-14** | **`TestChiudiPagCad2D`** via **Calculate** (chiusura pagina CAD) | **`StdAfx.cpp`** |
| **6ª** | **2026-07-14** | Ipotesi **`LookForBends`** / `prof_lin` con **40/40** + Calculate | **`Ottutens.cpp`**, **`Ottinit.cpp`**, **`Ottcomp.cpp`**, **`Ppgdoc.cpp`**, **`PezzoFrame.cpp`**, **`Pezzoview.cpp`** |
| **6ª rev** | **2026-07-14** | Clamp `SezioniCadGrafico` / `sez = min(..., MAX_GBEND)` — **necessario ma non sufficiente** | stessi file ott + `GetDatiOttPezzo` |
| **7ª** | **2026-07-14** | **`PolyLinePez`** stack smashing — **`pez_temp[MAX_ELEM_PERM]`** con 40 pieghe (`index_max≈80`, ~82 punti) | **`Sim2DView.cpp`**, **`Ottutens.cpp`**, diagnostica **`CommonConst.h`** (`CAD_DIAG`), **`peg_crashdiag.cpp`** |
| **8ª** | **2026-07-15** | **Freeze GUI** su chiusura Optimize **`THR_ENDSOL`/`THR_IMPOSS`** — `PpgMessageWindow` in `OnHide()` durante `Execute()` modale; trace `CAD_DIAG` | **`Ottimdlg.cpp`**, **`Sim2DFrame.cpp`**, **`ottimizzatore.pro`**, **`sim2d.pro`**, **`Ottcomp.cpp`** |
| **9ª** | **2026-07-21** | **SIGSEGV** su **Add Section** a **40/40** dopo dialog **10008** — `ViewGrafSucc` lasciava sezione vuota con `RecGrafPtr()==NULL` → `PopulateTable` / `NumRowTableFromGrafNum` | **`PezzoDoc.cpp`** (rollback), **`PezzoForm.cpp`** (`OnSezSucc`), **`PezzoFormLAlpha.cpp`** (guard NULL/bounds) |

<a id="cad-crash-add-section-10008"></a>

**Crash Add Section @ 40/40 (errore 10008) — 9ª build**

| Campo | Dettaglio |
|-------|-----------|
| **Trigger** | CAD pezzo a **Step 40/40** → dialog **Error 10008** (*Too many diagram elements*) → **Add Section** → **SIGSEGV** |
| **Stack tipico** | `CDeskToolBar::Message` → `CPezzoFrame::Message` → `CPezzoFormLAlpha::PopulateTable` → `NumRowTableFromGrafNum` |
| **Causa** | Limite **globale** `MAX_GBEND` (40): `InserimentoAbilitato` rifiuta la piega di default della nuova sezione. `ViewGrafSucc` aveva già avanzato la vista e messo `m_pRecG = NULL`; `OnSezSucc` chiamava comunque `PopulateTable()` → deref NULL |
| **Fix** | Rollback vista in `CPezzoDoc::ViewGrafSucc` se `AggiungiPiega` fallisce; `OnSezSucc` / tastiera L,α non refreshano se fallisce; guard NULL/bounds in `PopulateTable` / `NumRowTableFromGrafNum` |
| **Deploy** | Rebuild + `cp` **`libcad2d.so*`** su target |
| **Test** | 40/40 → Add Section → dialog 10008 → Ok → **resta su sezione 1**, niente crash; sotto 40 elementi Add Section continua a funzionare |

**Pipeline:**

```text
PEG RGB565 → blitDirtyRegion (memcpy) → dumb buffer back → pageFlip → display
Touch → evdev → PEG mouse mapping
```

#### L) Campagna stress completa — ~1h15, tutte le iterazioni grafiche pesanti (2026-07-15)

<a id="test-6-campagna-stress-1h15"></a>

**Scenario:** Test **6** (`EMBEDDED_HMI_RT_DRM_DIRECT`), build **7ª**. Durata **~1 h 15 min**. Percorso utente: tutte le iterazioni grafiche più pesanti (Calculation, zoom/dezoom, navigazione pieghe, ecc.) **+** programma di **ottimizzazione pesante** in parallelo.

**Metriche RT** (Lnk / PerfMonitor, CPU3 — `COM RTC Handler` / `nanosleep`):

| Durata | Worst `rtc_handler_us` | Iter worst | Note |
|--------|------------------------|------------|------|
| **~1 h 15 min** | **99 µs** | **33 934** | Campagna stress completa — esito **eccellente** |

**Peggior iterazione `[WORST rtc_handler_us]`** (CPU3, iter **33 934**):

| Contatore | Valore | Note |
|-----------|--------|------|
| `rtc_handler_us` | **99 µs** | sotto obiettivo **< 100 µs** |
| L2 miss | **32,22%** | `l2d_cache_refill` 6060 / `l2d_cache` 18811 |
| `bus_access` | 24 254 | |
| `bus_cycles` | 868 187 | |
| `bus_access/bus_cycles` | 0,0279 | |
| `cpu_cycles` | 1 731 532 | |
| Istruzioni | 633 399 | |
| IPC | **0,366** | |
| CPI | **2,734** | |

```text
Core: CPU3
[WORST rtc_handler_us] iter=33934  rtc_handler_us=99 us
  l2d_cache=18811  l2d_cache_refill=6060  L2 cache miss=32.2152 %
  bus_access=24254  bus_cycles=868187
  cpu_cycles=1731532  istruzioni=633399  IPC=0.365803  CPI=2.733714
```

**Interpretazione:**

| Asse | Esito |
|------|-------|
| **RT scheduler (CPU3)** | ✅ **Validato** su campagna prolungata — worst **99 µs** @ iter **33 934**, coerente con sezioni I/K |
| **Stabilità crash** | ✅ Nessun crash durante la sessione (Calculate, redraw Sim2D, ottimizzatore) |
| **Responsiveness GUI** | ✅ Freeze **`THR_ENDSOL`** risolto (8ª) — vedi [sezione M](#test-6-freeze-ottimizza-40-40) |

> Confronto worst iter: **530** (68 µs, ottimizzatore 17 pieghe) vs **33 934** (99 µs, campagna 1h15). Il picco assoluto resta **sotto 100 µs** — obiettivo RT Test 6 **raggiunto** anche sotto stress massimo.

---

#### M) Freeze GUI — Optimize «Soluzione non trovata» (`THR_ENDSOL`) — **RISOLTO** (2026-07-15)

<a id="test-6-freeze-ottimizza-40-40"></a>

**Sintomo (bug):** dopo ottimizzazione **senza soluzione**, UI **completamente bloccata** — dialog **«Optimization in progress»**, pulsante **Optimize** premuto, touch morto. Il **scheduler RT** resta sano (non è jitter RT).

**Repro originale (sessione collega, 17 pieghe):**

1. Programma con **17 pieghe** → **Optimize**
2. Ottimizzatore prova **~2 080 000+ permutazioni** (`permuta=2083000`, `passi_seq=17`) — **ore** di calcolo
3. Esito: **`OTT_ENDSOL` (9)** / **`THR_ENDSOL` (4)** — permutazioni esaurite, nessuna soluzione
4. Log si ferma su **`CloseDlg`** — **nessun** `OnOttimizza: OttimDlg closed` → **`Execute()` modale non ritorna**

```text
[CAD] diag MainOtt: ottimize_cycle ret=9
[CAD] diag EseguiCalcola2: thread exit ret=4
[CAD] diag OttimDlg: OnTimer terminal sSoluzTrovata=4
[CAD] diag OttimDlg: CloseDlg
→ FREEZE (stderr fermo, ^C)
```

**Repro rapido validato (4 pieghe, secondi):**

1. Pezzo CAD **4 pieghe** (max **4! = 24** permutazioni)
2. **Optimize** → messaggio UI **«Soluzione non trovata»**
3. **Stesso percorso codice** di chiusura (`sSoluzTrovata=4`), senza ore di attesa

**Root cause (build 8ª):**

`COttimDlg::OnHide()` chiamava **`PpgMessageWindow()`** (seconda dialog con `Execute()`) **mentre** la dialog ottimizzazione era ancora dentro il proprio **`Execute()`** → due modali annidate → message pump bloccato → freeze.

**Fix (build 8ª):**

| File | Modifica |
|------|----------|
| **`Ottimdlg.cpp`** | `OnHide()` per **`THR_ENDSOL`/`THR_IMPOSS`**: solo log `nosoluz deferred`, **no** `PpgMessageWindow` |
| **`Sim2DFrame.cpp`** | `OnOttimizza()` / `OttimizzaAutomatico()`: `PpgMessageWindow(IDS_OTT_NOSOLUZ)` **dopo** `pDlg->Execute()` |
| **`Ottimdlg.cpp`**, **`Ottcomp.cpp`**, **`Sim2DFrame.cpp`** | Trace `CAD_DIAG` opzionale (`CAD_DIAG_CALCULATE=1` in `sim2d.pro` + `ottimizzatore.pro`) |

**Log atteso post-fix (4 pieghe, `THR_ENDSOL`):**

```text
[CAD] diag OttimDlg: OnTimer terminal sSoluzTrovata=4
[CAD] diag OttimDlg: CloseDlg push IDB_OK
[CAD] diag OttimDlg: Message IDB_OK
[CAD] diag OttimDlg: OnHide nosoluz deferred (sSoluzTrovata=4)
[CAD] diag OnOttimizza: OttimDlg closed sSoluzTrovata=4 EXISTSOLUZ=0
[CAD] diag OnOttimizza: PpgMessageWindow nosoluz begin
[CAD] diag OnOttimizza: PpgMessageWindow nosoluz done
```

**Esito validazione 2026-07-15:** messaggio **«Soluzione non trovata»** visibile, UI **libera** dopo OK — **stesso percorso** del freeze collega, **fix confermato**.

**Nota su repro 40/40:** il caso **40/40 + Optimize** può sembrare freeze anche **senza** bug — combinatoria enorme (`40!`), dialog modale per **ore** con `permuta` che sale. Distinto da questo bug (freeze **in chiusura** con `THR_ENDSOL` già raggiunto).

**Evidenza stderr `[RT] uploadDirtyRegion` (sessione freeze originale):** spike singolo **`updateMs=797,6 ms`** (poi recupero) — contesa framebuffer→DRM sotto carico, **non** causa del freeze in chiusura dialog.

**Deploy fix:**

```bash
sh scripts/build_sqvara.sh avn8mp release
cp pressbrakepeg/out/avn8mp/release/libsim2d.so* /opt/Squeeze/
cp pressbrakepeg/out/avn8mp/release/libottimizzatore.so* /opt/Squeeze/   # se ricompilato con CAD_DIAG
# riavvio Enk/PegExec
```

**Produzione:** rimuovere `DEFINES += CAD_DIAG_CALCULATE` da `sim2d.pro` / `ottimizzatore.pro` quando non serve trace stderr.

**Test rapido regressione freeze:**

| Programma | Pieghe | Tempo atteso | Esito atteso |
|-----------|--------|--------------|--------------|
| CAD nuovo | **3–4** | secondi | «Soluzione non trovata» → UI OK |
| Collega | 17 | ore (se no soluzione) | stesso messaggio + UI OK post-fix |

---

#### N) Campagna endurance — **~4 h**, **0 crash** (2026-07-15)

<a id="test-6-campagna-endurance-4h"></a>

**Scenario:** Test **6** (`EMBEDDED_HMI_RT_DRM_DIRECT`), build **8ª** (fix freeze **`THR_ENDSOL`** + stack stabilità **7ª**). Durata **~4 ore** continue. Uso operativo esteso su GUI (navigazione, grafici, interazione touch) — **nessun crash** per tutta la sessione.

**Metriche RT** (Lnk / PerfMonitor, CPU3 — `COM RTC Handler` / `nanosleep`):

| Durata | Attivazioni | `nanosleep` min | `nanosleep` max | Spike **> 100 µs** | Crash |
|--------|------------:|----------------:|----------------:|-------------------:|-------|
| **~4 h** | **~4 152 000** | **11 µs** | **102 µs** | **1** (≈ **0,000024%**) | **0** |

```text
valore massimo della nanosleep delle ultime 4152000 attivazioni vale = 102
valore minimo della nanosleep delle ultime 4152000 attivazioni vale = 11
i valori sopra ai 100 us nelle ultime 4152000 attivazioni sono = 1
```

**Peggior iterazione `[WORST rtc_handler_us]`** (CPU3, iter **14 685**):

| Contatore | Valore | Confronto vs campagna 1h15 (iter 33 934, 99 µs) |
|-----------|--------|--------------------------------------------------|
| `rtc_handler_us` | **102 µs** | +3 µs (entro rumore di misura) |
| L2 miss | **31,59%** | simile (32,22%) |
| `bus_access` | 36 006 | +48% |
| `bus_cycles` | 1 079 083 | +24% |
| `cpu_cycles` | 2 153 619 | +24% |
| Istruzioni | 710 627 | +12% |
| IPC | **0,330** | simile (0,366) |
| CPI | **3,031** | simile (2,734) |

```text
Core: CPU3
[WORST rtc_handler_us] iter=14685  rtc_handler_us=102 us
  l2d_cache=28434  l2d_cache_refill=8982  L2 cache miss=31.5889 %
  bus_access=36006  bus_cycles=1079083
  bus_access/bus_cycles=0.033367  bus_cycles/bus_access=29.9695
  cpu_cycles=2153619  istruzioni=710627  IPC=0.329969  CPI=3.030590
```

**Interpretazione:**

| Asse | Esito |
|------|-------|
| **RT scheduler (CPU3)** | ✅ **Eccellente** su **4 h** — worst **102 µs**, **1 solo spike** > 100 µs su **4,15 M** attivazioni |
| **Stabilità** | ✅ **0 crash** — validazione endurance post build **8ª** |
| **Confronto 1h15** | Worst **99 µs** → **102 µs** (+3 µs); spike **2** / 432k → **1** / 4,15M — **distribuzione RT migliore** su run lungo |

> Il picco **102 µs** resta **accettabile** per produzione (obiettivo < 100 µs con margine spike raro). Su **4,15 M** attivazioni un solo valore > 100 µs (**0,000024%**) conferma che Test 6 **non degrada** la catena RT sotto carico prolongato.

---

<a id="test-6-multitouch-die-set"></a>

#### O) Freeze CAD Die Set — gesto a due dita / pinch (2026-07-20) — ✅ risolto

**Contesto:** pagina **Die Set** (CAD matrice), path Test 6 (`EMBEDDED_HMI_RT_DRM_DIRECT` + touch ILITEK via `pegdrm_evdev`).

**Sintomo:**

| Gesto | Comportamento |
|-------|----------------|
| **1 dito** (pan) | OK — disegno si sposta in modo controllato |
| **2 dita** (mimica pinch/zoom) | disegno **salta a zig-zag** su/giù; poi GUI **freeze** (strisce verticali / artefatti); campo lato es. `DX≈-12070` fuori scala |

**Metriche durante il freeze (stderr `[RT]`):**

| Fase | `updateMs` | `calls` | `maxRectPx` | Note |
|------|----------:|--------:|------------:|------|
| Burst 2 dita | **~53–58 ms** | ~100 | ~485 051 | pan a raffica |
| Escalation freeze | **29 → 572 ms** | 22–77 | ~485 051 | `effMBps` crolla (~610 → ~22) — GUI satura |

**Causa (catena):**

1. `pegdrm_evdev` trattava il multitouch Type B come **un solo cursore**, **senza** filtrare `ABS_MT_SLOT`: a ogni `SYN_REPORT` le coordinate saltavano tra dito 1 e dito 2.
2. PEG vede un mouse con LMB premuto → `CPPGBaseView::OnMove` → `UpdateAllViews()` a ogni salto (soglia 2 px).
3. ILITEK emette ~**100 Hz** di `SYN_REPORT`: ogni evento diventava un motion immediato verso PEG → centinaia di ridisegni CAD + upload full-canvas → `updateMs` a centinaia di ms → **freeze** (interferenza DDR/CPU anche sul thread RT).

> Il pinch **non** è supportato dall’HMI (zoom solo tasti `+` / `−` / fit). Il gesto a 2 dita era solo un pan spurio.

**Fix** (`pegenstein/PegLib/pegdrm_evdev.cpp` + `.h`, branch Test 6):

| Misura | Comportamento |
|--------|----------------|
| **Solo primo dito** | `ABS_MT_SLOT` + `TRACKING_ID`: il primo contatto è lo slot primario; gli altri non aggiornano `m_curX/Y` |
| **Abort su 2° dito** | secondo `TRACKING_ID ≥ 0` → `TouchUp`, flag `m_suppressMulti` fino a tutte le dita alzate |
| **Coalescing motion** | al massimo **1** `TouchMotion` per chiamata a `poll()` (non uno per ogni `SYN_REPORT`) |

**Validazione post-fix (2026-07-20):**

| Check | Esito |
|-------|-------|
| Freeze / zig-zag con 2 dita | ✅ **risolto** — 2° dito interrompe il drag, niente pan a raffica |
| Pan 1 dito + aggiunta lati Die Set | ✅ usabile |
| Spike RT durante aggiunta righe | **1** outlier nanosleep **111 µs** (unico > 100 µs); correlato temporalmente a un `updateMs≈110` **ms** (ridisegno CAD), **non** la stessa grandezza; **non ricomparso** continuando ad aggiungere lati |

> **Unità:** ritardo nanosleep = **µs**; `updateMs` = **ms**. I numeri ~111 / ~110 sembrano “uguali” ma sono scale diverse; la correlazione è l’istante di carico GUI, non un ritardo RT di 110 ms.

**Deploy:** ricompilare e copiare `libPegLib.so` in `/opt/Squeeze/`.

---

### Esito attuale (2026-07-20)

> ✅ **Test 6 — RT:** campagna endurance **~4 h** (2026-07-15) — worst **102 µs** @ iter **14 685**, **1** spike > 100 µs su **~4,15 M** attivazioni — vedi [sezione N](#test-6-campagna-endurance-4h). Sessioni precedenti: **99 µs** (1h15), **109 µs** (dezoom), **68 µs** (ottimizzatore). Sessione Die Set (2026-07-20): outlier unico **111 µs** — vedi [O](#test-6-multitouch-die-set).
>
> ✅ **Test 6 — Stabilità crash:** **0 crash** su **4 h** (build **8ª**); **Calculate 40/40** risolto con **7ª build** (`PolyLinePez`).
>
> ✅ **Test 6 — Responsiveness:** freeze **Optimize / `THR_ENDSOL`** risolto (**8ª**); freeze **Die Set 2 dita / pinch** risolto in `pegdrm_evdev` (2026-07-20) — vedi [sezione O](#test-6-multitouch-die-set).

| Asse | Esito attuale Test 6 |
|------|----------------------|
| **RT** | ✅ worst **102 µs** (4 h) · **1** spike / **4,15 M** · Die Set **111 µs** (1 outlier, 2026-07-20) · **99 µs** (1h15) · **68 µs** (ottimizzatore) · **109 µs** (dezoom) |
| **GUI rendering** | ✅ touch OK · ✅ linee orizzontali risolte · ✅ rettangoli bianchi assenti |
| **GUI liveness** | ✅ Freeze **`THR_ENDSOL`** risolto (8ª) · ✅ Freeze **Die Set multitouch** risolto (`pegdrm_evdev`, 2026-07-20) |
| **CPU** | ✅ idle PegExec **4%** / Lnk **19%** (2026-07-13) · stress PegExec **30,5%** · ottimizzatore PegExec **~60%** + SqServerd **~47%** (17 pieghe) |
| **Stabilità crash** | ✅ **0 crash** su campagna **~4 h** (8ª, 2026-07-15) · **Calculate 40/40** (7ª) |

**Stato codice:** macro `EMBEDDED_HMI_RT_DRM_DIRECT` su branch Test 6 (`PegLib`); fix stabilità **cad2d/ottimizzatore** (1–7ª) + **sim2d** freeze Optimize (**8ª**); **evdev single-pointer + coalescing** (2026-07-20, Die Set).

**Prossimi passi:**

1. **Promuovere Test 6 a produzione** (merge branch Test 6 → main; deploy `libPegLib.so` + `libcad2d.so` + `libsim2d.so` + lib ottimizzatore + `PegExec`)
2. Rimuovere `CAD_DIAG_CALCULATE` dai `.pro` per build produzione (stderr pulito)
3. Opzionale RT: **istogramma** ritardi 50–120 µs (70 bin) in Lnk/PerfMonitor per grafico distribuzione
4. Pin permanente `Lnk` su CPU3 (`taskset -cp 3 $(pidof Lnk)`)

**Verifica deploy (atteso in avvio):**

```text
[RT] crashdiag: handler SIGSEGV/SIGABRT/SIGBUS + __stack_chk_fail attivo
[RT] drm_direct: Opzione D — output DRM dumb RGB565, SDL solo eventi
[RT] PegLib build … …
[RT] rtos.ini XRes=1024 YRes=768 → framebuffer PEG 1024x768 … display 1024x600 …
[RT] drm: KMS disponibile su /dev/dri/card1
[RT] drm: damage tracking attivo (sync integrale PEG→back se damage>=15% o blit>=6)
[RT] evdev: touch multitouch (...) da /dev/input/event2 "ILITEK ILITEK-TP"
```

**Verifica rapida binario sul target:**

```bash
strings /opt/Squeeze/libPegLib.so | grep -E 'crashdiag|framebuffer PEG|PegLib build'
```

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
4. Il warning `XView/YView != XRes/YRes` **non è più un problema di sicurezza** se il log mostra `framebuffer PEG 1024x768` — il buffer deve essere `XRes×YRes`, il display `XView×YView`

---

<a id="test-b"></a>

## TEST B — `SDL_LockTexture` streaming (Opzione B)

**Stato:** ❌ Misurato → **rollback** · **Macro:** `EMBEDDED_HMI_RT_STREAMING_LOCK` · **Branch:** `test/SDL_LockTexture` · [← Tabella](#stato-test)

---

> **Obiettivo:** sostituire `SDL_UpdateTexture()` con il path raccomandato da SDL per texture **STREAMING** aggiornate frequentemente: `SDL_LockTexture` → `memcpy` riga-per-riga → `SDL_UnlockTexture`.
>
> **Ipotesi SDL:** `UpdateTexture` può usare staging/copie extra; il lock scrive direttamente nel buffer mappato della texture.

### Pipeline

**Baseline (Test 0):**

```text
framebuffer PEG (RAM) → SDL_UpdateTexture() → SDL_Texture → SDL_RenderCopy() → SDL_RenderPresent()
```

**Test B (questa build):**

```text
framebuffer PEG (RAM) → SDL_LockTexture() → memcpy → SDL_UnlockTexture() → SDL_RenderCopy() → SDL_RenderPresent()
```

La texture resta `SDL_TEXTUREACCESS_STREAMING` (già così in Test 0); cambia solo il modo di caricare i pixel dirty.

> **`updateMs` in questo test:** tempo cumulato in **`SDL_LockTexture()`** + `memcpy` + **`SDL_UnlockTexture()`** (al posto di `SDL_UpdateTexture`). Present SDL fuori metrica — come Test 0.

### Modifica codice

| File | Cosa |
|------|------|
| `PegLib/PegLib.pro` | `DEFINES += EMBEDDED_HMI_RT_STREAMING_LOCK` |
| `PegLib/peglvglwindow.cpp` | `rtUploadViaStreamingLock()` in `uploadDirtyRegion()` |

**Verifica deploy:**

```bash
strings libPegLib.so | grep 'SDL_LockTexture path'
# oppure all'avvio PegExec su stderr:
# [RT] upload path: SDL_LockTexture + memcpy (streaming, Test B)
```

**Rollback:** commentare `EMBEDDED_HMI_RT_STREAMING_LOCK` in `PegLib.pro`, rebuild.

### Protocollo misura

Stesso di [TEST 0](#test-0):

1. Deploy `libPegLib.so` + `PegExec`
2. Avvio PegExec → attendere **≥ 30 s** → Lnk / PerfMonitor
3. Scenari: **idle** (no touch) e **scroll/drag grafico**
4. Confrontare `[RT] uploadDirtyRegion` (`reqMBps`, `updateMs`, `effMBps`) e `rtc_handler_us` / `nanosleep` max vs baseline Test 0

### Attese / rischi

| Aspetto | Nota |
|---------|------|
| GUI `effMBps` / `updateMs` | Possibile miglioramento se SDL evita staging interno |
| RT | Da misurare — beneficio non garantito |
| Formato RGB565 su GLES2 | [TEST 5](#test-5) suggeriva che il mismatch formato potrebbe limitare il guadagno (conversione altrove nella pipeline) |
| `lockedPitch` | Può differire dal pitch PEG → copia **riga per riga** (già implementata) |

### Esito (2026-07-10, target avn8mp)

**Config:** `rtos.ini` produzione 1024×768 / viewport 1024×600 (`maxRectPx=485051` in scroll). Deploy verificato: `[RT] upload path: SDL_LockTexture + memcpy (streaming, Test B)`.

#### GUI — scroll grafico (`calls≈33`, stesso scenario Test 0)

| Metrica | Test 0 baseline | Test B (LockTexture) | Δ |
|---------|----------------:|---------------------:|---|
| `reqMBps` | ~24,1 | ~23,4–23,8 | ≈ uguale |
| `updateMs/s` | ~208–216 | ~209–241 | ≈ uguale (leggermente peggio) |
| `effMBps` | ~114–117 | ~101–106 | **≈ −10%** |
| `maxRectPx` | 485051 | 485051 | uguale |

Esempi log Test B:

```text
[RT] uploadDirtyRegion: calls=33 req=24.37MB reqMBps=23.77 updateMs=229.267 effMBps=106.3 maxRectPx=485051
[RT] uploadDirtyRegion: calls=33 req=24.74MB reqMBps=24.09 updateMs=241.xxx effMBps=101.1 maxRectPx=485051
```

#### RT — worst case scroll (CPU3)

| Metrica | Test 0 | Test B |
|---------|-------:|-------:|
| `rtc_handler_us` max | 122 µs | **120 µs** |
| `L2 cache miss` | — | 17,1% |

RT entro rumore di misura (±2 µs), **nessun beneficio reale**.

#### Conclusione

> **Confermato empiricamente:** su i.MX8MP + SDL/KMSDRM + GLES2, `SDL_LockTexture` **non migliora** rispetto a `SDL_UpdateTexture` — GUI uguale o leggermente peggiore (`effMBps` −10%), RT invariato. Coerente con [TEST 5](#test-5): il collo di bottiglia non è lo staging di `UpdateTexture` ma il path formato/conversione RGB565→GLES e il resto della pipeline.
>
> **Rollback:** commentare `EMBEDDED_HMI_RT_STREAMING_LOCK`, tornare a `test0-baseline` / Test 0.

`ERROR: Could not restore CRTC` all'avvio/uscita: noto su teardown SDL/KMSDRM, **non** causato da Test B (vedi nota in [TEST 6](#test-6)).

---

<a id="test-7"></a>

## TEST 7 — Pixel clock display (kernel / DRM) *(cronologia)*

**Stato canonico:** ⏸️ [Possibili migliorie §2](#pm-pixel-clock) — questa sezione resta il **log dettagliato** fase 0.

**Stato:** ⏸️ **Sospeso** (2026-07-10) — fase 0 ✅ · fasi 2–3 solo via **BSP Yocto** · **Repo app:** nessuna modifica · [← Tabella](#stato-test) · [← Possibili migliorie](#possibili-migliorie)

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

> **Stato aggiornato:** [Possibili migliorie](#possibili-migliorie) e sintesi in [Stato test](#stato-test).
>
> Non duplicare qui — modificare solo quelle sezioni.

---

<a id="test-dcc"></a>

## TEST DCC — Framebuffer Compression / Prefetch (fase 0, i.MX8MP) *(cronologia)*

**Stato canonico:** ❌ [Possibili migliorie §1](#pm-dcc) — questa sezione resta il **log dettagliato** fase 0.

**Stato:** ❌ Non applicabile · **Data diagnosi:** 2026-07-10 · [← Tabella](#stato-test) · [← Possibili migliorie](#possibili-migliorie)

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

**Stato attuale:** il framebuffer **PEG** è a **16 bpp RGB565** (`Bpp=16` in `rtos.ini`). Attenzione: sul path **SDL + KMSDRM** lo **scanout** verso il pannello resta **32 bpp** (vedi [TEST 5](#test-5)); i **16 bpp sul driver display** (`lcdifv3`) si ottengono solo con il path **Test 6** ([sotto](#bpp-scanout-drm-16)). L'alternativa **24 bpp** in ini è commentata — aumenterebbe i byte PEG (+50%).

**Cosa è stato verificato:**

- Config produzione / test: `Bpp=16`, `ForceBPP=True` (PEG in RAM)
- Log avvio: `[RT] rtos.ini XRes=… YRes=… → framebuffer effettivo … @ 16 bpp = X.XX MiB` (`peg_run.cpp`)
- Test 4 (800×600) ha ulteriormente ridotto il traffico pixel mantenendo PEG a 16 bpp
- Path SDL: `fb0` a **32 bpp**; path Test 6: FB DRM **RGB565 / RG16**

<a id="bpp-scanout-drm-16"></a>

#### Come impostare 16 bpp a livello di scanout (userspace → kernel)

Su i.MX8MP **non** c’è un flag DT / `menuconfig` del tipo “forza 16 bpp sul LCDIF”. Il driver **`imx-drm` + `imx-lcdifv3-crtc`** espone i formati supportati (`XR24` = 32 bpp, `RG16` = 16 bpp); il bpp effettivo dello scanout è quello del **framebuffer DRM** attaccato al CRTC/plane.

| Path | Cosa fa `Bpp=16` in `rtos.ini` | Formato scanout (LCDIF) |
|------|--------------------------------|-------------------------|
| SDL + GLES + KMSDRM (Test 0 / tipico) | PEG a 16 bpp in RAM | **32 bpp** (`XR24` / `fb0`) |
| **Test 6** `EMBEDDED_HMI_RT_DRM_DIRECT` | PEG a 16 bpp + blit `memcpy` | **16 bpp** (`DRM_FORMAT_RGB565` / `RG16`) |

**Codice (Test 6) — `PegLib/pegdrmoutput.cpp` (`PegDrmOutput`):**

1. Alloca dumb buffer a **16 bpp**:
```cpp
createReq.width  = m_width;
createReq.height = m_height;
createReq.bpp    = 16;   // RGB565
drmIoctl(m_fd, DRM_IOCTL_MODE_CREATE_DUMB, &createReq);
```

2. Registra il framebuffer DRM come **RGB565** (fourcc → plane `RG16`):
```cpp
drmModeAddFB2(m_fd, m_width, m_height,
    DRM_FORMAT_RGB565, handles, pitches, offsets, &buf.fbId, 0);
```

3. Modeset / flip sul CRTC (`drmModeSetCrtc`, `drmModePageFlip`) — a quel punto il kernel programma il LCDIF sul FB a 16 bpp.

4. Present: `blitDirtyRegion()` copia RGB565 PEG → dumb buffer back (nessuna conversione a 32 bpp).

**Abilitazione:** macro `EMBEDDED_HMI_RT_DRM_DIRECT` in `PegLib.pro` (branch Test 6), rebuild `libPegLib.so`.

**Verifica sul target** (con PegExec Test 6 in esecuzione):

```bash
# Capacità plane (deve elencare RG16)
modetest -M imx-drm -p 2>/dev/null | grep formats
# → formats: XR24 AR24 RG16 ...

# Bpp dello scanout esposto da fbdev (path SDL tipicamente 32; con DRM diretto dipende dal bridge fb)
cat /sys/class/graphics/fb0/bits_per_pixel

# Log avvio PegLib: probe card imx-drm + dumb RGB565
./PegExec 2>&1 | grep -E '\[RT\] drm:'
```

> **Per il confronto userspace vs kernel:** `Bpp=16` in `rtos.ini` da solo **non** basta a far lavorare il LCDIF a 16 bpp sul path SDL. Serve un FB DRM creato/registrato come `DRM_FORMAT_RGB565` e impostato sul CRTC — come fa `PegDrmOutput` nel Test 6.

**Modifica rapida del Bpp PEG (senza rebuild)** — solo `rtos.ini` + riavvio `PegExec` *(non cambia lo scanout SDL 32 bpp)*:

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

> Per ridurre ulteriormente la banda pixel oltre il 16 bpp, le leve rimanenti sono **risoluzione** (Test 4 ✅), [DCC/Prefetch](#pm-dcc) (non applicabile su 8MP) e [ottimizzazione asset](#pm-immagini) — non scendere sotto 16 bpp senza refactor del pipeline colore PEG/SDL/DRM.

---

<a id="diagnostica-test-6"></a>

## DIAGNOSTICA TEST 6

Guida operativa: **cosa modificare nel codice** per attivare o disattivare ogni tipo di log/trace introdotto durante i test RT / stabilità Calculate–Optimize. Due famiglie distinte:

| Prefisso stderr | Modulo | Macro principale | File di controllo |
|-----------------|--------|------------------|-------------------|
| **`[CAD] diag`** | Calculate / ottimizzatore / Sim2D | `CAD_DIAG_CALCULATE` | `pressbrakepeg/.../*.pro` + `CommonConst.h` |
| **`[RT]`** | Display / upload / DRM / crash | `EMBEDDED_HMI_RT_*` | `pegenstein/PegLib/PegLib.pro` |

Dopo ogni modifica ai `.pro`: **ricompilare** la libreria interessata e **copiare** le `.so` in `/opt/Squeeze/`.

---

### A — `[CAD] diag` (Calculate / Optimize / Sim2D)

**Sintomo:** righe continue tipo  
`[CAD] diag LookForBends: nsect=1 piega=9 rot=-1 sez=18 …`  
soprattutto con dialog **Optimization in progress** (loop dell’ottimizzatore).

**Meccanismo:** macro `CAD_DIAG(tag, fmt, …)` in `pressbrakepeg/IncPPG/CommonConst.h`. Se `CAD_DIAG_CALCULATE` è **1** a compile-time, ogni chiamata fa `fprintf(stderr, …)` + `fflush`.

#### Disattivare (stderr pulito — produzione)

1. Aprire e **commentare o rimuovere** la riga in **entrambi** i file (oggi ancora attivi per debug freeze Optimize):

| File | Riga da togliere |
|------|------------------|
| [`pressbrakepeg/sim2d/sim2d.pro`](../../../Squeeze/pressbrakepeg/sim2d/sim2d.pro) | `DEFINES += CAD_DIAG_CALCULATE` |
| [`pressbrakepeg/ottimizzatore/ottimizzatore.pro`](../../../Squeeze/pressbrakepeg/ottimizzatore/ottimizzatore.pro) | `DEFINES += CAD_DIAG_CALCULATE` |

`cad2d/cad2d.pro` **non** ha questa define (già off).

2. Ricompilare e deploy:

```bash
# build avn8mp (script o qmake/make su sim2d + ottimizzatore)
cp pressbrakepeg/out/avn8mp/release/libsim2d.so* /opt/Squeeze/
cp pressbrakepeg/out/avn8mp/release/libottimizzatore.so* /opt/Squeeze/
# libcad2d solo se avete aggiunto CAD_DIAG anche lì
```

3. Verifica trace **spento**:

```bash
strings /opt/Squeeze/libsim2d.so | grep '\[CAD\] diag'
# atteso: nessun output

strings /opt/Squeeze/libottimizzatore.so | grep '\[CAD\] diag'
# atteso: nessun output
```

#### Riattivare (debug crash Calculate / freeze Optimize)

Aggiungere in **ciascuno** di `sim2d.pro`, `ottimizzatore.pro` e (opzionale) `cad2d/cad2d.pro`:

```qmake
DEFINES += CAD_DIAG_CALCULATE
```

Ricompilare le tre librerie. L’ultima riga `[CAD] diag` su stderr prima di un crash indica il punto nel percorso.

**Punti trace principali** (non serve toccare il codice sorgente, solo la macro):

| Tag `CAD_DIAG` | File | Quando |
|----------------|------|--------|
| `LookForBends` | `ottimizzatore/Ottutens.cpp` | ogni piega provata in ottimizzazione |
| `CalcolaNewSituaz`, `MainOtt`, `ottimize_cycle` | `ottimizzatore/Ottcomp.cpp` | ciclo ottimizzatore |
| `OttimDlg`, `OnOttimizza` | `sim2d/Ottimdlg.cpp`, `Sim2DFrame.cpp` | dialog Optimize |
| `OnCalcola`, `OnSim2DCalcola` | `cad2d/PezzoFrame.cpp`, `sim2d/Sim2DExport.cpp` | pulsante Calculate |
| `PolyLinePez` | `sim2d/Sim2DView.cpp` | redraw grafico (clamp `MAX_ELEM_PERM`) |

> Dettaglio storico freeze / build 8ª: sezione [TEST 6](#test-6) e [CAD diag build](#cad-diag-build).

---

### B — `[RT]` display / upload (PegLib — Test 6)

Tutte le define stanno in [`pegenstein/PegLib/PegLib.pro`](../../../Squeeze/pegenstein/PegLib/PegLib.pro) (sezione `DEFINES` in testa). **Mutualmente esclusive** per il path video: usare **solo uno** tra SDL texture (Test 0), `NATIVE_TEXTURE` (Test 5b), `DRM_DIRECT` (Test 6).

#### Tabella rapida — configurazione Test 6 consigliata

| Macro | Test 6 (default attuale) | Effetto se **ON** | Come attivare | Come disattivare |
|-------|--------------------------|-------------------|---------------|------------------|
| **`EMBEDDED_HMI_RT_DRM_DIRECT`** | ✅ **ON** | Path Opzione D: dumb buffer RGB565, pageflip DRM, touch evdev; log `[RT] drm:`, `[RT] drm_direct:` | `DEFINES += EMBEDDED_HMI_RT_DRM_DIRECT` | Commentare la riga; **non** usare Test 6 senza alternativa (tornare Test 0 SDL) |
| **`EMBEDDED_HMI_RT_STATS`** | ✅ **ON** | Ogni ~1 s: `[RT] uploadDirtyRegion: calls=… reqMBps=… updateMs=…` | `DEFINES += EMBEDDED_HMI_RT_STATS` | Commentare la riga in `PegLib.pro` |
| **`EMBEDDED_HMI_RT_DIAG`** | ❌ **OFF** | Una tantum all’avvio: report driver SDL, texture, formati (`[RT] diag:`) — **solo path SDL** | Decommentare `#DEFINES += EMBEDDED_HMI_RT_DIAG` | Lasciare commentato (inutile con DRM_DIRECT: niente SDL video) |
| **`EMBEDDED_HMI_RT_SAFE`** | ❌ **OFF** | Coalescing eventi touch/mouse motion | Decommentare `#DEFINES += EMBEDDED_HMI_RT_SAFE` | Lasciare commentato |
| **`EMBEDDED_HMI_RT_NATIVE_TEXTURE`** | ❌ **OFF** | Test 5b ARGB8888 + conversione esplicita | Decommentare (e **togliere** `DRM_DIRECT`) | Lasciare commentato su Test 6 |

**Snippet `PegLib.pro` — profilo Test 6 produzione + metriche RT:**

```qmake
DEFINES += EMBEDDED_HMI_RT_STATS
#DEFINES += EMBEDDED_HMI_RT_DIAG
#DEFINES += EMBEDDED_HMI_RT_NATIVE_TEXTURE
DEFINES += EMBEDDED_HMI_RT_DRM_DIRECT
#DEFINES += EMBEDDED_HMI_RT_SAFE
```

Dopo modifica: ricompilare `libPegLib.so` e deploy.

**Verifica path attivo sul target:**

```bash
strings /opt/Squeeze/libPegLib.so | grep -E 'drm_direct|native_texture|SDL_LockTexture'
# Test 6 atteso: [RT] drm_direct: Opzione D ...
# Test 0: nessuna delle stringhe sopra (o solo path SDL classico)

strings /opt/Squeeze/libPegLib.so | grep '\[RT\] uploadDirtyRegion'
# presente se EMBEDDED_HMI_RT_STATS=ON
```

**Log attesi con STATS ON (esempio):**

```text
[RT] drm_direct: Opzione D — output DRM dumb RGB565, SDL solo eventi
[RT] PegLib build … …
[RT] rtos.ini XRes=… → framebuffer effettivo … @ 16 bpp = … MiB
[RT] drm: modeset OK, dumb buffer RGB565 attivo (Opzione D POC)
[RT] uploadDirtyRegion: calls=… req=…MB reqMBps=… updateMs=… effMBps=… maxRectPx=…
```

Per **silenziare solo le metriche periodiche** ma tenere Test 6: commentare `EMBEDDED_HMI_RT_STATS` (restano i log una-tantum di avvio DRM se presenti nel codice).

---

### C — `[RT] crashdiag` (backtrace su segfault)

**Sempre compilato** con `PEG_USE_LVGL`: `peg_crashdiag.cpp` in `PegLib.pro`, installato da `PegCrashDiagInstall()` in `peg_run.cpp` all’avvio.

| Comportamento | Output |
|---------------|--------|
| Avvio | `[RT] crashdiag: handler SIGSEGV/SIGABRT/SIGBUS + __stack_chk_fail attivo` |
| Crash | `[RT] FATAL … — backtrace (N frame):` + stack |

**Disattivare** (solo se serve stderr minimo assoluto):

1. Commentare la chiamata in `peg_run.cpp`: `PegCrashDiagInstall();`
2. Opzionale: rimuovere `peg_crashdiag.cpp` da `SOURCES` in `PegLib.pro`

**Consiglio:** lasciare **attivo** in Test 6 — costo trascurabile, utile senza core dump su target.

---

### D — Checklist deploy diagnostica

| Obiettivo | Azioni |
|----------|--------|
| **Produzione Test 6 silenziosa** | `CAD_DIAG` off nei `.pro` sim2d/ottimizzatore · `RT_STATS` off (opzionale) · `RT_DIAG` off · `crashdiag` on (consigliato) |
| **Campagna misure GUI/RT** | `RT_STATS` on · `CAD_DIAG` off · protocollo in [Protocollo misura](#protocollo-misura-standard) |
| **Debug crash Calculate** | `CAD_DIAG_CALCULATE` on su cad2d+sim2d+ottimizzatore · riprodurre · ultima riga `[CAD] diag` |
| **Debug pipeline SDL** (Test 0/5) | `RT_DIAG` on · **non** usare con `DRM_DIRECT` |
| **Tornare a Test 0 baseline** | Commentare `EMBEDDED_HMI_RT_DRM_DIRECT` · togliere define Test 5b · rebuild PegLib |

**File da copiare sul target dopo rebuild:**

```bash
cp libPegLib.so* /opt/Squeeze/
cp libsim2d.so* libottimizzatore.so* libcad2d.so* /opt/Squeeze/   # se toccati
cp PegExec /opt/Squeeze/                                          # se ricompilato
```

---

### E — Riepilogo “cosa vedo su stderr e come spegnerlo”

| Messaggio | Spegnere con |
|-----------|----------------|
| `[CAD] diag LookForBends: …` | Rimuovere `CAD_DIAG_CALCULATE` da `sim2d.pro` + `ottimizzatore.pro` |
| `[RT] uploadDirtyRegion: …` | Commentare `EMBEDDED_HMI_RT_STATS` in `PegLib.pro` |
| `[RT] diag: SDL video driver=…` | Non abilitare `EMBEDDED_HMI_RT_DIAG` (già off su Test 6) |
| `[RT] drm: …` / `[RT] drm_direct: …` | Normale con Test 6; per meno log solo a init, non c’è switch — sono pochi messaggi all’avvio |
| `[RT] crashdiag: handler …` | Commentare `PegCrashDiagInstall()` (sconsigliato) |
| `[RT] FATAL … backtrace` | Solo in caso di crash — non è diagnostica da “spegnere” |

---

<a id="file-modificati-vs-test-0"></a>

<a id="differenze-strutturali-interfacce"></a>

## Differenze strutturali interfacce

Questa sezione confronta i **tre host path** usati o discussi durante il lavoro:

1. **Qt classico** (`PEG_USE_QT`)
2. **SDL / Test 0 baseline** (`PEG_USE_LVGL`, ma con path SDL texture + renderer)
3. **DRM diretto / Test 6** (`PEG_USE_LVGL` + `EMBEDDED_HMI_RT_DRM_DIRECT`)

L’obiettivo è chiarire **quali passaggi esistono davvero** tra il framebuffer PEG e il pannello, e perché il Test 6 non si “trasferisce” gratis sul path Qt.

### Vista sintetica

```text
PEG disegna su g_pyBitmap (RAM)
        │
        ▼
   [Qt path]                       [SDL / Test 0]                    [DRM / Test 6]
   QImage (stessa RAM)             SDL_UpdateTexture()               memcpy → dumb buffer DRM
        │                           │                                 │
   QPainter::drawImage             SDL_RenderCopy()                  drmModePageFlip()
        │                           │                                 │
   Qt compositor / EGLFS           SDL_RenderPresent()               scanout diretto KMS
        │                           │
   GPU / display driver            DRM/KMS / display
```

### Tabella comparativa

| Aspetto | Qt classico | SDL / Test 0 baseline | DRM diretto / Test 6 |
|--------|-------------|------------------------|----------------------|
| Macro build | `PEG_USE_QT` | `PEG_USE_LVGL` | `PEG_USE_LVGL` + `EMBEDDED_HMI_RT_DRM_DIRECT` |
| Classe host | `PegMainWindow` | `PegLvglWindow` | `PegLvglWindow` + `PegDrmOutput` |
| Buffer PEG | `g_pyBitmap` / `QImage` in RAM | `g_pyBitmap` in RAM | `g_pyBitmap` in RAM |
| Passo successivo | `QPainter::drawImage()` | `SDL_UpdateTexture()` | `memcpy` verso dumb buffer |
| Present finale | Qt / window system | `SDL_RenderPresent()` | `drmModePageFlip()` |
| Chi controlla il video | Qt | SDL/KMSDRM | codice applicativo |
| Input touch | eventi Qt (`QMouseEvent` / `QTouchEvent`) | eventi SDL | evdev (`pegdrm_evdev`) |
| Livello di controllo sul display | medio / astratto | medio | massimo / esplicito |

### 1 — Qt classico (`PEG_USE_QT`)

Nel path Qt, PEG disegna in RAM e la parte host usa una `QImage` come contenitore dei pixel. La visualizzazione vera avviene dentro `paintEvent()` tramite `QPainter::drawImage()`.

Pipeline semplificata:

```text
PEG
↓
g_pyBitmap / QImage
↓
QPainter::drawImage()
↓
Qt platform plugin / compositor / EGLFS
↓
display
```

**Conseguenza pratica:** l’applicazione non controlla direttamente né il pageflip né il buffer scanout. Il backend video appartiene a Qt.

### 2 — SDL / Test 0 baseline

Questo è il path di riferimento iniziale delle misure RT.

Pipeline semplificata:

```text
PEG
↓
g_pyBitmap
↓
uploadDirtyRegion()
↓
SDL_UpdateTexture()
↓
SDL_RenderCopy()
↓
SDL_RenderPresent()
↓
DRM/KMS / display
```

Qui il collo di bottiglia principale osservato è la copia CPU → texture (`SDL_UpdateTexture()`), che aumenta traffico DDR e interferenza RT.

### 3 — DRM diretto / Test 6

Questo path elimina texture SDL e present GPU dalla catena normale di rendering.

Pipeline semplificata:

```text
PEG
↓
g_pyBitmap
↓
uploadDirtyRegion()
↓
memcpy RGB565 → dumb buffer DRM
↓
drmModePageFlip()
↓
scanout diretto KMS
```

L’input touch, in questo caso, non passa più dal video SDL ma da `pegdrm_evdev`.

### Perché il Test 6 non si porta “gratis” su Qt

Il punto chiave è che **Qt e Test 6 non sono due flag sullo stesso ultimo stadio**, ma due **host path diversi**.

| Caso | Cosa cambia |
|------|-------------|
| Passare da SDL baseline a Test 6 | si resta dentro `PegLvglWindow`, ma si sostituisce texture/present con DRM diretto |
| Passare da LVGL/SDL a Qt | si cambia proprio host class: `PegLvglWindow` → `PegMainWindow` |

Quindi il lavoro Test 6:

- vive in `peglvglwindow.cpp`
- usa `pegdrmoutput.cpp`
- usa `pegdrm_evdev.cpp`
- misura `uploadDirtyRegion()` sul path LVGL/SDL/DRM

Sul path Qt questi pezzi **non vengono usati**. Per avere lo stesso beneficio RT con Qt bisognerebbe:

1. decidere se lasciare Qt a video oppure bypassarlo;
2. portare il present diretto DRM anche dentro l’host Qt;
3. ridefinire input e sincronizzazione;
4. riscrivere le metriche RT sul path Qt.

In altre parole: non è una semplice attivazione/disattivazione di macro, ma un **porting del backend host**.

### Conclusione operativa

| Famiglia modifiche | Dipendenza dal path LVGL/SDL/DRM | Portabilità su Qt |
|--------------------|----------------------------------|-------------------|
| Test 6 video / DRM / evdev / stats `[RT]` | **alta** | **no, non direttamente** |
| Fix CAD / Calculate / Optimize | bassa | **sì** |
| Font #2 (`kvuib`) | nulla | **sì** |

---

<a id="ruolo-reale-lvgl"></a>

## Ruolo reale di LVGL (Test 0 / path embedded)

> Domanda tipica: *“Se disegno con PEG e mostro con SDL, dove stanno le LVGL?”*  
> Risposta breve: **nella pipeline grafica che conta, non ci sono.**

### Chi fa cosa (onesto)

| Pezzo | Ruolo reale |
|-------|-------------|
| **PEG** | widget, bottoni, grafici, ridisegno → `g_pyBitmap` |
| **SDL** (Test 0) o **DRM** (Test 6) | input + portare i pixel a schermo |
| **LVGL** | quasi niente di utile per l’HMI |

Schema Test 0:

```text
Touch
  → SDL (eventi)
  → PEG (logica + disegno)
  → g_pyBitmap (RAM)
  → SDL_UpdateTexture / RenderPresent
  → schermo

LVGL: lv_init + lv_timer_handler  … e basta (scollegato)
```

### Perché tutti dicono “LVGL”?

Perché la macro di build si chiama **`PEG_USE_LVGL`**.

Quel nome è **fuorviante**. Non significa “disegniamo con LVGL”. Significa:

> non usare Qt; usa il backend embedded Linux (classe `PegLvglWindow`, SDL, poi DRM).

È il nome del **ramo di compilazione**, non del motore grafico.

| Macro | Classe host | Significato reale |
|-------|-------------|-------------------|
| `PEG_USE_QT` | `PegMainWindow` | path Qt classico |
| `PEG_USE_LVGL` | `PegLvglWindow` | path embedded non-Qt (SDL / DRM) |

Anche il nome `PegLvglWindow` è storico: “finestra del path non-Qt”, **non** “finestra che disegna con LVGL”.

### Cosa fa il codice LVGL nel loop

All’avvio:

```cpp
lv_init();   // inizializza lo stato interno della libreria LVGL
```

Nel loop principale (`peg_run.cpp`, ogni ~10 ms):

```text
processEvents()           // touch / SDL / evdev
processPendingUpdates()   // upload dirty PEG → SDL/DRM
flushPresent()            // pageflip / present
pumpLvgl()                // ← solo questo
Sleep(10)
```

`pumpLvgl()` nel vostro codice è solo:

```cpp
void PegLvglWindow::pumpLvgl()
{
    lv_timer_handler();
}
```

#### Cosa fa `lv_timer_handler()`

In LVGL è il **super-loop interno**: scorre la lista dei **timer LVGL** registrati e ne esegue quelli scaduti.

In una GUI LVGL vera lì tipicamente girano:

- animazioni
- refresh display LVGL
- lettura input LVGL (`lv_indev`)
- task periodici dei widget

Nel **vostro** caso, invece:

1. `lv_init()` crea lo stato interno  
2. **non** create `lv_display_create` / driver display LVGL  
3. **non** create widget (`lv_btn`, `lv_label`, …)  
4. **non** create input LVGL (`lv_indev`)

Quindi la lista timer è **vuota o quasi**. `lv_timer_handler()` entra, guarda la lista, non trova lavoro utile per l’HMI, esce.

| Chiamata | Effetto reale sull’HMI PEG+SDL/DRM |
|----------|-------------------------------------|
| `lv_init()` | alloca stato interno LVGL |
| `pumpLvgl()` → `lv_timer_handler()` | gira timer LVGL; senza GUI LVGL ≈ **no-op** |

Non disegna bottoni, non tocca `g_pyBitmap`, non chiama SDL, non fa pageflip.

### Analogia

Avete acceso un motore LVGL in garage (`lv_init`) e ogni 10 ms girate la chiave (`lv_timer_handler`), ma:

- non avete collegato il cruscotto (display LVGL)
- non avete collegato il volante (input LVGL)
- non avete messo passeggeri (widget LVGL)

La macchina “gira a vuoto”. Chi guida e chi mostra lo schermo restano **PEG** e **SDL/DRM**.

### Sintesi

> L’interfaccia non è LVGL. È **PEG → framebuffer → SDL** (Test 0) o **PEG → framebuffer → DRM** (Test 6).  
> `PEG_USE_LVGL` è solo il nome della build embedded senza Qt.  
> LVGL è linkato e inizializzato (`lv_init` + `lv_timer_handler`), ma **non gestisce GUI, input né display**.

Vedi anche [Differenze strutturali interfacce](#differenze-strutturali-interfacce) e il documento `pipeline_peg_sdl_drm_rt.md` (§ ruolo LVGL).

---

<a id="cgroups-cpu-non-disponibile"></a>

## Cgroups CPU non disponibile su avn8mp (2026-07-21)

### Motivazione dell’esperimento

Obiettivo proposto: limitare i processi GUI a carico elevato (in primis **`PegExec`**) con un duty-cycle dell’ordine di **~10 ms**, scala tipica di fluidità / latenza percepita dall’operatore HMI, usando **cgroups** (`cpu.max` = quota CFS).

Esempio teorico (cgroup v2):

```text
cpu.max = 10000 20000
          ↑     ↑
          10 ms di CPU ogni 20 ms  →  ~50% di un core
          (= 10 ms ON / 10 ms OFF)
```

Obiettivo: ridurre interferenza RT (nanosleep / DDR) forzando pause sul lavoro GUI, **senza** rebuild dell’applicazione.

> Le modifiche sotto `/sys/fs/cgroup` sono **volatili** (RAM): al reboot spariscono. Nessun rischio di configurazione persistente se non si scrivono unit systemd / script di boot.

### Esito sul target avn8mp

**Non applicabile:** il controller **`cpu`** non è presente nella gerarchia cgroup v2 di questa immagine. Di conseguenza non esistono `cpu.max` / quota CFS e `echo '+cpu'` fallisce.

### Dimostrazione — comandi e output (target `root@avn8mp`, 2026-07-21)

#### 1) Controller disponibili in cgroup v2

```bash
cat /sys/fs/cgroup/cgroup.controllers
cat /sys/fs/cgroup/cgroup.subtree_control
mount | grep cgroup
```

**Output osservato:**

```text
cpuset io
```

```text
(vuoto — nessun controller abilitato in subtree_control)
```

```text
cgroup2 on /sys/fs/cgroup type cgroup2 (rw,nosuid,nodev,noexec,relatime,nsdelegate,memory_recursiveprot)
```

**Lettura:** gerarchia **cgroup v2 unificata** montata, ma tra i controller compaiono solo **`cpuset`** e **`io`**. Manca **`cpu`**.

#### 2) Tentativo di abilitare `cpu` (fallisce)

```bash
echo '+cpu' > /sys/fs/cgroup/cgroup.subtree_control
```

**Output osservato:**

```text
echo: write error: Invalid argument
```

**Lettura:** non è un problema di permessi root. Il kernel rifiuta `+cpu` perché quel controller **non è nella lista** di `cgroup.controllers`.

#### 3) Non c’è nemmeno cgroup v1 `cpu`

```bash
ls /sys/fs/cgroup/cpu 2>/dev/null
cat /proc/cmdline | tr ' ' '\n' | grep -i cgroup
```

**Output osservato:**

```text
(nessuna directory /sys/fs/cgroup/cpu)
```

```text
cgroup_no_v1=all
```

**Lettura:** cmdline forza **solo v2** (`cgroup_no_v1=all`). Non esiste un piano B su `/sys/fs/cgroup/cpu/cpu.cfs_quota_us` (v1).

#### 4) Causa root in config kernel

```bash
zcat /proc/config.gz 2>/dev/null | grep -i CGROUP_SCHED
```

**Output osservato:**

```text
# CONFIG_CGROUP_SCHED is not set
```

**Lettura:** il kernel in esecuzione è stato compilato **senza** `CONFIG_CGROUP_SCHED`. Senza quella opzione (e, per `cpu.max`, tipicamente anche `CONFIG_CFS_BANDWIDTH`) il controller di scheduling CPU per cgroups **non viene esposto**.

Sintesi della catena causale:

```text
CONFIG_CGROUP_SCHED not set
        ↓
niente controller "cpu" in cgroup.controllers
        ↓
echo '+cpu' → Invalid argument
        ↓
impossibile creare/usare cpu.max (quota 10 ms)
```

### Cosa resta fattibile senza rebuild kernel

| Approccio | Disponibile su avn8mp? | Effetto |
|-----------|------------------------|---------|
| `cpu.max` / quota CFS 10 ms | **No** | — |
| `cpuset` (pin PegExec su CPU 0–1 o 0–2) | **Sì** (`cpuset` è in `cgroup.controllers`) | meno core, non duty-cycle 10 ms |
| `PEG_PRESENT_INTERVAL_MS=10` (rate-limit present in PegLib) | **Sì** | cap refresh ~100 Hz, già nel codice |
| `Sleep(10)` nel main loop | **Sì** (già presente) | loop host ~100 Hz; non ferma da solo `PegRefresh` sotto carico |
| `cpulimit` userspace (se in image) | da verificare | approx quota CPU senza cgroup `cpu` |
| Rebuild kernel con `CONFIG_CGROUP_SCHED=y` (+ `CONFIG_CFS_BANDWIDTH=y`) | solo via BSP/Yocto | abilita davvero `cpu.max` |

### Sintesi

> Sull’immagine avn8mp in uso (2026-07-21) la limitazione di PegExec con cgroups a un duty-cycle ~10 ms **non è realizzabile**: il sistema è in cgroup v2 puro (`cgroup_no_v1=all`), ma `cgroup.controllers` elenca solo `cpuset io`, e la config kernel ha `# CONFIG_CGROUP_SCHED is not set`. Di conseguenza `echo '+cpu'` restituisce `Invalid argument` e non esiste `cpu.max`. Per l’esperimento a ~10 ms restano leve applicative (`PEG_PRESENT_INTERVAL_MS`) e/o `cpuset`; l’abilitazione del controller `cpu` richiederebbe un kernel con `CONFIG_CGROUP_SCHED` (e `CONFIG_CFS_BANDWIDTH`) ricostruito nel BSP.

Script di supporto (opzionale, utile solo se in futuro compare il controller `cpu`): `pressa/perf_terminale/peg_cgroup_throttle.sh`.

---

## File modificati vs Test 0

Inventario dei sorgenti toccati **dalla condizione iniziale di Test 0** allo stato attuale (Test 6 + stabilità CAD + experiment font #2).  
Baseline git pegenstein: commit `7acaf1d` (*Strumentazione RT e test riduzione risoluzione 800x600*). Branch corrente tipico: `experiment/test6-with-new-font` (PegLib) / `lvgl-hmi` (pressbrakepeg) / `experiment/test6-with-new-font` (kvuib).

> Solo codice “prodotto”. Esclusi artefatti di build (`Makefile`, `.qmake.stash`, …).

### Riepilogo

| Repo | File “veri” | Contenuto |
|------|------------:|-----------|
| **pegenstein** | **10** | path DRM Test 6, stats RT, crashdiag, touch evdev |
| **pressbrakepeg** | **20** | stabilità Calculate/Optimize, `CAD_DIAG`, guard null |
| **kvuib** | **7** | pulizia build font Chs (rimozione `Yahei_N.cpp` morti) |
| **doc** | registro + dump | `registro_test_rt.md`, `valori_perf.txt`, … |

Per **solo** il delta PegLib Test 0 → Test 6 (senza CAD/font/doc): i **10 file** della sezione pegenstein.

---

### 1 — `pegenstein` (PegLib / HMI)

Branch: `experiment/test6-with-new-font` (ex `experiment/test6-drm`).

#### Modificati rispetto a Test 0

| File | Ruolo |
|------|--------|
| `PegLib/PegLib.pro` | macro `EMBEDDED_HMI_RT_*` (STATS, DRM_DIRECT, …) |
| `PegLib/peglvglwindow.cpp` | `uploadDirtyRegion`, path DRM, stats `[RT]`, eventi |
| `PegLib/peglvglwindow.h` | dich. DRM / RT / coalescing |
| `PegLib/peg_run.cpp` | `PegCrashDiagInstall` + log RT avvio |
| `Files/avn8mp/rtos.ini` | risoluzione / viewport produzione |

#### Nuovi (non esistevano in Test 0)

| File | Ruolo |
|------|--------|
| `PegLib/pegdrmoutput.cpp` / `.h` | Opzione D — dumb buffer RGB565, pageflip, damage tracking |
| `PegLib/pegdrm_evdev.cpp` / `.h` | touch via `/dev/input/event*` (+ filtro multitouch Die Set) |
| `PegLib/peg_crashdiag.cpp` / `.h` | backtrace su SIGSEGV/SIGABRT/SIGBUS |

**Da non contare come “lavoro di test”:** `Makefile`, `PegLib/Makefile`, `.qmake.stash`, regole `.cursor/`.

**Deploy tipico:** `libPegLib.so*` (+ `PegExec` se ricompilato) → `/opt/Squeeze/`.

---

### 2 — `pressbrakepeg` (stabilità Calculate / Optimize)

Working tree tipicamente su branch `lvgl-hmi`. Introdotto durante le campagne di validazione Test 6 (crash Calculate, freeze Optimize, diag).

#### Macro / diagnostica / freeze Optimize

| File | Ruolo |
|------|--------|
| `IncPPG/CommonConst.h` | macro `CAD_DIAG` / `CAD_DIAG_CALCULATE` |
| `ottimizzatore/ottimizzatore.pro` | `DEFINES += CAD_DIAG_CALCULATE` (debug; togliere in prod) |
| `ottimizzatore/Ottcomp.cpp` | trace ciclo ottimizzatore |
| `ottimizzatore/Ottinit.cpp` | trace init |
| `ottimizzatore/Ottpunti.cpp` | trace punti |
| `ottimizzatore/Ottutens.cpp` | trace `LookForBends`, … |
| `sim2d/sim2d.pro` | `DEFINES += CAD_DIAG_CALCULATE` (debug; togliere in prod) |
| `sim2d/Ottimdlg.cpp` | dialog Optimize / `THR_ENDSOL` |
| `sim2d/Sim2DExport.cpp` | trace Calculate / export |
| `sim2d/Sim2DFrame.cpp` | freeze Optimize / OnOttimizza |
| `sim2d/Sim2DView.cpp` | `PolyLinePez` / clamp `MAX_ELEM_PERM` |
| `cad2d/PezzoFrame.cpp` | trace / path Calculate |
| `cad2d/Pezzoview.cpp` | vista pezzo |
| `cad2d/Ppgdoc.cpp` | documento CAD |
| `cad2d/StdAfx.cpp` | init / hook condivisi |

#### Guard null / crash Calculate

| File | Ruolo |
|------|--------|
| `cad2d/DraftPieceWnd.cpp` / `.h` | lifetime / null draft window |
| `cad2d/PezzoDoc.cpp` | bounds-check vista / file |
| `cad2d/PezzoForm.cpp` | `EditDraw` / `RecGrafPtr` sicuro |
| `cad2d/PezzoFormLAlpha.cpp` | null check record successivi |

**Deploy tipico:** `libcad2d.so*`, `libsim2d.so*`, `libottimizzatore.so*` → `/opt/Squeeze/`.  
Vedi anche [DIAGNOSTICA TEST 6](#diagnostica-test-6) per spegnere `CAD_DIAG` in produzione.

---

### 3 — `kvuib` (experiment font #2)

Branch: `experiment/test6-with-new-font`. Commit tipico: *togliere bitmap Yahei embedded ridondanti*.

Rimozione di `Yahei_N.cpp` (morto in build; runtime CHS usa già `.gz`) da:

| File |
|------|
| `PegFontChs8/PegFontChs8.pro` |
| `PegFontChs9/PegFontChs9.pro` |
| `PegFontChs10/PegFontChs10.pro` |
| `PegFontChs11/PegFontChs11.pro` |
| `PegFontChs12/PegFontChs12.pro` |
| `PegFontChs14/PegFontChs14.pro` |
| `PegFontChs16/PegFontChs16.pro` |

Dettaglio misure flash: [Experiment font #2](#experiment-font-2) / [Possibili migliorie §6](#pm-font).

**Deploy tipico:** `libPegFontChs*.so*` aggiornate sull’immagine / `/opt/Squeeze/`.

---

### 4 — Documentazione

| File | Ruolo |
|------|--------|
| `pressa/perf_terminale/registro_test_rt.md` | questo registro (test, diag, banda DDR, inventario) |
| `pressa/perf_terminale/valori_perf.txt` | dump `perf` DDR cycles (riposo / scroll) |
| altri `.md` in `pressa/perf_terminale/` | pipeline, metriche GUI, interferenza CPU/DDR, … |

---

### Come rigenerare l’elenco da git

```bash
# pegenstein: delta Test 0 → HEAD (escludere Makefile)
cd Squeeze/pegenstein
git diff --name-status 7acaf1d..HEAD -- PegLib Files/avn8mp

# pressbrakepeg: working tree campagna stabilità
cd Squeeze/pressbrakepeg
git status --porcelain

# kvuib: font #2
cd Squeeze/kvuib
git diff --name-status lvgl-hmi...HEAD -- 'PegFontChs*/'
```

---

## Note operative

- **Deploy:** copiare sempre `libPegLib.so*`, `PegExec`, **`libcad2d.so*`**, **`libsim2d.so*`**, lib ottimizzatore **e** `rtos.ini` aggiornato in `/opt/Squeeze/`
- **Verifica build:** `strings libPegLib.so | grep -E 'crashdiag|stack_chk_fail|PegLib build|framebuffer PEG'` · `md5sum /opt/Squeeze/libcad2d.so.1.0.0`
- **Trace `[CAD] diag`:** default **off** — vedi [Diagnostica CAD_DIAG](#cad-diag-build). Con trace off: `strings libsim2d.so | grep '\[CAD\] diag'` → **nessun output**
- **Crash Calculate (debug):** riabilitare `DEFINES += CAD_DIAG_CALCULATE` nei tre `.pro` (cad2d, sim2d, ottimizzatore); ultima riga `[CAD] diag` prima del fault indica il punto
- **Crash:** se `core` assente, usare backtrace `[RT] FATAL` su stderr oppure `coredumpctl gdb PegExec`
- **Avvio RT:** non avviare Lnk subito dopo PegExec — attendere **≥ 30 s** (vedi protocollo sopra)
- **Trappola:** log `[RT]` visibili con `.so` vecchio anche se macro commentata → verificare con `strings`
- **Diagnosi SDL:** `strings libPegLib.so | grep '\[RT\] diag'` — se assente, macro `EMBEDDED_HMI_RT_DIAG` non compilata
- **LVGL:** irrilevante per il percorso display — vedi [Possibili migliorie §5](#pm-lvgl-cache)

---

<a id="experiment-font-2"></a>

## O — Experiment font #2 (pulizia PegFontChs, 2026-07-16)

> Sintesi nello stato leve: [Possibili migliorie §6](#pm-font).

### Quale delle 3 ha più senso

**Consiglio: #2 — multilingua + pulizia build.**

| Opzione | Perché sì / no ora |
|---------|--------------------|
| **#1 EU slim** | Risparmio massimo e immediato, ma spegne CHS/KOR/JAP sull'immagine. Ha senso solo se Test 6 resta EU-only. Per una pressa multilingua è troppo aggressivo come primo passo. |
| **#2 pulizia** | **Scelta migliore ora.** Look invariato, lingue intatte, risparmio su flash togliendo ridondanze (Yahei embedded dove il runtime usa `.gz`; MSSong/Light lasciati se ancora referenziati). Misurabile e reversibile. |
| **#3 subset** | Massimo guadagno a lungo termine, ma serve PEG Font Capture (non nel tree) + charset dalle stringhe. Prepararlo dopo #2, non come primo commit. |

**Ordine pratico:** #2 subito → charset/script per #3 dopo → #1 solo se serve un'immagine lab EU-only.

---

### Cos’è il problema delle “ridondanze” (prima)

Per il **cinese (CHS)** l’immagine poteva contenere **due copie** dello stesso font Yahei:

1. **Dentro la libreria** `libPegFontChsN.so` — glifi compilati da file C enormi tipo `Yahei_12.cpp` (centinaia di KB / ~1 MB di bitmap per taglia).
2. **Su disco come file esterno** — `PegFontTypeYaHei12.gz` (e, se ClearStripe, `PegFontTypeYaHei12_CS.gz`).

A runtime, però, `DeskGetFont(YAHEI_CHS_12)` **non usava mai** i glifi dentro la `.so`. In `PegDeskFontChs12.cpp` il percorso è:

- punta a uno **stub vuoto** `static PegFont Yahei12 = {0}` (non ai dati di `Yahei_12.cpp`);
- se `uHeight == 0`, chiama `pegLoadBinFont(...)` e carica i glifi dal **`.gz`**.

Quindi: **stesso font “pieno” presente due volte sull’immagine, ma solo il `.gz` veniva usato.**  
I `Yahei_8.cpp` … `Yahei_16.cpp` linkati nelle `.so` erano **peso morto**: occupavano **flash**, non servivano al path normale di `YAHEI_CHS_*`.

Nella stessa lib c’erano anche altre cose **non** morte:

| Contenuto | Usato a runtime? | Azione #2 |
|-----------|------------------|-----------|
| `Yahei_N.cpp` (embedded) | No — path usa `.gz` | **Rimosso** dalla build |
| `PegFontTypeYaHeiN.gz` | Sì — path CHS normale | Lasciato (serve) |
| `MSSong_N` | Sì — alcuni dialoghi | Lasciato |
| `Yahei_N_Light` | Sì — alcuni path (`bYaweiUI`, allarmi, …) | Lasciato |

```text
PRIMA (ridondanza)
  libPegFontChs12.so  =  MSSong + Yahei_embedded (MORTO) + Yahei_Light + loader
  disco               =  PegFontTypeYaHei12.gz   ← questo sì, usato a runtime

DOPO (#2)
  libPegFontChs12.so  =  MSSong + Yahei_Light + loader
  disco               =  PegFontTypeYaHei12.gz   ← invariato, ancora usato
```

**Nota RAM:** togliere codice mai referenziato riduce soprattutto lo **spazio flash** (~6.3 MB sulle Chs). La RAM “viva” del path CHS resta quella del load dal `.gz` (invariata). Su Linux le pagine della `.so` mai toccate spesso non entravano comunque in RAM fisica.

---

### Cosa abbiamo fatto concretamente (#2)

**Branch:** `experiment/test6-with-new-font` in **kvuib** (twin di pegenstein).

**Modifica:** da `PegFontChs{8,9,10,11,12,14,16}.pro` rimossi i `SOURCES` `Yahei_N.cpp` (con commento che spiega il load da `.gz`).  
`PegFontChs18` era già solo-loader (niente embedded da togliere).

**Effetto:** look e lingue invariati (CHS via `.gz`, EU/Arial non toccati); `.so` più piccole.

**Misura SO (avn8mp Release), prima → dopo:**

| Lib | Prima (byte) | Dopo (byte) | Risparmio (byte) | Risparmio |
|-----|-------------:|------------:|-----------------:|----------:|
| Chs8 | 1 524 064 | 996 288 | 527 776 | 515.4 KB |
| Chs9 | 1 655 496 | 1 062 000 | 593 496 | 579.6 KB |
| Chs10 | 1 984 088 | 1 324 832 | 659 256 | 643.8 KB |
| Chs11 | 2 444 304 | 1 587 896 | 856 408 | 836.3 KB |
| Chs12 | 2 706 808 | 1 719 168 | 987 640 | 964.5 KB |
| Chs14 | 3 757 928 | 2 441 904 | 1 316 024 | 1285.2 KB |
| Chs16 | 4 611 360 | 2 967 112 | 1 644 248 | 1605.7 KB |
| **Totale 7 lib** | **18 684 048** | **12 099 200** | **6 584 848** | **6.28 MB** |

*(Prima = size `.so.1.0.0` prima del relink; Dopo = size dopo rebuild senza `Yahei_N.cpp`, copiate anche in `pressbrakepeg-output/.../opt/Squeeze/`.)*

---

### Memoria effettiva nel caso specifico — cosa contare

Distinguere **tre cose diverse** (altrimenti i numeri si confondono):

#### 1) Flash / spazio immagine (quello che #2 ha ridotto)

| Voce | Prima | Dopo | Delta |
|------|------:|-----:|------:|
| Somma `libPegFontChs{8,9,10,11,12,14,16}.so` | **17.818 MB** (18 684 048 B) | **11.539 MB** (12 099 200 B) | **−6.280 MB** (−6 584 848 B) |

Questo è il **risparmio reale e misurato** di #2: meno byte sul filesystem dell’immagine.

**Non ridotti da #2** (ancora sull’immagine `opt/Squeeze`, misura post-#2):

| Voce | Size |
|------|-----:|
| Tutte le `libPegFont*.so.1.0.0` (base + Chs + Kor + Jap + Rus + Gre + LatinExt) | **~26.00 MB** |
| Solo Yahei `.gz` standard (`PegFontTypeYaHei*.gz` senza `_CS`) | **~4.39 MB** |
| Solo Yahei `.gz` ClearStripe (`*_CS.gz`) | **~20.01 MB** |
| Tutti Yahei `.gz` (standard + CS) | **~24.39 MB** |

I `.gz` **devono** restare: sono la copia **usata** a runtime per `YAHEI_CHS_*`. #2 ha tolto solo il duplicato morto **dentro** le `.so`.

#### 2) RAM processo HMI (RSS) — #2 quasi non cambia

- I `Yahei_N.cpp` erano **mai referenziati** → su Linux spesso **non entravano in RAM fisica** (demand paging: pagine della `.so` non toccate = non faultate).
- Il path CHS continua a fare `pegLoadBinFont` dal `.gz` → quel costo RAM **è invariato**.
- Quindi: **non dichiarare “abbiamo liberato 6.3 MB di RAM”**. Abbiamo liberato **6.3 MB di flash** sulle Chs.

#### 3) “Caso specifico” tipico pressa

| Scenario lingua | Cosa pesa davvero | Effetto #2 |
|-----------------|-------------------|------------|
| **EU / ITA** (Arial) | `libPegFont.so` (~662 KB) + eventualmente LatinExt; lib Chs spesso caricate solo se serve CHS | Flash immagine più leggera; RSS EU tipicamente **uguale** |
| **CHS** | `.gz` Yahei della taglia usata (es. 12 ≈ **522 KB** standard / **~2.4 MB** CS) decompresso/caricato + lib Chs (MSSong + Light ancora dentro) | Flash −6.3 MB sulle Chs; RAM load Yahei **uguale** |

**Come verificarlo sul target (consigliato):**

```bash
# Flash: size file dopo deploy
ls -l /opt/Squeeze/libPegFontChs*.so.1.0.0

# RAM: confrontare RES/PSS del processo PRIMA vs DOPO deploy (stessa lingua/scenario)
# es. smem -P PegExec   oppure   ps -o rss= -p $(pidof PegExec)
```

**Sintesi misura (caso specifico):**  
controllata la memoria font → **flash Chs 17.82 → 11.54 MB (−6.28 MB)**; **RAM runtime del path Yahei invariata** perché i glifi usati restano quelli del `.gz`.

---

**In una frase (tecnica):** prima Yahei “pieno” sia nella `.so` sia nel `.gz`, ma a runtime solo il `.gz` → ridondanza su flash; #2 toglie dalla build la copia morta nella `.so`.

**Audit Arial (solo documentazione, nessun prune):** taglie definite ma senza `DeskGetFont`/`DeskGetBaseFont` nel tree pressbrakepeg+kvuib: `ARIAL_6`, `ARIAL_6G`, `ARIAL_10CB/GB/GCB`, `ARIAL_10CS/GS/GCS`, `ARIAL_40`. Il resto delle ARIAL_* risulta usato.

**Prossimi passi possibili:** subset Unicode (#3) / SKU EU slim (#1) / valutare redirect Light→Yahei.gz se si accetta look diverso.

**Stato:** fatto su branch experiment; redeploy `libPegFontChs*.so` + smoke UI CHS (Yahei da `.gz`) e EU.  
**Rollback:** `git switch` al branch precedente in kvuib → ricompilare Chs → redeploy `.so` vecchie.

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
