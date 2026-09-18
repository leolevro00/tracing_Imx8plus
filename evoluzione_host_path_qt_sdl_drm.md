# Evoluzione dell'host path grafico: Qt → SDL → DRM diretto

**Data:** 2026-09-16
**Scopo:** ricostruire con precisione le tre generazioni di "host path" dell'HMI, cosa
cambia fra l'una e l'altra, e in particolare **che cosa fece la versione intermedia**
(sviluppata da Tommaso) — il cui nome corrente, `lvgl+sdl`, è fuorviante.
**Metodo:** analisi del codice in sola lettura su `/home/leolevro/Sqvara-lvgl/Squeeze`,
incrociata con `registro_test_rt.md`. Nessun file sorgente è stato modificato.

> **Nota sui numeri di riga.** I riferimenti a `peglvglwindow.cpp` si riferiscono al file
> **con la patch `[AI-DIRTY]` applicata** (34 righe aggiunte in totale: 13 nel namespace
> anonimo, 21 in `processPendingUpdates()`). Dopo la rimozione della patch andranno
> rivisti. Gli ancoraggi stabili sono i **nomi di funzione**, non i numeri.

---

## 0. In una frase

Ad ogni passo si toglie uno strato che decide al posto dell'applicazione: prima
l'event loop e il compositor di Qt, poi la texture e la GPU di SDL — finché
l'applicazione possiede direttamente il buffer di scanout e decide lei quando
fare il pageflip.

| Stadio | Host | Ritardo max | Chi controlla il present |
|---|---|---:|---|
| Soluzione iniziale | Qt | 450 µs ⚠️ | Qt (event loop + compositor) |
| Situazione intermedia | SDL | 130 µs | l'app, ma via texture GPU |
| Situazione attuale | DRM/KMS diretto | 98–99 µs | l'app, direttamente |

⚠️ Il 450 µs non è documentato nel registro — vedi §6.

---

## 1. Il punto fermo: chi disegna non cambia mai

In **tutte e tre** le configurazioni è sempre **PEG** a disegnare, in **software**,
dentro il bitmap in RAM `g_pyBitmap`. La GPU non partecipa mai alla rasterizzazione
dei widget.

Quello che cambia fra i tre path è **soltanto** chi porta quei pixel dal
`g_pyBitmap` al pannello LVDS, e quanti strati di intermediazione ci sono in mezzo.

Questo è il punto che rende confrontabili i tre stadi: il lavoro di disegno è
identico, varia solo il percorso di uscita.

---

## 2. La scoperta: LVGL non partecipa al rendering

### 2.1 L'evidenza

In tutto `PegLib` le API LVGL effettivamente usate sono **tre**, su quattro
punti di chiamata:

| Chiamata | Posizione | Contesto |
|---|---|---|
| `lv_init()` | `peglvglwindow.cpp:449` | ramo DRM |
| `lv_init()` | `peglvglwindow.cpp:488` | ramo SDL |
| `lv_deinit()` | `peglvglwindow.cpp:614` | cleanup |
| `lv_timer_handler()` | `peglvglwindow.cpp:953` | dentro `pumpLvgl()` |

`pumpLvgl()` è chiamata nel main loop (`peg_run.cpp:1516`, ogni ~10 ms) ed è
interamente questo:

```cpp
void PegLvglWindow::pumpLvgl()
{
    lv_timer_handler();
}
```

### 2.2 Cosa manca

Un `grep` su `PegLib/*.cpp` e `*.h` non trova **nessuna** occorrenza di:

- `lv_disp_draw_buf_init`, `lv_disp_drv_register`, `lv_display_*` → **nessun display registrato**
- `lv_indev_drv_register` → **nessun input**
- `lv_tick_inc` → **nessuna base tempi**
- `lv_obj_*`, `lv_draw_*` → **nessun widget, nessun disegno**

Riscontro oggettivo: l'oggetto compilato `peglvglwindow.o` importa solo i simboli
`lv_init`, `lv_deinit`, `lv_timer_handler`. Il `lv_conf.h` del progetto
(`PegLib/lvgl_port/lv_conf.h`) è lungo 13 righe.

### 2.3 Conseguenza

`lv_timer_handler()` è il super-loop interno di LVGL: scorre la lista dei timer
registrati ed esegue quelli scaduti. Non essendo registrato **nessun** display,
input o widget, quella lista è vuota: la funzione entra, non trova lavoro, esce.

**LVGL è linkata e inizializzata, ma è fuori dal percorso dei pixel, dell'input e
del tempo.**

