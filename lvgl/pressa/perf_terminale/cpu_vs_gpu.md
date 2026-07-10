# Interferenza CPU/DDR durante interazione GUI

> Documento riassuntivo per riunione tecnica.
> Basato su: strumentazione `[RT] uploadDirtyRegion`, misure `perf`, `gc/meminfo`, `top`/`htop`, analisi del codice in `PegLib/`.
> Fonti dati: `registro_test_rt.md`, `osservazioni`, `interferenza_cpu_ddr_idle_vs_interazione.md`.

---

## Legenda metodologica

In tutto il documento distinguiamo tre tipi di affermazione:

| Etichetta | Significato |
|-----------|-------------|
| **Evidenza misurata** | Valore numerico o osservazione diretta su target |
| **Deduzione tecnica** | Conclusione logica da dati + architettura codice |
| **Ipotesi da verificare** | Spiegazione plausibile ma non ancora dimostrata con misura dedicata |

---

## 1. Scopo dell'analisi

### Perché si misura la GUI

Su i.MX8M Plus con Linux PREEMPT_RT, l'interfaccia HMI (`PegExec`) e i task real-time condividono:

- gli stessi **core CPU**;
- la stessa **DDR** (memoria principale);
- la stessa **gerarchia cache** (L1/L2);
- lo stesso **interconnect di bus**.

L'obiettivo non è aumentare gli FPS della GUI, ma **ridurre l'interferenza real-time** (jitter, latenza `rtc_handler_us`, traffico bus) generata dall'interfaccia quando l'operatore interagisce con lo schermo.

### Cosa si vuole distinguere

| Componente | Domanda |
|------------|---------|
| **CPU** | Il processo GUI consuma più tempo di calcolo? |
| **Cache L2** | Aumentano miss, refill e write-back? |
| **Bus** | Aumentano `bus_access` e `bus_cycles`? |
| **DDR** | Aumenta il traffico verso memoria esterna? |
| **GPU** | Cresce o si satura la memoria GPU? |
| **Dirty region** | Quanto pixel viene ridisegnato e copiato per ogni interazione? |

### Concetti chiave (definizioni)

| Termine | Definizione operativa nel nostro sistema |
|---------|------------------------------------------|
| **CPU** | Esegue il codice PEG (disegno widget), SDL (eventi, upload texture), e la logica di integrazione |
| **GPU** | Acceleratore grafico Vivante; usato da SDL con renderer **OpenGL ES2** per compositing e present |
| **Framebuffer software** | Buffer `g_pyBitmap` in **RAM normale** (RGB565, 16 bpp); PEG ci scrive i pixel |
| **Texture SDL** | Buffer grafico gestito da SDL, tipicamente in memoria accessibile alla GPU |
| **Dirty region** | Rettangolo di pixel modificati da PEG, propagato fino a `uploadDirtyRegion()` |
| **Renderer** | Oggetto SDL che gestisce il target di rendering (back buffer logico) |
| **Render target / back buffer** | Superficie su cui `SDL_RenderCopy()` disegna la texture prima del present |
| **Present / pageflip** | `SDL_RenderPresent()` → driver KMSDRM → il frame diventa visibile sul display |
| **DRM/KMS** | Sottosistema Linux/kernel che gestisce mode setting e scanout verso il pannello |
| **DDR** | Memoria DRAM esterna condivisa da CPU, GPU e periferiche |
| **Cache L2** | Cache di secondo livello della CPU; riduce latenza verso DDR |
| **L2 refill** | Ricaricamento di una linea di cache dopo un miss |
| **L2 write-back (wb)** | Scrittura verso livelli inferiori di dati modificati in cache |
| **mem_access** | Contatore PMU: accessi memoria lato CPU (non distingue L1/L2/DDR) |
| **bus_access** | Contatore PMU: accessi al bus/interconnect (più vicino a traffico fuori cache) |

---

## 2. Architettura della pipeline grafica

### Ruolo dei componenti

| Componente | Ruolo | Gestisce widget PEG? |
|------------|-------|----------------------|
| **PEG** | Motore GUI: bottoni, popup, grafici, finestre, stato interfaccia | **Sì** |
| **SDL** | Eventi input, texture, renderer, present verso KMSDRM | **No** |
| **LVGL** | Libreria inizializzata ma **non usata per il display** in questo porting | **No** (display) |
| **DRM/KMS** | Scanout hardware verso il pannello fisico | **No** |