### 2.4 Perché allora si chiama così

Perché la macro di build si chiama `PEG_USE_LVGL`. Il nome **non** significa
"disegniamo con LVGL": significa "non usare Qt, usa il backend embedded Linux".
È il nome del **ramo di compilazione**, non del motore grafico. Anche il nome
della classe `PegLvglWindow` è storico: vuol dire "finestra del path non-Qt".

| Macro | Classe host | Significato reale |
|---|---|---|
| `PEG_USE_QT` | `PegMainWindow` | path Qt classico |
| `PEG_USE_LVGL` | `PegLvglWindow` | path embedded non-Qt (SDL, poi DRM) |

Il registro documenta già questo punto nella sezione *Ruolo reale di LVGL*, dove
il nome è definito testualmente «fuorviante».

**Quindi: quello che ha fatto Tommaso è stato sostituire Qt con SDL.** LVGL non
c'entra, se non come nome della macro.

---

## 3. I tre path, meccanicamente

### 3.1 Qt classico — `PEG_USE_QT`, classe `PegMainWindow`

Il framebuffer PEG **è** una `QImage`: `createSurface()` crea la QImage e
restituisce `m_pImage->bits()` (`pegmainwindow.cpp:103-105`); quel puntatore
diventa `g_pyBitmap` e viene passato a `PEG_SetFrameBuffer` (`peg_run.cpp:1195`,
`:1205`). **A monte non c'è alcuna copia:** PEG disegna direttamente dentro i
pixel della QImage.

Il costo sta a valle, nella catena di present:

```text
PEG_DoubleBufferingRefresh()        peg_run.cpp:916-932
  → request_update(QRect)
  → emit dispatch_update            pegmainwindow.cpp:466-468
  → [QueuedConnection]              pegmainwindow.cpp:37
  → slot execute_update
  → QWidget::update(rect)           pegmainwindow.cpp:471-473
  → paintEvent
  → QPainter::drawImage()           pegmainwindow.cpp:108-145
  → backing store Qt
  → plugin di piattaforma / compositor / EGLFS
  → display
```

Due punti chiave:

1. **L'aggiornamento passa dall'event loop di Qt** (connessione accodata,
   `pegmainwindow.cpp:37`). L'applicazione non decide *quando* il frame arriva
   a schermo: lo decide lo scheduler degli eventi Qt.
2. **Il backend video appartiene a Qt.** Né il buffer di scanout né il pageflip
   sono sotto controllo applicativo.

**Input:** override Qt classici `mouseMoveEvent` / `mousePressEvent` /
`mouseReleaseEvent` / `wheelEvent` / `keyPressEvent` (`pegmainwindow.h:65-72`),
poi `mouse_event_manager()` → callback `PFUN_PEGMOUSEMAPPING`. Non esiste alcun
`QTouchEvent`: il touch arriva **solo come evento mouse sintetizzato da Qt**.

### 3.2 SDL — `PEG_USE_LVGL` senza `EMBEDDED_HMI_RT_DRM_DIRECT` (lavoro di Tommaso)

Inizializzazione: `SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER)`
(`peglvglwindow.cpp:478-479`) → `SDL_CreateWindow` fullscreen desktop (`:495`)
→ `SDL_CreateRenderer` (`:513`, via `createEmbeddedRenderer` `:360-371`) con
backend **kmsdrm + opengles2 + vsync** (hint a `:336-355`) →
`SDL_CreateTexture(RGB565, STREAMING)` (`:540`).

```text
PEG
  → g_pyBitmap (RAM)
  → uploadDirtyRegion()
  → SDL_UpdateTexture(rettangolo dirty)     peglvglwindow.cpp:1184
  → SDL_RenderClear / SDL_RenderCopy        :781-783, :1237-1239
  → SDL_RenderPresent
  → DRM/KMS
  → display
```

Nota: `SDL_LockTexture` non è mai usato; il throttle del present è su
`SDL_GetTicks` (`:775`).

**Input:** `SDL_PollEvent` (`:823`) con `SDL_MOUSEMOTION/BUTTONDOWN/UP`
(`:836-871`), `SDL_FINGERDOWN/UP/MOTION` (`:881-922`, coordinate normalizzate
scalate con `SDL_GetWindowSize` `:889`), `SDL_MOUSEWHEEL` (`:926`),
`SDL_KEYDOWN/UP` (`:933-938`). Per evitare il doppio evento c'è
`SDL_HINT_TOUCH_MOUSE_EVENTS=0` (`:435`) più il filtro su `SDL_TOUCH_MOUSEID`.