> **Deduzione tecnica:** SDL non è la GUI applicativa. SDL è il layer di integrazione tra il framebuffer software PEG e la pipeline di visualizzazione Linux (texture + GPU + DRM).

### Schema ASCII della pipeline completa

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                         THREAD GUI PEG (EtsGUIThread)                  │
│                                                                          │
│   gpPresentation->Execute()                                              │
│       → messaggi mouse/tastiera/timer                                    │
│       → hit-test, aggiornamento widget                                   │
│       → Invalidate() / Draw() / EndDraw()                                │
│       → scrittura pixel in g_pyBitmap (framebuffer software, RAM)        │
│       → driverDBR → PEG_DoubleBufferingRefresh()                         │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                      THREAD MAIN (PEG_internalWinMain)                 │
│                                                                          │
│   request_update()  ←── dirty region (left, top, right, bottom)          │
│       ↓                                                                  │
│   processPendingUpdates()                                                │
│       ↓                                                                  │
│   uploadDirtyRegion()          ←── copia CPU: framebuffer → texture    │
│       ↓                                                                  │
│   SDL_UpdateTexture()          ←── (interno a uploadDirtyRegion)       │
│       ↓                                                                  │
│   flushPresent()                                                         │
│       ↓                                                                  │
│   SDL_RenderCopy()             ←── texture → render target (GPU)        │
│       ↓                                                                  │
│   SDL_RenderPresent()          ←── pageflip / present                    │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────────┐
│   SDL video driver KMSDRM  →  DRM/KMS  →  imx-drm  →  Display fisico   │
└─────────────────────────────────────────────────────────────────────────┘
```

### Stack attivo in produzione (path 5b)

Con macro `EMBEDDED_HMI_RT_NATIVE_TEXTURE` attiva:

```text
PEG RGB565 (RAM) → SDL_ConvertPixels (CPU) → ARGB8888 staging → SDL_UpdateTexture → texture GLES2 → RenderCopy → RenderPresent → DRM/KMS
```

**Evidenza misurata (Test 5):** su renderer OpenGL ES2, RGB565 **non** è formato nativo; SDL avvisa di possibile `SDL_ConvertPixels` nascosta in `SDL_UpdateTexture`.

---

## 3. Cosa succede quando l'utente tocca lo schermo

### Sequenza passo-passo

#### Fase A — Input (thread main, ~100 Hz con `Sleep(10)`)

| Passo | Funzione | Cosa fa |
|------:|----------|---------|
| 1 | `SDL_PollEvent()` in `processEvents()` | Legge eventi touch/mouse da evdev/SDL |
| 2 | `mapWindowToFramebuffer()` | Converte coordinate finestra → coordinate framebuffer PEG |
| 3 | `emitMouseEvent()` | Prepara evento con flag `MOUSE_MOVED`, bottoni, coordinate assolute |
| 4 | `PegMouseMapping()` | Traduce in `ETS_INPUT_RECORD` e invoca `__p_EtsInvokeCallback()` |

**Deduzione tecnica:** il touch non aggiorna direttamente i widget. Passa attraverso una callback che inserisce l'evento nel sistema di input PEG (thread separato).

#### Fase B — Logica GUI (thread `EtsGUIThread`)

| Passo | Funzione | Cosa fa |
|------:|----------|---------|
| 5 | `EtsGUIThread()` → `gpPresentation->Execute()` | Loop principale PEG: processa messaggi |
| 6 | Hit-test, gestione bottoni/popup/grafici | PEG decide cosa ridisegnare |
| 7 | `Invalidate(rect)` | Marca area da ridisegnare |
| 8 | `Draw()` / `EndDraw()` | Scrive pixel nel framebuffer `g_pyBitmap` |
| 9 | `PegScreen16::EndDraw()` → `driverDBR()` | Notifica area dirty al driver |
| 10 | `PEG_DIB16_DoubleBufferingRefresh()` → `PEG_DoubleBufferingRefresh()` | Chiama `request_update(Left, Top, Right, Bottom)` |

#### Fase C — Upload e present (thread main)

| Passo | Funzione | Cosa fa |
|------:|----------|---------|
| 11 | `mergeDirtyRegion()` in `request_update()` | Unisce rettangoli dirty in un bounding box |
| 12 | `processPendingUpdates()` | Legge dirty region e chiama upload |
| 13 | `uploadDirtyRegion()` | Calcola pitch, puntatori, area; copia verso texture |
| 14 | `SDL_UpdateTexture()` | Upload effettivo framebuffer → texture |
| 15 | `flushPresent()` | Se pending, esegue RenderCopy + RenderPresent |
| 16 | `pumpLvgl()` → `lv_timer_handler()` | Timer LVGL (non display path) |

### Loop principale (codice)

In `peg_run.cpp`, `PEG_internalWinMain()` per `PEG_USE_LVGL`:

```text
while (!peg_video_window->shouldClose())
{
    peg_video_window->processEvents();        // touch/mouse
    peg_video_window->processPendingUpdates(); // uploadDirtyRegion
    peg_video_window->flushPresent(false);     // RenderCopy + RenderPresent
    peg_video_window->pumpLvgl();              // lv_timer_handler
    Sleep(10);                                 // ~100 Hz
}
```

**Deduzione tecnica:** ogni ciclo del thread main può generare upload e present. L'interazione aumenta la frequenza e l'ampiezza delle dirty region, quindi più chiamate a `uploadDirtyRegion()` con rettangoli più grandi.

---

## 4. Analisi del codice

### 4.1 Collegamento PEG → dirty region

In `GuiScr/screen.cpp`, `PegScreen16::EndDraw()`:

- quando `miInvalidCount > 0`, calcola il rettangolo invalidato `IR`;
- interseca con lo schermo (`CopyRect &= IR`);
- chiama `driverDBR(CopyRect.wLeft, CopyRect.wTop, CopyRect.wRight, CopyRect.wBottom)`.

Il driver DIB16 (`Video/dib16.cpp`) registra:

```text
PEG_DIB16_DoubleBufferingRefresh() → PEG_DoubleBufferingRefresh()
```

In `peg_run.cpp`, `PEG_DoubleBufferingRefresh()` (path `PEG_USE_LVGL`):

```text
peg_video_window->request_update(Left, Top, Right, Bottom);
```

### 4.2 Accumulo dirty region

`PegLvglWindow::request_update()` chiama `mergeDirtyRegion()`, che espande il bounding box `(m_dirtyLeft, m_dirtyTop, m_dirtyRight, m_dirtyBottom)`.

**Deduzione tecnica:** più invalidazioni PEG nello stesso ciclo vengono fuse in un unico rettangolo. Questo può **amplificare** l'area copiata se widget distanti vengono invalidati nello stesso frame.

### 4.3 uploadDirtyRegion() — cuore dell'integrazione

`uploadDirtyRegion()` in `peglvglwindow.cpp` **non è una funzione SDL standard**. È codice di porting che:

1. Clampa le coordinate al framebuffer;
2. Costruisce `SDL_Rect rect` e calcola `src` (puntatore nel framebuffer PEG);
3. Calcola `pitch = m_width * bytesPerPixel()`;
4. (Path 5b) esegue `SDL_ConvertPixels(RGB565 → ARGB8888)` su buffer staging;
5. Chiama `SDL_UpdateTexture(m_texture, &rect, buffer, pitch)`;
6. Con `EMBEDDED_HMI_RT_STATS`, accumula `calls`, `req`, `updateMs`, `maxRectPx`.

**Evidenza dal codice:** `updateMs` misura il tempo **dentro** la chiamata di upload (include `SDL_ConvertPixels` + `SDL_UpdateTexture`), non il solo present.

### 4.4 flushPresent() — separazione upload / present

Su embedded (`EMBEDDED_HMI`), il commento in `processPendingUpdates()` dice esplicitamente:

> *Su embedded aggiorniamo solo la texture; il pageflip avviene in flushPresent().*

`flushPresent()`:

- controlla `m_pendingPresent`;
- rispetta un intervallo minimo `kMinKmsdrmPresentIntervalMs` (~60 Hz);
- esegue `SDL_RenderClear` + `SDL_RenderCopy` + `SDL_RenderPresent`.

**Deduzione tecnica:** l'upload texture e il present sono **fasi distinte**. L'interferenza misurata da `updateMs` riguarda la fase CPU di copia verso texture, non solo il pageflip GPU.

### 4.5 Ruolo di LVGL

`pumpLvgl()` chiama solo `lv_timer_handler()`.

**Evidenza dal codice:** LVGL è inizializzato (`lv_init()` in `initialize()`) ma **non gestisce widget né display** in questo porting. Non va citato come motore GUI dell'interfaccia.

### 4.6 Thread coinvolti

| Thread | Funzione principale | File |
|--------|---------------------|------|
| **Main** | Eventi, upload, present, LVGL timer | `peg_run.cpp` → `PEG_internalWinMain()` |
| **EtsGUIThread** | Loop PEG `Execute()`, Draw, Invalidate | `GuiScr/etspeg.cpp` |
| **PegRefreshDaemon** | Refresh periodico PEG | `peg_run.cpp` |

---

## 5. Perché il carico dell'interfaccia è prevalentemente lato CPU/memoria e non GPU

> **Questa è la sezione centrale del documento.**

### 5.1 Premessa corretta: la GPU viene usata

**Evidenza dal codice e dai log diagnostici (Test 5):**

- video driver SDL = **KMSDRM**;
- renderer = **opengles2** (GPU Vivante);
- `SDL_RenderCopy()` e `SDL_RenderPresent()` sono chiamate ad ogni present;
- il frame raggiunge il display tramite **DRM/KMS**.

> **Conclusione parziale:** la pipeline display coinvolge la GPU e il sottosistema DRM. Non è corretto affermare che "la GPU non viene usata".

### 5.2 Cosa NON emerge dai dati: saturazione memoria GPU

**Evidenza misurata** (`/sys/kernel/debug/gc/meminfo`):

| Scenario | POOL SYSTEM Used | POOL VIRTUAL Used | Interpretazione |
|----------|-----------------:|------------------:|-----------------|
| Interfaccia **spenta** | **21 520 B** | **0 B** | Memoria GPU quasi nulla |
| Interfaccia **attiva** | **8 652 920 B** (~8,3 MiB) | **7 389 184 B** (~7,0 MiB) | Allocazione risorse grafiche all'avvio |
| **Interazione/touch** | **stabile** (~8,3 MiB) | **stabile** (~7,0 MiB) | Nessuna crescita dinamica |

Pool totale GPU: **268 435 456 B** (256 MiB).

**Deduzione tecnica:** la GUI alloca texture e risorse GPU all'avvio (~8 MiB su 256 MiB, ~3,2% del pool), ma **non si osserva crescita** di memoria GPU durante touch, scroll o drag. Il problema misurato **non** è "memoria GPU piena".

### 5.3 Cosa emerge dai dati: aumento contatori CPU/cache/bus nel processo PegExec

**Evidenza misurata** (`perf stat -p $PID_PegExec -I 1000`, da `osservazioni`):

| Contatore | Interfaccia ferma | Interazione touch | Variazione |
|-----------|------------------:|------------------:|-----------|
| `l2d_cache` | ~2 M/s | ~6–8 M/s | ×3–4 |
| `l2d_cache_refill` | ~0,3–0,4 M/s | ~1,5–2 M/s | ×4–5 |
| `l2d_cache_wb` | ~40–50 k/s | ~600–980 k/s | ×12–20 |
| `bus_access` | ~1,2–2,4 M/s | ~6–9 M/s | ×3–5 |
| `bus_cycles` | (proporzionale) | (proporzionale) | aumenta |
| `mem_access` | ~13–17 M/s | ~65–90 M/s | ×4–6 |

**Deduzione tecnica:** i contatori sono **process-specific** (`-p $PID`). L'aumento durante l'interazione indica che è **proprio PegExec** a generare più accessi memoria, più miss/refill L2, più write-back e più traffico bus — non un altro processo del sistema.

### 5.4 Conferma a livello sistema: traffico DDR globale

**Evidenza misurata** (`perf stat -a -I 1000`):

| Contatore | Interfaccia ferma | Interazione | Note |
|-----------|------------------:|------------:|------|
| `imx8_ddr0/write-accesses` | ~600–800 k/s | picchi **>2–3 M/s** | Aumento scritture DDR globali |
| `imx8_ddr0/read-accesses` | **sempre 0** | **sempre 0** | **Non affidabile** in questa config |

> **Attenzione:** `read-accesses = 0` **non** significa che la DDR non viene letta. Significa che il contatore del controller DDR non sta riportando letture in questa configurazione. Va usato solo `write-accesses` come indicatore relativo.

### 5.5 Conferma diretta: strumentazione uploadDirtyRegion()

**Evidenza misurata** (Test 0, risoluzione effettiva ~960×640):

| Scenario | calls/s | reqMBps | updateMs/s | maxRectPx | % schermo |
|----------|--------:|--------:|-----------:|----------:|----------:|
| Idle | 7–9 | 0,75–1,0 | 22–30 | ~66 912 | ~11% |
| Touch generico | 14–20 | 5–8 | 60–97 | fino a 614 400 | 100% |
| Popup | 4–8 | 0,67–3,2 | 16–37 | fino a 370 688 | ~60% |
| **Drag grafico** | **~33** | **~24** | **208–220** | **485 051** | **~79%** |

**Deduzione tecnica:** `updateMs` misura il tempo CPU speso dentro l'upload texture. Con `updateMs ≈ 210 ms/s`, il processo dedica **~21% di un core** equivalente solo alla fase di copia (formula sotto).

### 5.6 Perché questo carico è CPU-side e non GPU-side

| Fase | Eseguita da | Tipo di lavoro |
|------|-------------|----------------|
| PEG `Draw()` / scrittura framebuffer | **CPU** | Compute + store in RAM |
| Calcolo dirty region, pitch, puntatori | **CPU** | Logica + accessi memoria |
| `SDL_ConvertPixels()` (path 5b) | **CPU** | Conversione formato pixel |
| `SDL_UpdateTexture()` | **CPU** (+ bus verso memoria GPU) | **Copia/memcpy** da RAM a texture |
| `SDL_RenderCopy()` | **GPU** | Compositing texture → back buffer |
| `SDL_RenderPresent()` / pageflip | **GPU + kernel DRM** | Scanout |

> **Deduzione tecnica:** le fasi che **variano di più** con l'interazione (freq. upload, volume byte, tempo in `uploadDirtyRegion`) sono tutte **CPU-side** o **memoria/bus-side**. Le fasi GPU (RenderCopy/Present) esistono ma non mostrano correlazione con crescita memoria GPU.

### 5.7 Conferma indipendente: pipeline DRM diretta (Test 6)

**Evidenza misurata:** con output DRM dumb-buffer (bypass texture SDL/GPU), il pattern idle→interazione si ripete:

| Condizione | %CPU PegExec |
|------------|-------------:|
| Idle | 7,9% |
| Interazione | 38,4% |
| **Δ** | **+30,5 pp** |

**Deduzione tecnica:** il salto di CPU avviene **anche senza** SDL texture/GPU. La causa comune è la **copia di pixel dal framebuffer software** (dirty region grandi e frequenti), non un artefatto specifico di OpenGL ES2.

### 5.8 Sintesi della sezione 5

| Affermazione | Tipo | Valido? |
|-------------|------|---------|
| "La GPU non viene usata" | — | **No** (falso) |
| "La memoria GPU si satura durante il touch" | Evidenza | **No** (non osservato) |
| "PegExec genera più traffico memoria/cache/bus durante il touch" | Evidenza | **Sì** |
| "Il collo di bottiglia variabile è la copia CPU framebuffer → texture" | Deduzione | **Sì** (coerente con codice + perf + uploadDirtyRegion) |
| "L'interferenza RT è principalmente effetto memoria/bus, non GPU memory" | Deduzione | **Sì** |

---

## 6. Dati GPU meminfo

### 6.1 Interfaccia spenta

```text
POOL SYSTEM:
  Used :            21520 B
POOL VIRTUAL:
  Used :                0 B
```

**Interpretazione (evidenza):** senza GUI, la GPU non alloca risorse grafiche significative.

### 6.2 Interfaccia attiva

```text
POOL SYSTEM:
  Used :          8652920 B   (~8,3 MiB)
POOL VIRTUAL:
  Used :          7389184 B   (~7,0 MiB)
```

**Interpretazione (evidenza):** all'avvio della GUI, SDL/GPU allocano texture e risorse. Uso contenuto: ~3,2% del pool da 256 MiB.

### 6.3 Durante interazione/touch

**Evidenza misurata:** i valori `Used` restano **stabili** durante touch, scroll e drag.

> **Conclusione:** la GPU è coinvolta nella pipeline, ma **non ci sono evidenze** di crescita dinamica della memoria GPU durante l'interazione. Il monitoraggio `gc/meminfo` misura **allocazioni**, non **utilizzo computazionale** della GPU.

---

## 7. Dati perf sul processo PegExec

### 7.1 Significato dei contatori

| Contatore | Cosa misura | Perché aumenta durante GUI |
|-----------|-------------|--------------------------|
| `mem_access` | Accessi memoria lato CPU | Più load/store da Draw, memcpy, SDL |
| `l2d_cache` | Accessi alla cache L2 | Più dati attraversano la cache |
| `l2d_cache_refill` | Miss L2 (ricaricamento linee) | Working set più grande, meno locality |
| `l2d_cache_wb` | Write-back verso livelli inferiori | Più pixel scritti nel framebuffer e copiati |
| `bus_access` | Accessi al bus/interconnect | Traffico oltre la cache |
| `bus_cycles` | Cicli di bus consumati | Correlato a `bus_access` |

### 7.2 Tabella comparativa (da `osservazioni`)

| Contatore | Fermo | Touch | Rapporto |
|-----------|------:|------:|---------:|
| `l2d_cache` | ~2 M/s | ~6–8 M/s | ~3–4× |
| `l2d_cache_refill` | ~0,3–0,4 M/s | ~1,5–2 M/s | ~4–5× |
| `l2d_cache_wb` | ~40–50 k/s | ~600–980 k/s | ~12–20× |
| `bus_access` | ~1,2–2,4 M/s | ~6–9 M/s | ~3–5× |
| `mem_access` | ~13–17 M/s | ~65–90 M/s | ~4–6× |

### 7.3 Collegamento al codice

Durante l'interazione, il thread PEG esegue più cicli `Draw()`→`EndDraw()`→`driverDBR()`, e il thread main esegue più `uploadDirtyRegion()` con rettangoli più grandi. Ogni operazione:

- legge e scrive nel framebuffer (`g_pyBitmap`) in RAM;
- copia blocchi di pixel verso la texture (`SDL_UpdateTexture`);
- (path 5b) converte formato pixel (`SDL_ConvertPixels`).

**Deduzione tecnica:** questo pattern di accessi sequenziali e ripetuti a grandi buffer è esattamente il tipo di workload che incrementa `mem_access`, `l2d_cache_refill`, `l2d_cache_wb` e `bus_access`.

### 7.4 Dati top: CPU processo vs memoria residente

**Evidenza misurata** (risoluzione produzione 1024×600, `top`):

| Condizione | %CPU PegExec | RES PegExec |
|------------|-------------:|------------:|
| Idle | 7,3% | 195 744 KB |
| Interazione (drag) | 30,4% | 196 256 KB |
| **Δ** | **+23,1 pp** | **+0,26%** |

**Deduzione tecnica:** la CPU cresce di oltre 4×; la RAM residente resta piatta. Il carico aggiuntivo è **attività runtime** (copie, accessi memoria), non nuove allocazioni.

---

## 8. Dati DDR globali

### 8.1 write-accesses

**Evidenza misurata:**

| Stato | `imx8_ddr0/write-accesses` |
|-------|---------------------------:|
| Interfaccia ferma | ~600–800 k/s |
| Interazione | picchi **>2–3 M/s** |

**Deduzione tecnica:** l'interazione GUI contribuisce al traffico DDR **globale** del sistema, in particolare in scrittura. Questo è coerente con: scrittura framebuffer PEG + copia verso texture + write-back cache.

### 8.2 read-accesses sempre zero

**Evidenza misurata:** `imx8_ddr0/read-accesses` = 0 in tutte le misure.

> **Non è una evidenza di assenza di letture DDR.** È un'**anomalia del contatore** nella configurazione attuale. Le letture DDR avvengono (ogni load dal framebuffer, ogni refill cache), ma non sono contate da questo evento PMU.

---

## 9. Dati uploadDirtyRegion

### 9.1 Significato delle metriche

| Metrica | Definizione | Utilità |
|---------|-------------|---------|
| **calls** | Numero di chiamate a `uploadDirtyRegion()` nell'intervallo (~1 s) | Frequenza upload |
| **req** | MB totali copiati nell'intervallo | Volume dati |
| **reqMBps** | `req` / durata intervallo | Banda grafica richiesta (MB/s) |
| **updateMs** | Tempo totale dentro upload (ConvertPixels + UpdateTexture) | **Tempo CPU diretto** |
| **effMBps** | `req` / tempo dentro upload | Velocità copia durante esecuzione |
| **maxRectPx** | Area max (px) della dirty region nell'intervallo | Ampiezza redraw |

### 9.2 Formule

```text
areaPx = rect.w × rect.h