**Cosa si guadagna rispetto a Qt:**
- Qt esce completamente dal link (`CONFIG -= qt`, `PegLib.pro:33`)
- sparisce l'event loop Qt dalla catena di present
- **l'applicazione decide quando presentare**

**Cosa resta in mezzo:**
- una **copia CPU → texture** (`SDL_UpdateTexture`)
- un **giro sulla GPU**: `SDL_RenderCopy` è a tutti gli effetti una draw call
  OpenGL ES, benché il rasterizzatore dei widget sia software

Il registro identifica proprio `SDL_UpdateTexture()` come collo di bottiglia di
questo stadio: aumenta il traffico DDR e quindi l'interferenza sul thread RT.

### 3.3 DRM diretto — `PEG_USE_LVGL` + `EMBEDDED_HMI_RT_DRM_DIRECT` (attuale)

Inizializzazione: `SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER)`
(`peglvglwindow.cpp:442`) — **senza `SDL_INIT_VIDEO`**. Il sottosistema video di
SDL non viene proprio avviato.

Al suo posto, `pegenstein/PegLib/pegdrmoutput.cpp`:

| Passo | Riferimento |
|---|---|
| `open("/dev/dri/card0")` | `pegdrmoutput.cpp:45` |
| scan connector / encoder / CRTC | `:222-282` |
| **due** dumb buffer, `DRM_IOCTL_MODE_CREATE_DUMB` | `:316` |
| `MAP_DUMB` + `mmap` | `:341-347` |
| `drmModeAddFB2(DRM_FORMAT_RGB565)` | `:363` |
| `drmModeSetCrtc` | `:396` |
| `drmModePageFlip(DRM_MODE_PAGE_FLIP_EVENT)` + `select()` + `drmHandleEvent` | `:691-712` |

```text
PEG
  → g_pyBitmap (RAM)
  → blitDirtyRegion()  /  syncBackFromPeg()
  → memcpy RGB565 → dumb buffer DRM
  → drmModePageFlip()
  → scanout diretto KMS
  → display
```

**RGB565 dall'inizio alla fine, zero conversioni:** `createReq.bpp = 16`
(`pegdrmoutput.cpp:314`), `DRM_FORMAT_RGB565` nel `drmModeAddFB2` (`:364`), blit
con `left * 2` perché 2 byte/pixel (`:512`), e `initialize()` rifiuta
esplicitamente qualunque bpp ≠ 16 (`:448-450`).

**Input:** non più SDL ma **evdev diretto**, `pegenstein/PegLib/pegdrm_evdev.cpp`
— scansione di `/dev/input/event*` (`:225`) con probe `EVIOCGBIT` / `EVIOCGABS` /
`EVIOCGPROP` (`:44-78`), override via `PEGDRM_TOUCH_DEV` (`:206`), lettura
`input_event` (`:288`) con supporto multi-touch protocol B (`ABS_MT_SLOT`,
`ABS_MT_TRACKING_ID`) e single-touch `BTN_TOUCH`, con coalescing di un solo
motion per `poll()`. Aggancio in testa a `processEvents()`
(`peglvglwindow.cpp:816-819`) → `drmEvdevTouchThunk` → `handleDrmEvdevTouch` →
`emitMouseEvent` (`:1382-1409`).

---

## 4. Tabella comparativa

| Aspetto | Qt classico | SDL (intermedio) | DRM diretto (attuale) |
|---|---|---|---|
| Macro build | `PEG_USE_QT` | `PEG_USE_LVGL` | `PEG_USE_LVGL` + `EMBEDDED_HMI_RT_DRM_DIRECT` |
| Classe host | `PegMainWindow` | `PegLvglWindow` | `PegLvglWindow` + `PegDrmOutput` |
| Chi rasterizza i widget | PEG (software) | PEG (software) | PEG (software) |
| Buffer PEG | `QImage` in RAM | `g_pyBitmap` in RAM | `g_pyBitmap` in RAM |
| Passo successivo | `QPainter::drawImage()` | `SDL_UpdateTexture()` | `memcpy` → dumb buffer |
| Present finale | Qt / window system | `SDL_RenderPresent()` | `drmModePageFlip()` |
| GPU coinvolta nel present | sì (compositor) | sì (`RenderCopy` = draw call GLES) | **no** |
| Conversioni di formato | sì | possibili | **nessuna** (RGB565 end-to-end) |
| Chi decide *quando* presentare | Qt | l'applicazione | l'applicazione |
| Input touch | mouse sintetizzato da Qt | eventi SDL (`SDL_FINGER*`) | evdev diretto (MT protocol B) |
| Qt linkato | sì | no | no |
| SDL2 linkata | no | sì | **sì** (ma non per il video — §5) |
| libdrm linkata | no | no | sì |