bytes = rect.w × rect.h × bytesPerPixel

bytesPerPixel = 2    (RGB565)

Full screen 960×640:
  960 × 640 = 614 400 pixel
  614 400 × 2 = 1 228 800 byte ≈ 1,17 MiB

percentuale = maxRectPx / 614 400 × 100

core_equiv_percent = updateMs / 1000 × 100

Esempio: updateMs = 210 ms/s → 210/1000 × 100 = 21% di un core equivalente
```

### 9.3 Scenari misurati

#### Idle (riposo)

| Metrica | Valore | Interpretazione |
|---------|--------|-----------------|
| calls/s | 7–9 | Refresh leggero (grafico animato, clock, ecc.) |
| reqMBps | 0,75–1,0 | Banda bassa |
| updateMs/s | 22–30 | ~2–3% core equivalente |
| maxRectPx | ~66 912 | ~11% schermo |

**Tipo:** carico di background normale.

#### Cambio schermata (touch generico)

| Metrica | Valore | Interpretazione |
|---------|--------|-----------------|
| maxRectPx | fino a **614 400** | Full screen 960×640 |
| reqMBps | 5–8 | Picco transitorio |

**Tipo:** carico transitorio **giustificato** se l'intera interfaccia cambia.

#### Popup piccolo

| Metrica | Valore | Interpretazione |
|---------|--------|-----------------|
| calls/s | 4–8 | Frequenza moderata |
| reqMBps | 0,67–3,2 | Volume variabile |
| updateMs/s | 16–37 | Tempo upload contenuto |
| maxRectPx | fino a **370 688** | **~60% schermo** |

> **Ipotesi da verificare:** un popup visivamente piccolo invalida un'area molto più grande del necessario (bounding box PEG troppo ampio). Vale la pena loggare le coordinate `(x, y, w, h)` del `maxRect`.

#### Grafico trascinato (scenario critico)

| Metrica | Valore | Interpretazione |
|---------|--------|-----------------|
| calls/s | **~33** | ~30 Hz di upload |
| reqMBps | **~24** | ~24 MB/s di pixel RGB565 |
| updateMs/s | **208–220** | **~21% core** solo in upload |
| maxRectPx | **485 051** | **~79% schermo** |

**Log esempio:**

```text
[RT] uploadDirtyRegion: calls=33 req=24.74MB reqMBps=24.09 updateMs=216.366 effMBps=114.4 maxRectPx=485051
```

**Tipo:** carico **continuo** durante drag — scenario più critico per interferenza RT.

### 9.4 Come leggere reqMBps vs effMBps vs updateMs

| Osservazione | Significato |
|-------------|-------------|
| `reqMBps` alto | Tanti pixel copiati al secondo |
| `updateMs` alto | Tanto tempo CPU speso in upload |
| `effMBps` alto | La copia è veloce *mentre* è in esecuzione |
| `effMBps` alto + `updateMs` alto | Blocchi grandi copiati velocemente ma troppo spesso → sistema comunque stressato |
| `maxRectPx` alto su cambio schermo intero | **Atteso** |
| `maxRectPx` alto su popup piccolo | **Sospetto** — possibile over-invalidation |

---

## 10. Interpretazione finale

### Cosa i dati dicono

| Domanda | Risposta | Tipo |
|---------|----------|------|
| La GPU è nella pipeline? | Sì (opengles2 + KMSDRM + RenderCopy/Present) | Evidenza codice + log |
| La memoria GPU cresce durante il touch? | No, resta stabile (~8 MiB) | Evidenza misurata |
| PegExec fa più accessi memoria/cache/bus? | Sì, incremento 3–6× | Evidenza misurata |
| Il traffico DDR globale (write) aumenta? | Sì | Evidenza misurata |
| L'upload texture consuma tempo CPU? | Sì, fino a ~21% core in drag | Evidenza misurata |
| È CPU compute puro (ALU)? | No, RES stabile e profilo coerente con memcpy | Deduzione |

### Cosa i dati NON dicono (limiti)

| Limite | Nota |
|--------|------|
| Utilizzo % GPU engine | `gc/meminfo` non misura compute GPU |
| Correlazione diretta upload → `rtc_handler_us` | Misurati separatamente; correlazione temporale da approfondire |
| `read-accesses` DDR | Contatore non affidabile |
| Causa esatta di `maxRectPx` ampio su popup | Ipotesi bounding box; serve log coordinate |

### Sintesi tecnica

> L'interferenza osservata va interpretata principalmente come **effetto del traffico memoria generato dalla pipeline grafica CPU-side** (PEG → framebuffer RAM → SDL_UpdateTexture), più che come problema di **occupazione memoria GPU**.
>
> Non è "CPU compute puro" nel senso di calcolo floating-point intensivo: è **traffico memoria/cache/bus** prodotto da ridisegno widget, scrittura framebuffer e copia verso texture.

---

## 11. Implicazioni per ottimizzazione

Priorità suggerite, ordinate per impatto atteso su interferenza RT:

| # | Azione | Razionale | Tipo |
|---|--------|-----------|------|
| 1 | **Ridurre dirty region non necessarie** | Meno pixel = meno byte = meno bus/cache | Deduzione |
| 2 | **Evitare bounding box troppo ampi** (merge eccessivo) | `mergeDirtyRegion()` amplifica l'area | Deduzione codice |
| 3 | **Limitare refresh grafico durante drag** | Scenario critico: 33 calls/s, 79% schermo | Evidenza |
| 4 | **Separare parti statiche/dinamiche del grafico** | Solo l'area che cambia dovrebbe essere dirty | Ipotesi |
| 5 | **Coalescing eventi motion** (`EMBEDDED_HMI_RT_SAFE`) | Testato: nessun beneficio (redraw ~30 Hz da PEG) | Evidenza Test 2 |
| 6 | **Texture formato nativo** (`EMBEDDED_HMI_RT_NATIVE_TEXTURE`) | Test 5b: −22% updateMs, −64% bus_access | Evidenza |
| 7 | **Legare upload al present** | Già separati (`processPendingUpdates` / `flushPresent`) | Evidenza codice |
| 8 | **Loggare coordinate maxRect** (x, y, w, h) | Diagnosticare popup con area 60% schermo | Ipotesi |
| 9 | **Isolamento CPU/IRQ** dal core RT | Non ancora testato; potenziale per gli ultimi ~23 µs | Ipotesi |
| 10 | **Riduzione risoluzione** (800×600) | Test 4: rtc_handler_us 97 µs vs 122 µs | Evidenza |

---

## 12. Conclusione pronta per riunione

L'interfaccia utilizza la pipeline grafica fino a SDL/DRM/KMS e quindi la GPU/display pipeline è coinvolta nel present del frame.

Tuttavia, le misure raccolte **non indicano** una crescita o saturazione della memoria GPU durante l'interazione:

- a GUI spenta: ~21 KB GPU;
- a GUI attiva: ~8,3 MiB GPU (su 256 MiB pool);
- durante touch/scroll: **valori stabili**.

Al contrario, i contatori PMU del processo `PegExec` mostrano che l'interazione aumenta sensibilmente:

- `mem_access` (×4–6);
- `l2d_cache_refill` (×4–5);
- `l2d_cache_wb` (×12–20);
- `bus_access` (×3–5);

contemporaneamente a un aumento di `imx8_ddr0/write-accesses` a livello sistema.

La strumentazione `uploadDirtyRegion()` conferma il collegamento: durante il drag del grafico, il processo copia ~24 MB/s di pixel, spendendo ~210 ms/s di CPU solo nella fase di upload texture, su dirty region che coprono ~79% dello schermo.

Questo è coerente con una pipeline in cui:

1. **PEG** disegna i widget in un **framebuffer software in RAM** (CPU);
2. il thread main **copia le dirty region** verso una texture SDL tramite `SDL_UpdateTexture()` (CPU + bus + DDR);
3. la GPU esegue il compositing finale (`RenderCopy`/`Present`) su un'area già preparata.

**L'interferenza real-time osservata va quindi interpretata principalmente come effetto del traffico memoria/cache/bus generato dalla pipeline grafica CPU-side, più che come problema di occupazione memoria GPU.**

---

## Riferimenti codice

| File | Funzioni / elementi analizzati |
|------|-------------------------------|
| `PegLib/peg_run.cpp` | `PEG_internalWinMain()`, `PEG_DoubleBufferingRefresh()`, `PegMouseMapping()`, loop main |
| `PegLib/peglvglwindow.cpp` | `processEvents()`, `processPendingUpdates()`, `uploadDirtyRegion()`, `flushPresent()`, `request_update()`, `pumpLvgl()`, `emitMouseEvent()` |
| `PegLib/peglvglwindow.h` | Dichiarazioni API `PegLvglWindow` |
| `PegLib/GuiScr/screen.cpp` | `PegScreen16::EndDraw()`, `driverDBR` |
| `PegLib/GuiScr/etspeg.cpp` | `EtsGUIThread()`, `gpPresentation->Execute()` |
| `PegLib/Video/dib16.cpp` | `PEG_DIB16_DoubleBufferingRefresh()` |
| `PegLib/source/pstchart.cpp` | `Invalidate()` su grafici (esempio dirty region ampia) |

## Riferimenti dati

| File | Contenuto |
|------|-----------|
| `registro_test_rt.md` | Cronologia test 0–6, metriche, confronti |
| `osservazioni` | Tabelle perf, gc/meminfo, comandi misura |
| `interferenza_cpu_ddr_idle_vs_interazione.md` | Argomento CPU/DDR vs GPU (base) |
| `analisi_metriche_gui_rt.md` | Definizione dettagliata metriche |