---

## 5. Cosa resta di SDL nella configurazione attuale

Questo è il punto su cui è più facile sbagliare, perché "siamo passati a DRM" viene
spesso riassunto come "abbiamo tolto SDL". **Non è così: SDL2 è ancora linkata e
ancora usata.** Quello che è stato rimosso è il suo **sottosistema video**.

`include($$PWD/../lvgl_sdl2.pri)` è **incondizionato** sotto `PEG_USE_LVGL`
(`PegLib.pro:232`, dentro il blocco aperto a `:224`), quindi `-lSDL2` entra nel
link anche con `EMBEDDED_HMI_RT_DRM_DIRECT` attivo.

**Compilato ed eseguito oggi:**

| Uso | Posizione |
|---|---|
| `SDL_SetHint` | `peglvglwindow.cpp:435` |
| `SDL_Init(EVENTS \| TIMER)` / `SDL_GetError` | `:442-444` |
| `SDL_CreateMutex` | `:461` |
| `SDL_LockMutex` / `SDL_UnlockMutex` | `:640/652`, `:1012/1041`, `:1060/1062` |
| `SDL_DestroyMutex` | `:578` |
| `SDL_GetTicks` | `:695` |
| `SDL_PollEvent` | `:823` |
| `SDL_ShowCursor` | `:969` |
| `SDL_Quit` | `:611` |

In pratica SDL sopravvive come libreria di **primitive di sincronizzazione e
tempo** (mutex, tick), non come backend grafico.

**Compilato ma morto a runtime:**
- il loop `SDL_PollEvent` gira, ma senza sottosistema video non riceve eventi
  mouse / finger / tastiera
- `presentDirtyRegion()` con `RenderClear/RenderCopy/RenderPresent` (`:1236-1239`)
  non è mai chiamata: su `EMBEDDED_HMI` si passa da `uploadDirtyRegion()` +
  `flushPresent()` (`:673-680`)
- `SDL_ShowWindow` (`:621`) e le `SDL_Destroy*` (`:583-593`) sono protette da
  puntatori sempre nulli

**Non compilato** (dietro `#else` / `#if`): tutto il blocco di init
video/window/renderer/texture (`:469-558`), `SDL_UpdateTexture` (`:1184`),
`RT_NATIVE_TEXTURE`, `RT_DIAG`, `RT_SAFE`.

Riscontro oggettivo: l'oggetto compilato
`build/sqvara/avn8mp-Release/.../peglvglwindow.o` importa **solo** i simboli SDL
elencati nella tabella sopra — nessun `SDL_CreateWindow`, nessun
`SDL_UpdateTexture`.

> **Punto non verificato.** Che il `SDL_PollEvent` residuo non produca alcun
> evento discende dalla semantica SDL (mouse e finger dipendono dal sottosistema
> video, non inizializzato), ma **non esiste un test nel repo che lo attesti**.
> Registrato come inferenza, non come misura.

---

## 6. I numeri e la loro tracciabilità

| Valore | Stadio | Fonte | Stato |
|---|---|---|---|
| **130 µs** | SDL, no throttle | `registro_test_rt.md:3953`, `:5041` | ✅ documentato |
| **99 µs** | DRM, no throttle | `registro_test_rt.md:3966` | ✅ documentato, A/B diretto |
| **98 µs** | DRM, test finale tirocinio | handoff §1, 1 451 000 attivazioni, zero sopra 100 | ✅ documentato |
| **450 µs** | Qt | — | ⚠️ **non presente nel registro** |

Il confronto **130 → 99 µs** è solido: il registro lo riporta come A/B diretto
(riga 3966, «SDL 130 µs vs DRM senza throttle 99 µs — Test 6 migliora anche il
worst-case senza cgroup»).

### 6.1 Lavoro aperto: la baseline Qt

Il valore **450 µs** per il path Qt **non compare in `registro_test_rt.md`**
(ricerca su `450`, `PEG_USE_QT`, `Qt`). Presumibilmente precede la campagna
documentata. Per essere difendibile va ricostruita la tracciabilità:

- quando è stato misurato
- con quale carico e quale durata (vedi regola: minimo un'ora per configurazione)
- con quale grandezza (ritardo di risveglio `clock_nanosleep`? `rtc_handler_us`?)
- su quale build / branch

Finché non è ricostruito, in sede di presentazione va dichiarato come **dato
storico non riprodotto nella campagna corrente**.

### 6.2 Attenzione: due "130 µs" diversi nel registro

Il numero 130 µs compare nel registro in **due contesti scorrelati**:

1. **Il 130 µs di questa slide** — massimo RT del path SDL, tabella di confronto
   configurazioni (`:3953`, `:5041`, `:3966`).
2. **Un 130 µs diverso** (`:4486`, `:4606`) — jitter misurato martellando i tasti
   di stato **con un programma grafico caricato**, attribuito alla macchina dei
   cambi pagina (`CambiaPagina`: teardown/rebuild degli alberi widget + ridisegno
   completo, eseguita due volte per pressione), **non** al path di display.

Chi cerca "130 µs" nel registro trova entrambi. Non vanno confusi.

---

## 7. Uso nella slide "Confronto chiave"

### 7.1 Testo proposto per i tre baloon

Versione consigliata:

| # | Bar | Testo |
|---|---|---|
| 1 | 450 | **Host Qt** — present via `QPainter`, accodato nell'event loop di Qt |
| 2 | 130 | **Host SDL** (Tommaso) — present nostro, ma passa da texture GPU |
| 3 | 100 | **DRM/KMS diretto** — memcpy nel buffer di scanout + pageflip |

Versione breve, se lo spazio è poco:

| # | Testo |
|---|---|
| 1 | Qt governa il display |
| 2 | SDL: present nostro, texture GPU |
| 3 | Pageflip diretto, niente GPU |

Versione "cosa si toglie", se si preferisce l'angolo narrativo:

| # | Testo |
|---|---|
| 1 | Qt: event loop + compositor in mezzo |
| 2 | Tolto Qt, resta una copia CPU→texture |
| 3 | Tolta anche quella: scrittura diretta sullo scanout |

### 7.2 Tre correzioni rispetto alla bozza iniziale

**1. "lvgl+sdl" → scrivere "SDL".** Se arriva la domanda «e LVGL cosa fa?», la
risposta onesta è «niente: è il nome della macro di build» (§2). Meglio anticiparla
con una nota a piè di slide che subirla.

**2. "Rimosso sdl" è impreciso e verificabile.** SDL2 è ancora linkata e ancora
usata (§5). Quello che è stato rimosso è il **video** SDL. Formulazione corretta:
*"Rimosso il video SDL"*, o meglio in positivo: *"Present diretto DRM/KMS"*.

**3. Il 450 µs va tracciato** (§6.1) o dichiarato come dato storico.

Nota minore: i valori documentati per lo stadio attuale sono 98–99 µs; il "100"
della slide è un arrotondamento coerente con la soglia obiettivo (< 100 µs), ma
conviene saperlo.

---

## 8. Riferimenti incrociati

| Documento | Sezione pertinente |
|---|---|
| `lvgl/pressa/perf_terminale/registro_test_rt.md` | *Differenze strutturali interfacce* (≈ riga 3360) e *Ruolo reale di LVGL* (≈ riga 3500) |
| `lvgl/pressa/perf_terminale/registro_test_rt.md` | tabella confronto configurazioni RT (`:3953`, `:3966`, `:5041`) |
| `lvgl/pressa/perf_terminale/modifiche_progetto.md` | §3 modifiche, §7 lavoro aperto |
| `handoff_rt_jitter_2026-09-15.md` | stato al 15/09/2026, risultato di riferimento 98 µs |
| `pipeline_peg_sdl_drm_rt.md` | § ruolo LVGL (citato dal registro) |

### File sorgente citati

| File | Ruolo |
|---|---|
| `pegenstein/PegLib/pegmainwindow.cpp/.h` | host Qt (`PegMainWindow`) |
| `pegenstein/PegLib/peglvglwindow.cpp/.h` | host embedded (`PegLvglWindow`), SDL e DRM |
| `pegenstein/PegLib/pegdrmoutput.cpp/.h` | dumb buffer, `drmModeSetCrtc`, pageflip |
| `pegenstein/PegLib/pegdrm_evdev.cpp/.h` | touch via evdev |
| `pegenstein/PegLib/PegLib.pro` | macro di build e condizioni di link |
| `pegenstein/lvgl_sdl2.pri` | link SDL2 (incondizionato sotto `PEG_USE_LVGL`) |
| `pegenstein/PegLib/peg_run.cpp` | main loop, `PEG_SetFrameBuffer`, `pumpLvgl()` |
