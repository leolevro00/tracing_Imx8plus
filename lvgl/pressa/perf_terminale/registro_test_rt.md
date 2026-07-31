# Registro test RT — GUI PEG/SDL su i.MX8M Plus

> **File vivo**: aggiornato a ogni esperimento sul target o modifica rilevante nel codice.
> 🏁 **[IPOTESI FINALE](#ipotesi-finale) (2026-07-30)** — il jitter residuo nasce nel **percorso kernel di risveglio**, il cui costo è **bimodale**: o l'iterazione è pulita (~670-700 k cicli, ~18 µs) o porta con sé ~100 000 cicli di lavoro kernel periodico in più (tick, accounting cgroup, PELT, bilanciamento pre-idle) e finisce fra 25 e 60 µs. **La GUI non rallenta quel percorso: aumenta la probabilità che la collisione avvenga.** Nove ipotesi alternative chiuse ciascuna con una misura (banda DDR, cache miss, contesa di latenza, processi concorrenti, idle profondo, ciclo idle/wakeup via PM QoS, DVFS, termico, load balancing). Piste aperte: `nohz_full=3` (guadagno limitato, richiede un solo task runnable) e il **secondo thread RT sul core isolato**.
> 🎯 **Meccanismo del jitter — indagine PMU (2026-07-30)**. Confronto tra **due distribuzioni complete da 10 000 iterazioni** (riposo vs martellamento del simulatore di piegatura), contatori PMU per singola iterazione RT.
> **La GUI non cambia la natura di un'iterazione lenta, cambia quanto spesso capita**: iterazioni > 25 µs **+62 %**, > 55 µs **+132 %**, ma **a parità di ritardo i contatori sono identici** (CPI, refill L2, cicli) in entrambe le condizioni.
> E il ritardo **non è tempo di esecuzione**: nella fascia ≥ 55 µs i cicli CPU sono gli stessi della fascia < 25 µs (~670–780 k) a fronte di un ritardo triplo ⇒ **~40 µs in cui il core non esegue nulla**. Su ARM il contatore si ferma col clock gated ⇒ sospetto principale: **latenza di uscita da `cpuidle`**, non contesa di memoria. Test decisivo (5 min, non ancora fatto): disabilitare gli idle state profondi su CPU3.
> ⚠️ Un'ipotesi precedente ("contesa di latenza sulla memoria", basata su 2 soli worst case) è stata **ritirata**: utile come nota metodologica sul rischio dei campioni piccoli. → [→ indagine](#meccanismo-jitter-risolto-2026-07-30)
> 🏁 **RISULTATO FINALE (2026-07-30, 15 min / 225 000 attivazioni)** — configurazione: branch merged + path **DRM** + fix riallocazione PegGL + coalescing pan **33 ms** + throttling cgroup 15%, uso intensivo e vario:
> **max 103 µs · 1 solo sforamento > 100 µs (4,4 /M)**. Il **grafico 2D non è più un hotspot** (era 47,1 /M → ora 4,4 /M, allineato al 4,7 /M dell'uso generale); fasce 71-80 / 81-90 / 91-99 µs **−70% / −85% / −92%**. L'unico sforamento residuo si è verificato martellando **Piece Set ↔ Manual**, cioè nella macchina dei **cambi pagina** (filone B) — l'unico punto deliberatamente non affrontato → [→ esito finale](#test-finale-merged-scroll-calculation-2026-07-30).
> 🥇 Miglior singola sessione: martellando il **3D viewer** con la stessa configurazione, **0 sforamenti** su 77 000 attivazioni, max 100 µs (durata ~5 min, da confermare con una sessione lunga).
> 🔴 **Hotspot residuo individuato (non risolto)**: pagina **"Manual Sequence"** — max **135 µs**, il più alto dai test SDL. Causa nel codice: `DrawDisView` **ricalcola la geometria della piega dentro la routine di disegno** (`Sim2DView.cpp:326-347`). **Misurato** (tempo di CPU): un draw costa **1 142 µs medi / 2 248 µs max** su un ciclo RT di 4 ms, di cui **`Posiziona` ~50%** (832 µs) — e non disegna nulla. Documentato come **lavoro futuro n.1**; non toccato perché governa la sequenza reale di piegatura → [→ Manual Sequence](#test-finale-merged-scroll-calculation-2026-07-30).
> 🔬 **Conclusione trasversale (formulazione prudente)**: l'efficacia del throttling cgroup **dipende dallo scenario** — nello *scroll grafico* migliorava il massimo (99→71 µs, campagna 2026-07-20), su *Manual Sequence / 3D* no (max invariato 113-135 µs), perché lì il costo sta in **singole operazioni** da 1-2 ms di CPU che non si possono accorciare. La **banda DDR è stata esclusa come causa** con un controllo sperimentale: i picchi a 10 ms sono quasi identici a riposo (7,8 %) e sotto carico (9,1 %), quindi non discriminano tra le due condizioni; sotto carico cambia solo la *frequenza* delle raffiche. Candidato ora più plausibile: **inquinamento delle cache** da parte di un draw che tocca >1 MB di pixel in 1,1 ms, più lock/page fault. Direzione con maggiore probabilità di successo: **ridurre il lavoro per disegno**, non regolare lo scheduler.
> Ultimo aggiornamento: **2026-07-30** (🏆 **CONFRONTO PRINCIPALE — SDL vs DRM diretto, a parità di codice** (stesso branch, cambiato solo il define `EMBEDDED_HMI_RT_DRM_DIRECT`): sforamenti >100 µs **215/M (SDL) → 4,7/M (DRM)**, cioè **−98%, fattore ~46×**; **caso peggiore 158 → 113 µs (−45 µs)**. È l'**unico** intervento del lavoro che ha abbassato il *picco massimo* e non solo la frequenza. Uno sforamento ogni ~18 s con SDL, uno ogni ~14 min con DRM. 🥇 **Confermato sotto il carico più pesante** (martellamento **3D viewer**, 63 k attivazioni per parte, confronto diretto senza normalizzazione): sforamenti >100 µs **33 → 4** (−88%), max **180 → 106 µs** (−41%). Il **3D viewer è il carico peggiore rimasto**: 63,5 sforamenti/M contro 4,7/M dell'uso normale, anche su DRM. 🔬 **Meccanismo ora verificato nel codice — quattro fattori**: SDL fa **2 copie** di pixel invece di 1, ridisegna **tutto lo schermo** ad ogni present (`SDL_RenderCopy(..., nullptr, nullptr)`), **si blocca sul vsync** (`SDL_RENDERER_PRESENTVSYNC` + `SDL_RenderSetVSync(1)`), e subisce una **conversione RGB565→ARGB8888 nascosta** ad ogni upload che raddoppia i byte scritti (RGB565 non è nativo su GLES2/i.MX8MP, vedi TEST 5); il DRM è invece **RGB565 nativo end-to-end** (`DRM_FORMAT_RGB565`), copia solo la regione sporca e il page flip è asincrono. È la varianza e la banda di memoria, non il carico medio, a generare il jitter — il **TEST 5b** lo dimostra: GUI +150% ma RT 191 µs, cioè prestazioni grafiche e determinismo RT sono **assi indipendenti**. Vedi [→ sezione U](#test-finale-merged-scroll-calculation-2026-07-30). 🔍 **3D viewer**: causa individuata e **ipotesi "contesa GPU" RETTIFICATA** — `libPegGL.so` è un'implementazione **software** di OpenGL ES (rasterizzatore + JIT ARM, nessuna libreria GPU linkata), quindi la GPU **non è coinvolta né nell'interfaccia né nel 3D**: nella configurazione DRM è praticamente inutilizzata. Il 3D viewer è il carico peggiore perché fa rasterizzazione 3D **software sulla CPU** e, ad ogni frame, **rialloca** il bitmap nativo (`PegGL/egl.cpp:891-894`) — allocazione dinamica di un buffer grande nel percorso di disegno, ostile al RT. ✅ **FIX APPLICATO**: rimossa quella riallocazione (`PegGL/egl.cpp`) — la logica di riuso del buffer **esisteva già** in `renderToNative`, ma il chiamante la disattivava azzerando `pStart`. Il buffer da ~960 KB superava la soglia mmap di glibc, quindi ogni frame comportava `munmap` (→ TLB shootdown con IPI verso il core RT) più ~240 page fault. ⚖️ **Validazione INCONCLUSIVA**: post-fix 70 k att., max **102 µs** (era 106) e sforamenti >100 µs **42,9/M** (erano 63,5/M) — ma il totale eventi >60 µs è **2,2× più alto**, segno che in quella sessione il 3D ha disegnato molti più frame (martellamento manuale = carico non riproducibile), e 3 eventi contro 4 sono statisticamente indistinguibili. Servirebbero carico automatizzato e ≥40 min per sessione. Il fix resta giustificato a prescindere: rimuove un'operazione non deterministica dal percorso di disegno e ripristina il comportamento previsto da `renderToNative`. → [→ 3D viewer](#3d-viewer-gpu-2026-07-30). 📊 **test sul branch merged, due sessioni**: senza throttling 1 589 000 att. ≈ 1 h 46 min (max 113 µs, 8 spike >100 µs) e **con throttling ~15%** 848 000 att. ≈ 57 min (max 113 µs, 4 spike >100 µs). **Confronto valido con la sezione S** a parità di throttling: fasce 60–70/71–80/81–90 µs **−75%/−55%/−51%**, sforamenti >100 µs **−38%**, ma **caso peggiore invariato** (113 vs 109 µs) e fascia 91–99 µs peggiorata — le ottimizzazioni riducono la *frequenza*, non il *picco massimo*. Vedi [→ sezione U](#test-finale-merged-scroll-calculation-2026-07-30). ❌ **Ottimizzazione collisioni scartata con misura**: `check_collisioni_pezzo` costa in media **4 µs**/frame → cacharlo è inutile. 📐 **Nota metodologica**: gli sforamenti >100 µs avvengono in media **uno ogni ~13 min**, quindi sotto la mezz'ora un test che non li rileva **non dimostra nulla**. Spike correlati al martellamento dello scroll della pagina **"Calculation"** (`PAG_OTTIM_SIM2D`, `CSim2DView`), che **ha già** il throttling `DrawPanIfDue` — il margine residuo è il ricalcolo collisioni per frame. Vedi [→ sezione U](#test-finale-merged-scroll-calculation-2026-07-30). 🔀 **merge finale**: branch `experiment/test-6-ch0-defer-plus-pan-scroll` = defer CH0 (IMP/MAN) + ottimizzazione pan/scroll, senza conflitti; l'estensione CORR/AUTO/SAUTO resta fuori perché il guadagno non è dimostrato. Il merge ha toccato solo 8 file (`cad2d/`+`sim2d/`): le modifiche `liste/` erano già nel baseline, quindi rischio regressione Die/Program List basso. Vedi [→ sezione T](#merge-ch0-defer-pan-scroll-2026-07-30). ✅ **defer CH0 validato funzionalmente**: con programma numerico, restando sulla pagina numerica, il defer si innesca dalla 2ª pressione e il batching coalesce davvero il lavoro pesante — prima volta osservato empiricamente; debug rimosso da tutti i file, resta da fare la misura RT pulita. 🔑 scoperta decisiva: i test venivano fatti dalla **pagina CAD 2D del pezzo** (`PAG_CAD2D_PEZZO`=27), non dalla pagina numerica → premere Piece Set è un no-op scartato da `CambiaPagina`, premere Manual costa **due cambi pagina completi** 27→0→27; il jitter osservato viene dalla macchina dei cambi pagina, non da `SettaControlli`/`GetEntry`. Mappatura icone toolbar confermata: documento=F1/IMP, mano=F2/MAN, chiave=F3/SAUTO, fabbrica=F4/AUTO. Vedi [→ sezione R](#ch0-defer-estensione-corr-auto-sauto-2026-07-29). Nota precedente: toggle Zoom↔Normale — ripremere lo stesso tasto già attivo alterna deliberatamente tra due istanze pagina (`CPpgView`/`CPpgViewZoom`), quindi lo stato di defer per-istanza non può sopravvivere; guard `m_bCH0Completing` resta comunque in codice come fix valido; da rivalidare alternando stati diversi nel test; debug rimosso da entrambi i repo; vedi [→ sezione R](#ch0-defer-estensione-corr-auto-sauto-2026-07-29); + test cgroup ~15% uso comune/scroll Die Set, vedi [→ sezione S](#test-cgroup15-uso-comune-scroll-dieset-2026-07-29))
> Aggiornamento precedente: **2026-07-28** (branch `experiment/test-6-deferred-ch0-feedback`, pressbrakepeg: defer 500 ms Editor/Manual → max **88 µs**, 0 spike su 137k att.; vedi [→ sezione P](#editor-manual-defer-2026-07-28))
> Aggiornamento precedente: **2026-07-27** (campagna 4× su `test-6-font-pan-scroll-opt`; confronto UI `test-6-with-new-font` + cgroup `2000 20000` → max **83 µs**, 0 spill)
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
| [Cgroups CPU](#cgroups-cpu-non-disponibile) | **In questo file** — da “non disponibile” (2026-07-21) a kernel con `cpu` + campagne quota; [istruzioni script](#peg-cgroup-throttle-uso) |
| [File modificati vs Test 0](#file-modificati-vs-test-0) | **In questo file** — inventario completo dei sorgenti toccati da Test 0 → Test 6 (pegenstein, pressbrakepeg, kvuib, doc) |
| [Ottimizzazioni pan/scroll grafici](#ottimizzazioni-pan-scroll) | **In questo file** — alleggerire drag Die Set / Sim2D / Ottimizza (pressbrakepeg + ruolo PegLib) |
| [Campagna 4×30 min (2026-07-27)](#campagna-4x30min-2026-07-27) | **In questo file** — validazione RT su branch **`experiment/test-6-font-pan-scroll-opt`** |
| [Confronto branch font / UI](#confronto-branch-test6-font-2026-07-27) | **In questo file** — `pan-scroll-opt` vs **`experiment/test-6-with-new-font`** (stesso cgroup 10%) |
| [Editor/Manual: feedback + defer 500 ms](#editor-manual-defer-2026-07-28) | **In questo file** — branch **`experiment/test-6-deferred-ch0-feedback`** (pressbrakepeg): jitter da martellamento Editor↔Manual |
| [Estensione defer CH0 a Correzioni/Auto/Semiauto](#ch0-defer-estensione-corr-auto-sauto-2026-07-29) | **In questo file** — analisi rischio Start/Stop/Plus/Minus, piano defer completo (CORR) vs parziale (AUTO/SAUTO), scope esclusi (liste, Posiziona); **bug trovato**: `PM_HIDE` azzera il defer ad ogni pressione |
| [Test cgroup ~15%, uso comune + scroll Die Set](#test-cgroup15-uso-comune-scroll-dieset-2026-07-29) | **In questo file** — 529k att., max 109µs; nota: pan/scroll non ottimizzato su questo branch |
| 🏁 [**IPOTESI FINALE**](#ipotesi-finale) | **In questo file** — **sezione conclusiva**: catena di eliminazione (9 ipotesi chiuse ciascuna con una misura), cosa resta accertato, l'ipotesi che sopravvive a tutti i dati, piste aperte e cosa NON riprovare |
| ⚠️ [**Meccanismo del jitter — indagine PMU (non risolto)**](#meccanismo-jitter-risolto-2026-07-30) | **In questo file** — `PerfMonitor` riattivato (`RTCHndlr.cpp` + `Lnk/main.cpp`, warm-up `PERF_WARMUP_ITER`); ipotesi "contesa di latenza" formulata su 2 worst case e **ritirata** dopo analisi su 10 000 iterazioni; cosa resta accertato e cosa no; include la nota metodologica sul rischio dei campioni piccoli |
| ⭐ [**SDL vs DRM diretto: confronto architetturale**](#sdl-vs-drm-architettura) | **In questo file** — **sezione di riferimento**: le due pipeline passo per passo con riferimenti al codice, conteggio buffer/copie, i **quattro fattori** che spiegano i risultati (copie, schermo intero vs regione sporca, vsync bloccante vs page flip asincrono, conversione RGB565→ARGB8888 nascosta), terminologia (display controller vs GPU vs dumb buffer), principio **determinismo ≠ throughput**, sequenza Test 5 → 5b → 6, risultati misurati e cosa resta non verificato |
| [Merge finale: defer CH0 + pan/scroll](#merge-ch0-defer-pan-scroll-2026-07-30) | **In questo file** — branch `experiment/test-6-ch0-defer-plus-pan-scroll`, contenuto e rischi del merge |
| [Test finale + scroll "Calculation" + SDL vs DRM (misure)](#test-finale-merged-scroll-calculation-2026-07-30) | **In questo file** — misure sul branch merged, confronto **SDL vs DRM** (uso generale e martellamento 3D), nota metodologica sulla durata minima dei test, ottimizzazione collisioni scartata con misura |
| [3D viewer: GPU nel percorso di presentazione](#3d-viewer-gpu-2026-07-30) | **In questo file** — contesto EGL e `peglSwapBuffers` propri; carico peggiore rimasto (63,5 sforamenti/M vs 4,7/M); **lavoro futuro**, non toccato |

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

**Altra trappola “ieri ok / oggi no”:** `CAD_DIAG_CALCULATE` attivo (`fprintf`+`fflush` su stderr) → spike RT > 100 µs anche in pochi minuti; con diag off i ritardi tornano normali. Vedi [DIAGNOSTICA TEST 6](#diagnostica-test-6) e nota interferenza RT 2026-07-22.

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
| 7 | Pan/scroll grafici (Die Set, Sim2D, Ottimizza) | ✅ **Punti 1–3 attivi**; punto 4 ritirato (fluidità) | [→ sezione](#ottimizzazioni-pan-scroll) |

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

<a id="pegbitmap-rle-spiegazione"></a>

#### Dove sta la compressione: `fooData` e `BMF_RLE`

La compressione **non** è un file `.png`/`.jpg` a parte: è nel PegBitmap. Il campo dati (`fooData` / array `UCHAR`) contiene i pixel **solo se non c’è RLE**; con `BMF_RLE` contiene un **flusso compresso** che PEG decodifica a runtime.

```cpp
#define BMF_RLE  0x01   // bitmap is RLE encoded  (pegtypes.hpp)
```

```cpp
PegBitmap foo = {
    flags,   // 0 = raw, oppure BMF_RLE = compresso
    8,       // 8 bpp = indicizzata (palette)
    w, h,
    fooData  // raw indici-pixel  OPPURE  stream RLE
};
```

**Due leve distinte:**

| Leva | Dove | Significato |
|------|------|-------------|
| **Indicizzata** | `bpp = 8` | ogni pixel (decompresso) è un indice palette, 1 byte |
| **Compressa** | `uFlags |= BMF_RLE` | `fooData` **non** ha necessariamente 1 byte per pixel |

##### Caso 1 — 8 bpp **non** compressa (`flags = 0`)

```cpp
PegBitmap foo = {
    0,          // niente RLE
    8,          // 8 bpp
    100,
    50,
    fooData
};

UCHAR fooData[] = {
    3, 3, 3, 5, 5, 2, ...
};
```

Qui `fooData` contiene **direttamente** gli indici dei pixel:

- `fooData[0] = 3` → primo pixel usa `palette[3]`
- `fooData[1] = 3` → secondo pixel usa `palette[3]`
- …

Con un’immagine 100×50: **100 × 50 = 5000** pixel → senza compressione ≈ **5000 byte** (1 byte = 1 indice-pixel).

##### Caso 2 — 8 bpp **compressa RLE** (`flags = BMF_RLE`)

```cpp
PegBitmap foo = {
    BMF_RLE,
    8,
    100,
    50,
    fooData
};
```

`fooData` **non** contiene tutti i 5000 pixel in sequenza: è un flusso che PEG deve **decodificare**.

Esempio concettuale. Pixel originali:

```text
5 5 5 5 5 2 2 2
```

Senza compressione:

```cpp
UCHAR rawData[] = { 5, 5, 5, 5, 5, 2, 2, 2 };
```

Con RLE (schema semplificato “ripeti N volte il colore C”):

```text
5, 5, 3, 2
│  │  │  │
│  │  │  └─ indice colore 2
│  │  └──── ripetilo 3 volte
│  └─────── indice colore 5
└────────── ripetilo 5 volte
```

Quindi: **un elemento di `fooData` non corrisponde più a un pixel**; corrisponde a un pezzo dello stream RLE. Il formato esatto dei run è quello implementato da PEG in decode bitmap; l’idea è la stessa (run-length).

> **Nota:** in PEG non c’è un “livello di compressione” tipo JPEG 1–9: la leva è essenzialmente **RLE on/off** (`BMF_RLE`), oltre a scegliere 8 bpp vs 16 bpp.

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
| **Fatto / testato** | Misura footprint font sull’immagine; experiment **#2 pulizia build** (`experiment/test-6-with-new-font` in kvuib): rimossi `Yahei_N.cpp` morti dalle `libPegFontChs*` (runtime CHS usa già `PegFontTypeYaHeiN.gz`). **Flash Chs: 17,82 → 11,54 MB (−6,28 MB / −6 584 848 B)**. RAM path Yahei **invariata** (glifi usati restano dal `.gz`). |
| **Si può fare ancora?** | **Subset Unicode** (ricattura solo glifi usati nelle stringhe UI) → sì, massimo guadagno a lungo termine; serve PEG Font Capture + charset. **SKU EU slim** (non installare lib CJK) → sì su immagini lab EU-only. |
| **Dettaglio** | [Experiment font #2](#experiment-font-2) |

---

<a id="ottimizzazioni-pan-scroll"></a>

## Ottimizzazioni pan/scroll grafici (pressbrakepeg)

> **Obiettivo:** ridurre ritardo percepito e carico CPU/DDR durante il **drag su e giù** sui grafici che disturbano di più (Die Set / CAD e pagina Ottimizza / Sim2D), senza cambiare la pipeline DRM Test 6.
>
> **Branch codice:** `pressbrakepeg` → `experiment/test-6-font-pan-scroll-opt` (parte da `experiment/test-6-with-new-font`).
>
> **Nota (2026-07-27):** la [campagna 4× RT](#campagna-4x30min-2026-07-27) è su **`experiment/test-6-font-pan-scroll-opt`**. Confronto con la UI del branch padre **`experiment/test-6-with-new-font`**: [sezione dedicata](#confronto-branch-test6-font-2026-07-27).
>
> **Data avvio:** 2026-07-24.  
> **Stato attuale:** punti **1–3 attivi**; punto **4 ritirato** (fluidità peggiore).

### Contesto

Il pan dei grafici era pesante perché a ogni movimento touch l’app **ridisegnava troppo e troppo spesso**. PegLib poi caricava dirty region enormi → più CPU/DDR → più ritardo sul task RT (`rtc_handler_us` / `nanosleep`).

| Layer | Ruolo nel pan |
|--------|----------------|
| **pressbrakepeg** (`cad2d` / `sim2d`) | Decide *quando* e *cosa* ridisegnare |
| **PegLib** | `Invalidate` / `Draw` / upload DRM |

**Nota PEG:** `Invalidate()` da sola **marca** la regione; serve un `Draw()` (poi `EndDraw` → upload) per aggiornare lo schermo.

### Schermi critici (prima delle opt)

| Schermo | Path | Comportamento pan (prima) |
|---------|------|---------------------------|
| **Die Set** (CAD) | `CPPGBaseView::OnMove` | `UpdateAllViews()` → view + form/tabelle |
| **Ottimizza / Sim2D** | `CSim2DView::OnMouseMove` | ogni motion → `Invalidate()` + **`Draw()` immediato** |

---

### Sintesi — cosa abbiamo fatto (punti 1–3)

#### Punto 1 — Sim2D / Ottimizza (`Sim2DView`)

| | |
|--|--|
| **Prima** | Ogni motion → `Invalidate()` + `Draw()` subito (anche molti ridisegni tra un vsync e l’altro) |
| **Dopo** | L’origine (`m_ptTo`) si aggiorna sempre; il `Draw` al massimo ogni **~16 ms** (~60 Hz). Al rilascio dito, un ultimo `Draw` se serve (`DrawPanIfDue`) |
| **File** | `sim2d/Sim2DView.cpp`, `Sim2DView.h` |
| **Migliora** | Meno burst di ridisegno → meno code di lavoro e meno interferenza RT |
| **Non cambia** | Ogni frame resta un ridisegno **completo** del grafico |

#### Punto 2 — Die Set / CAD (`Ppgviews`)

| | |
|--|--|
| **Prima** | Pan → `UpdateAllViews()` → canvas **+** form/tabelle/`AdattaMondo`/preview |
| **Dopo** | In drag solo il **canvas** (`Invalidate`+`Draw` throttled come Sim2D). Form ecc. non si aggiornano mentre trascini |
| **File** | `cad2d/Ppgviews.cpp`, `Ppgviews.h` |
| **Migliora** | Soprattutto sul Die Set: meno lavoro inutile fuori dal disegno |

#### Punto 3 — Griglia light (CAD)

| | |
|--|--|
| **Prima** | A ogni ridisegno, migliaia di `PutPixel` per i puntini della griglia |
| **Dopo** | In pan (`m_bTracking`) la griglia **non** si disegna; torna al rilascio |
| **File** | `MatView.cpp` (Die Set), `Pezzoview.cpp`, `Punzview.cpp` |
| **Migliora** | Meno CPU per frame sul CAD durante lo scroll |

**In una frase:** abbiamo smesso di ridisegnare a raffica e di aggiornare UI/griglia inutili in pan; il grafico si muove ancora ridisegnando tutto, ma **meno spesso e con meno lavoro intorno**.

---

### Cosa è migliorato (misure 2026-07-24)

| | Prima (indicativo) | Dopo punti 1–3 |
|--|-------------------|----------------|
| RT worst in drag | ~100 µs (solo p.1) / storici full-res spesso 120+ | **~92 µs** / 10 min |
| `reqMBps` (upload) | ~46–47 (solo p.1) | **~23–26** (≈ **−45%**) |
| `maxRectPx` | ~485k | ancora ~485k |

| Esito | |
|-------|--|
| ✅ Sì | Ritardo RT sotto soglia 100 µs; banda media upload circa dimezzata vs solo throttle |
| ❌ No | Il rettangolo dirty per frame resta grande (`maxRectPx` ≈ viewport) — non è uno “scroll shift” |

**Sample perf — interazione peggiore** (`[WORST rtc_handler_us]`, CPU3, dopo punti 1–3):

```text
[WORST rtc_handler_us]
Iterazione:           10000
rtc_handler_us:       92
l2d_cache:            19265
l2d_cache_refill:     3602
L2 cache miss:        18.6971 %
bus_access:           14416
bus_cycles:           438202
bus_access/bus_cycles: 0.032898
bus_cycles/bus_access: 30.3969
cpu_cycles:           871939
istruzioni:           234517
IPC:                  0.268960
CPI:                  3.718020
```

| Lettura breve | |
|---------------|--|
| **92 µs** | Worst-case sotto soglia 100 µs |
| **L2 miss ~18.7%** | Ancora pressione memoria (coerente con drag GUI) |
| **IPC ~0.27 / CPI ~3.72** | Stall da memoria/bus |

Esempi log `[RT] uploadDirtyRegion` dopo punti 1–3:

```text
[RT] uploadDirtyRegion: calls=66 req=25.31MB reqMBps=25.05 updateMs=64.689 effMBps=391.3 maxRectPx=485051
[RT] uploadDirtyRegion: calls=47 req=15.69MB reqMBps=15.54 updateMs=40.373 effMBps=388.6 maxRectPx=469224
[RT] uploadDirtyRegion: calls=64 req=23.54MB reqMBps=23.41 updateMs=59.335 effMBps=396.8 maxRectPx=485051
```

**Deploy:** `libsim2d.so` + `libcad2d.so` → `/opt/Squeeze/`.

---

### Tabella piano (stato)

| # | Intervento | Dove | Stato |
|---|------------|------|-------|
| **1** | Cap frequenza `Draw` in pan (~60 Hz) | `sim2d/Sim2DView.*` | ✅ attivo |
| **2** | In drag CAD: solo canvas, non `UpdateAllViews` | `cad2d/Ppgviews.*` | ✅ attivo |
| **3** | Griglia light in pan | `MatView` / `Pezzoview` / `Punzview` | ✅ attivo |
| **4** | Scroll shift (`RectMove` + strisce) | Sim2D + CAD | ❌ **ritirato** |

### Punto 4 — provato e ritirato (2026-07-24)

**Implementazione:** `RectMove` + ridisegno strisce + `Invalidate` client.

| Metrica | Dopo p.1–3 | Con p.4 | Nota |
|---------|------------|---------|------|
| RT worst | ~92 µs | ~**88 µs** | RT ok / leggermente meglio |
| L2 miss (worst) | ~18.7% | ~**30.6%** | Peggio |
| `bus_access` (worst) | ~14k | ~**38k** | Peggio |
| **Fluidità pan** | accettabile | **peggiore** (utente) | Motivo del ritiro |

**Perché non ha funzionato:** `RectMove` PEG = Capture+Bitmap (costo aggiunto); le strisce richiamano comunque `Paint`/`DrawDisView`; invalidate client → upload ancora grande. Totale: più lavoro e scatti percepiti.

**Stato codice:** punto 4 **rimosso**; restano solo 1–3.

### Prossimi passi (se serve ancora)

Leve più sane della ripetizione dello shift naïf: risoluzione viewport, meno lavoro in `Paint`/`DrawDisView`, rivedere `syncBackFromPeg` full in Test 6.

---

<a id="campagna-4x30min-2026-07-27"></a>

### Campagna validazione RT — **4 × ~30 min** (2026-07-27)

> **Contesto:** Test **6** DRM + pan/scroll punti **1–3** attivi (punto 4 ritirato). Quattro run endurance da **~mezz’ora** ciascuno per chiudere la validazione RT post-opt.
>
> **Branch pressbrakepeg (test 1–4):** `experiment/test-6-font-pan-scroll-opt` (UI con opt pan/scroll 1–3).
>
> **Obiettivo:** `nanosleep` / `rtc_handler_us` max **≤ 100 µs** (spike **> 100 µs** rari o assenti); DDR stabile; **0 crash**.
>
> **Intervalli 60–99 µs:** conteggio dei ritardi “elevati ma sotto soglia” sulle attivazioni totali della finestra — metriche chiave per confrontare la **coda** della distribuzione, non solo il max.

| # | Durata | Scenario / config | Attivazioni | max | **>100** | **60–70** | **71–80** | **81–90** | **91–99** | DDR % | Worst µs | L2 miss | Esito |
|---|--------|-------------------|------------:|----:|---------:|----------:|----------:|----------:|----------:|------:|---------:|--------:|-------|
| **1** | ~30 min | no cgroup / baseline | **631 000** | **100** | **0** | **659** | **29** | **20** | **4** | ~3,3–3,4 | **100** @ 9268 | **17,34%** | ✅ |
| **2** | ~12 min* | cgroup **`25000 100000`** (25% @ 100 ms) | **244 000** | **111** | **1** | **592** | **37** | **6** | **6** | ~3,3–3,6 | **111** @ 11418 | **27,90%** | ⚠️ |
| **3** | ~13 min* | cgroup **`4000 20000`** (20% @ 20 ms) | **274 000** | **101** | **1** | **392** | **33** | **17** | **4** | ~3,7–3,9 | **101** @ 91662 | **20,88%** | ⚠️ |
| **4** | ~13 min* | cgroup **`2000 20000`** (10% @ 20 ms) | **280 000** | **101** | **1** | **307** | **12** | **8** | **2** | ~2,5–2,6 | **101** @ 53732 | **16,35%** | ⚠️ |

\*Test 2–4: durata stimata dalla densità att. del test 1 (~21k att./min); non full 30 min.

**Somma ritardi in coda 60–99 µs** (escluso spill >100):

| # | Σ (60–99) | Σ / attivazioni | Note |
|---|----------:|----------------:|------|
| **1** | **712** | **0,113%** | coda più popolata in assoluto (run più lungo) |
| **2** | **641** | **0,263%** | densità coda **più alta** + spill 111 µs |
| **3** | **446** | **0,163%** | coda intermedia |
| **4** | **329** | **0,118%** | coda più magra (simile densità al baseline); DDR più bassa |

> Lettura: il **max** da solo non basta. Gli intervalli dicono *quanti* risvegli RTC sono andati in zona critica (60–99). Il test **4** (`2000 20000`) riduce la coda 60–99 e la DDR%, ma resta **1** spill a **101 µs** — non migliore del baseline sul criterio “zero spill ≤100”.

#### Test 1 / 4 — ~30 min (2026-07-27)

**Metriche RT** (Lnk / PerfMonitor, CPU3 — `COM RTC Handler` / `nanosleep`):

| Metrica | Valore |
|---------|--------|
| Attivazioni (finestra report) | **631 000** |
| `nanosleep` min | **11 µs** |
| `nanosleep` max | **100 µs** |
| Valori **> 100 µs** | **0** |
| Obiettivo | ≤ 100 µs — **raggiunto** (max esattamente a soglia, zero spill) |

**Distribuzione valori elevati** (sotto soglia 100 µs):

| Intervallo (µs) | Occorrenze |
|-----------------|----------:|
| 60–70 | 659 |
| 71–80 | 29 |
| 81–90 | 20 |
| 91–99 | 4 |

```text
valore massimo della nanosleep delle ultime 631000 attivazioni vale = 100
valore minimo della nanosleep delle ultime 631000 attivazioni vale = 11
i valori sopra ai 100 us nelle ultime 631000 attivazioni sono = 0
intervallo 60-70: 659
intervallo 71-80: 29
intervallo 81-90: 20
intervallo 91-99: 4
```

**Banda DDR** (`perf` / `imx8mp_bandwidth_usage.lpddr4`, campioni ~1 s):

| Metrica | Valore tipico |
|---------|---------------|
| Utilizzo LPDDR4 | **~3,3–3,4%** (stabile sul run) |

Esempio (finestre consecutive):

```text
imx8_ddr0/axid-write + axid-read → imx8mp_bandwidth_usage.lpddr4 ≈ 3.3–3.4 %
duration_time ≈ 1.001–1.002 s
```

**Peggior iterazione `[WORST rtc_handler_us]`** (CPU3, iter **9 268**):

| Contatore | Valore |
|-----------|--------|
| `rtc_handler_us` | **100 µs** |
| L2 miss | **17,34%** (`l2d_cache_refill` / `l2d_cache`) |
| `bus_access` | 13 107 |
| `bus_cycles` | 419 741 |
| `bus_access` / `bus_cycles` | 0,0312 |
| `bus_cycles` / `bus_access` | 32,02 |
| `cpu_cycles` | 835 192 |
| Istruzioni | 234 114 |
| IPC | **0,280** |
| CPI | **3,567** |

```text
Core: CPU3
[WORST rtc_handler_us]
Iterazione:           9268
rtc_handler_us:       100
l2d_cache:            18880
l2d_cache_refill:     3274
L2 cache miss:        17.3411 %
bus_access:           13107
bus_cycles:           419741
bus_access/bus_cycles: 0.031226
bus_cycles/bus_access: 32.0242
cpu_cycles:           835192
istruzioni:           234114
IPC:                  0.280312
CPI:                  3.567459
```

**Lettura breve test 1:**

| Asse | Esito |
|------|-------|
| **RT** | ✅ max **100 µs**, **0** spill > 100 su **631k** att. |
| **DDR** | ✅ ~**3,3–3,4%** LPDDR4, senza burst anomali nel campione |
| **Worst PMU** | L2 miss **~17%**, IPC **~0,28** — coerente con carico GUI moderato (simile al sample post p.1–3 ~18–19%) |

#### Test 2 / 4 — cgroup `25000 100000` (2026-07-27)

**Config PegExec:**

```text
cpu.max = 25000 100000    # 25 ms CPU / 100 ms  →  ~25% medi, burst fino a ~25 ms
cpuset.cpus = 0-2
```

**Metriche RT** (CPU3):

| Metrica | Valore | vs test 1 |
|---------|--------|-----------|
| Attivazioni | **244 000** (~12 min) | più corto |
| `nanosleep` min | **11 µs** | = |
| `nanosleep` max | **111 µs** | **+11 µs** |
| Valori **> 100 µs** | **1** (≈ **0,0004%**) | 0 → 1 |

**Distribuzione valori elevati** (< 100 µs):

| Intervallo (µs) | Occorrenze |
|-----------------|----------:|
| 60–70 | 592 |
| 71–80 | 37 |
| 81–90 | 6 |
| 91–99 | 6 |

```text
valore massimo della nanosleep delle ultime 244000 attivazioni vale = 111
valore minimo della nanosleep delle ultime 244000 attivazioni vale = 11
i valori sopra ai 100 us nelle ultime244000 attivazioni sono = 1
intervallo 60-70: 592
intervallo 71-80: 37
intervallo 81-90: 6
intervallo 91-99: 6
```

**Banda DDR** (`imx8mp_bandwidth_usage.lpddr4`):

| Metrica | Valore tipico |
|---------|---------------|
| Utilizzo LPDDR4 | **~3,3–3,6%** (stabile; picco campione **3,6%**) |

```text
axid-write ≈ 176–192 M   axid-read ≈ 355–376 M   → lpddr4 ≈ 3.3–3.6 %
```

**Peggior iterazione `[WORST rtc_handler_us]`** (CPU3, iter **11 418**):

| Contatore | Valore | vs test 1 (100 µs @ 9268) |
|-----------|--------|---------------------------|
| `rtc_handler_us` | **111 µs** | **+11 µs** |
| L2 miss | **27,90%** | **+10,6 pp** (17,34% → 27,90%) |
| `bus_access` | 21 125 | **+61%** |
| `bus_cycles` | 533 861 | +27% |
| `cpu_cycles` | 1 063 410 | +27% |
| Istruzioni | 230 359 | ≈ uguale |
| IPC | **0,217** | peggio (0,280) |
| CPI | **4,616** | peggio (3,567) |

```text
Core: CPU3
[WORST rtc_handler_us]
Iterazione:           11418
rtc_handler_us:       111
l2d_cache:            18922
l2d_cache_refill:     5279
L2 cache miss:        27.8987 %
bus_access:           21125
bus_cycles:           533861
bus_access/bus_cycles: 0.039570
bus_cycles/bus_access: 25.2715
cpu_cycles:           1063410
istruzioni:           230359
IPC:                  0.216623
CPI:                  4.616316
```

**Lettura breve test 2:**

| Asse | Esito |
|------|-------|
| **RT** | ⚠️ max **111 µs**, **1** spill — **peggio del test 1** (100 / 0) |
| **DDR %** | ≈ invariata (~3,3–3,6%) — lo scanout domina; il cgroup non “salva” la metrica % |
| **Worst PMU** | L2 miss **~28%**, IPC **~0,22**, `bus_access` **+61%** → stall memoria durante il picco |

> **Conclusione su `25000 100000`:** stessa % media del 25% “buono” (`5000 20000` → 91 µs in campagna luglio), ma **period 100 ms** consente burst ~25 ms → peggiora il **worst-case RT** vs baseline senza cgroup. **Non usare in produzione.** Preferire `stop`, oppure `10000 20000` / `5000 20000`.

#### Test 3 / 4 — cgroup `4000 20000` (2026-07-27)

**Config PegExec:**

```text
cpu.max = 4000 20000     # 4 ms CPU / 20 ms  →  ~20% medi, burst max ~4 ms
cpuset.cpus = 0-2
```

**Metriche RT** (CPU3):

| Metrica | Valore | vs test 1 | vs test 2 |
|---------|--------|-----------|-----------|
| Attivazioni | **274 000** (~13 min) | più corto | simile |
| `nanosleep` min | **12 µs** | ≈ | ≈ |
| `nanosleep` max | **101 µs** | **+1 µs** | **−10 µs** |
| Valori **> 100 µs** | **1** (≈ **0,0004%**) | 0 → 1 | = 1 |

**Distribuzione valori elevati** (< 100 µs):

| Intervallo (µs) | Occorrenze |
|-----------------|----------:|
| 60–70 | 392 |
| 71–80 | 33 |
| 81–90 | 17 |
| 91–99 | 4 |

```text
valore massimo della nanosleep delle ultime 274000 attivazioni vale = 101
valore minimo della nanosleep delle ultime 274000 attivazioni vale = 12
i valori sopra ai 100 us nelle ultime274000 attivazioni sono = 1
intervallo 60-70: 392
intervallo 71-80: 33
intervallo 81-90: 17
intervallo 91-99: 4
```

**Banda DDR** (`imx8mp_bandwidth_usage.lpddr4`):

| Metrica | Valore tipico |
|---------|---------------|
| Utilizzo LPDDR4 | **~3,7–3,9%** (stabile; leggermente sopra test 1–2) |

```text
axi-write ≈ 190–206 M   axi-read ≈ 395–417 M   → lpddr4 ≈ 3.7–3.9 %
```

**Peggior iterazione `[WORST rtc_handler_us]`** (CPU3, iter **91 662**):

| Contatore | Valore | vs test 1 | vs test 2 |
|-----------|--------|-----------|-----------|
| `rtc_handler_us` | **101 µs** | +1 µs | **−10 µs** |
| L2 miss | **20,88%** | +3,5 pp | **−7,0 pp** |
| `bus_access` | 7 093 | **−46%** | **−66%** |
| `bus_cycles` | 265 426 | −37% | −50% |
| `cpu_cycles` | 526 485 | −37% | −50% |
| Istruzioni | 140 938 | −40% | −39% |
| IPC | **0,268** | ≈ (0,280) | meglio (0,217) |
| CPI | **3,736** | ≈ (3,567) | meglio (4,616) |

```text
Core: CPU3
[WORST rtc_handler_us]
Iterazione:           91662
rtc_handler_us:       101
l2d_cache:            8481
l2d_cache_refill:     1771
L2 cache miss:        20.8820 %
bus_access:           7093
bus_cycles:           265426
bus_access/bus_cycles: 0.026723
bus_cycles/bus_access: 37.4208
cpu_cycles:           526485
istruzioni:           140938
IPC:                  0.267696
CPI:                  3.735579
```

**Lettura breve test 3:**

| Asse | Esito |
|------|-------|
| **RT** | ⚠️ max **101 µs**, **1** spill — **molto meglio del test 2** (111), quasi a livello del baseline (100) |
| **DDR %** | ~**3,7–3,9%** — ancora scanout-dominated |
| **Worst PMU** | L2 miss **~21%**, `bus_access` **basso** (7k) — picco “leggero” ma ancora 1 µs sopra soglia |

> **Conclusione su `4000 20000`:** period corretto (20 ms); worst **101 µs** accettabile come outlier raro, ma **non migliore** del test 1 senza cgroup (100 / 0 spill). Utile come tetto aggressivo in lab; in produzione resta preferibile **no cgroup** o **`10000 20000`**.

#### Test 4 / 4 — cgroup `2000 20000` (2026-07-27)

**Config PegExec:**

```text
cpu.max = 2000 20000     # 2 ms CPU / 20 ms  →  ~10% medi, burst max ~2 ms
cpuset.cpus = 0-2
```

**Metriche RT** (CPU3):

| Metrica | Valore | vs test 1 | vs test 3 |
|---------|--------|-----------|-----------|
| Attivazioni | **280 000** (~13 min) | più corto | ≈ |
| `nanosleep` min | **12 µs** | ≈ | = |
| `nanosleep` max | **101 µs** | **+1 µs** | = |
| Valori **> 100 µs** | **1** | 0 → 1 | = |

**Distribuzione valori elevati** (< 100 µs) — **coda più magra della campagna**:

| Intervallo (µs) | Occorrenze | vs test 1 | vs test 3 |
|-----------------|----------:|----------:|----------:|
| 60–70 | **307** | −352 | −85 |
| 71–80 | **12** | −17 | −21 |
| 81–90 | **8** | −12 | −9 |
| 91–99 | **2** | −2 | −2 |
| **Σ 60–99** | **329** | — | — |
| **Σ / att.** | **0,118%** | ≈ test 1 (0,113%) | meglio di test 3 (0,163%) |

```text
valore massimo della nanosleep delle ultime 280000 attivazioni vale = 101
valore minimo della nanosleep delle ultime 280000 attivazioni vale = 12
i valori sopra ai 100 us nelle ultime 280000 attivazioni sono = 1
intervallo 60-70: 307
intervallo 71-80: 12
intervallo 81-90: 8
intervallo 91-99: 2
```

**Banda DDR** (`imx8mp_bandwidth_usage.lpddr4`):

| Metrica | Valore tipico |
|---------|---------------|
| Utilizzo LPDDR4 | **~2,5–2,6%** — **più basso** della campagna (test 1–3 ~3,3–3,9%) |

```text
axid-write ≈ 115–123 M   axid-read ≈ 279–288 M   → lpddr4 ≈ 2.5–2.6 %
```

**Peggior iterazione `[WORST rtc_handler_us]`** (CPU3, iter **53 732**):

| Contatore | Valore | vs test 1 |
|-----------|--------|-----------|
| `rtc_handler_us` | **101 µs** | +1 µs |
| L2 miss | **16,35%** | **−1,0 pp** (migliore della campagna) |
| `bus_access` | 12 209 | −7% |
| `bus_cycles` | 406 687 | ≈ |
| `cpu_cycles` | 809 039 | ≈ |
| Istruzioni | 230 944 | ≈ |
| IPC | **0,285** | ≈ (0,280) |
| CPI | **3,503** | ≈ (3,567) |

```text
Core: CPU3
[WORST rtc_handler_us]
Iterazione:           53732
rtc_handler_us:       101
l2d_cache:            18657
l2d_cache_refill:     3050
L2 cache miss:        16.3478 %
bus_access:           12209
bus_cycles:           406687
bus_access/bus_cycles: 0.030021
bus_cycles/bus_access: 33.3104
cpu_cycles:           809039
istruzioni:           230944
IPC:                  0.285455
CPI:                  3.503183
```

**Lettura breve test 4:**

| Asse | Esito |
|------|-------|
| **RT max** | ⚠️ **101 µs**, **1** spill — come test 3, non zero-spill come test 1 |
| **Coda 60–99** | ✅ **329** occ. / **0,118%** att. — densità simile al baseline, **migliore** di test 2–3 |
| **DDR %** | ✅ **~2,5–2,6%** — minimo della campagna |
| **Worst PMU** | ✅ L2 miss **~16%** — migliore della campagna |

#### Sintesi campagna 4 run

| Criterio | Vincitore |
|----------|-----------|
| Zero spill >100 + max ≤100 | **Test 1** (baseline, no cgroup) |
| Coda 60–99 più magra (densità) | **Test 1** ≈ **Test 4** |
| DDR % più bassa | **Test 4** (`2000 20000`) |
| L2 miss worst più basso | **Test 4** (16,35%) |
| Peggiore (max + densità coda) | **Test 2** (`25000 100000`) |

> **Verdetto (su `experiment/test-6-font-pan-scroll-opt`):** per produzione RT resta **test 1 senza cgroup**. I cgroup con period 20 ms (`4000`/`2000`) migliorano coda/DDR rispetto al period 100 ms, ma **non eliminano** lo spill a 101 µs. Gli **intervalli** restano la metrica da riportare sempre insieme a max/spill.

---

<a id="confronto-branch-test6-font-2026-07-27"></a>

### Confronto branch pressbrakepeg — UI “vecchia” vs pan/scroll-opt (2026-07-27)

| Branch | Cosa è |
|--------|--------|
| **`experiment/test-6-font-pan-scroll-opt`** | UI dei **test 1–4** sopra (opt pan/scroll punti 1–3) |
| **`experiment/test-6-with-new-font`** | UI **precedente** / “vecchia interfaccia” (font experiment, **senza** le opt pan/scroll 1–3 del branch successivo) |

**Setup run “vecchia UI”** (stesso cgroup del test 4, per confronto diretto):

```text
branch pressbrakepeg: experiment/test-6-with-new-font
cpu.max = 2000 20000     # 10% @ period 20 ms  (come test 4)
cpuset.cpus = 0-2
```

#### Risultati — `test-6-with-new-font` + `2000 20000`

| Metrica | Valore |
|---------|--------|
| Attivazioni | **248 000** (~12 min*) |
| `nanosleep` min | **12 µs** |
| `nanosleep` max | **83 µs** |
| Valori **> 100 µs** | **0** |
| DDR `lpddr4` | **~3,0–3,2%** |
| Worst `rtc_handler_us` | **83 µs** @ iter **66 413** |
| L2 miss (worst) | **27,43%** |

| Intervallo (µs) | Occorrenze |
|-----------------|----------:|
| 60–70 | **201** |
| 71–80 | **3** |
| 81–90 | **1** |
| 91–99 | **0** |
| **Σ 60–99** | **205** |
| **Σ / att.** | **0,083%** |

```text
valore massimo della nanosleep delle ultime 248000 attivazioni vale = 83
valore minimo della nanosleep delle ultime 248000 attivazioni vale = 12
i valori sopra ai 100 us nelle ultime 248000 attivazioni sono = 0
intervallo 60-70: 201
intervallo 71-80: 3
intervallo 81-90: 1
intervallo 91-99: 0
```

```text
Core: CPU3
[WORST rtc_handler_us]
Iterazione:           66413
rtc_handler_us:       83
l2d_cache:            9164
l2d_cache_refill:     2514
L2 cache miss:        27.4334 %
bus_access:           10093
bus_cycles:           326738
bus_access/bus_cycles: 0.030890
bus_cycles/bus_access: 32.3727
cpu_cycles:           649059
istruzioni:           151710
IPC:                  0.233738
CPI:                  4.278288
```

#### Confronto diretto — stesso cgroup `2000 20000`

| Metrica | Test 4 (`pan-scroll-opt`) | Vecchia UI (`test-6-with-new-font`) |
|---------|--------------------------:|-----------------------------------:|
| Attivazioni | 280 000 | 248 000 |
| max `nanosleep` | **101** | **83** |
| spill **>100** | **1** | **0** |
| 60–70 | 307 | **201** |
| 71–80 | 12 | **3** |
| 81–90 | 8 | **1** |
| 91–99 | 2 | **0** |
| Σ 60–99 / att. | 0,118% | **0,083%** |
| DDR % | **~2,5–2,6** | ~3,0–3,2 |
| L2 miss (worst) | **16,35%** | **27,43%** |
| Worst µs | 101 @ 53732 | **83** @ 66413 |

> **Lettura:** a parità di `2000 20000`, la **vecchia UI** (`test-6-with-new-font`) ha **max più basso** (83 vs 101), **0 spill** e coda 60–99 più magra; il worst ha però **L2 miss più alto** (~27% vs ~16%) e DDR% un filo più alta. Non è un verdetto “vecchia meglio in assoluto”: scenario/carico GUI possono differire; va ripetuto a **scenario allineato** (stesso scroll/pagina) e idealmente anche **senza cgroup** su entrambi i branch.

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
| **`CAD_DIAG_CALCULATE=1`** | ogni chiamata stampa `[CAD] diag …` su stderr con `fflush` — **⚠️ interferisce con RT** (vedi sotto) |

> **Attenzione:** con l’ottimizzatore in esecuzione (`Optimization in progress`, dialog STOP/Continue) i log possono essere **continui e molto numerosi** — l’ottimizzatore richiama in loop `CalcolaNewSituaz`, `LookForBends`, redraw Sim2D a ogni piega provata. Usare solo per debug mirato, **non** in campagne RT lunghe.

> **Interferenza RT (2026-07-22):** attivare `CAD_DIAG` fa salire spike `nanosleep` sopra **100 µs** (es. max 117 µs in ~2 min). Spegnendo le macro i ritardi tornano normali. Dettaglio in [DIAGNOSTICA TEST 6 §A](#diagnostica-test-6).

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

#### Come leggere il pixel clock in `modetest -c`

L’output tipico è:

```text
modes:
  index name refresh (Hz) hdisp hss hse htot vdisp vss vse vtot
#0 1024x600 64.31 1024 1084 1154 1214 600 615 619 634 49500 flags: ; type: preferred, driver
```

L’**header si ferma a `vtot`**: non c’è una colonna intestata “pixel clock”, ma dopo `vtot` c’è ancora un numero. Allineamento reale:

| Campo | Valore (esempio avn8mp) |
|--------|-------------------------|
| index | `#0` |
| name | `1024x600` |
| refresh (Hz) | `64.31` |
| hdisp, hss, hse, htot | `1024 1084 1154 1214` |
| vdisp, vss, vse, vtot | `600 615 619 634` |
| **pixel clock** *(non in header)* | **`49500`** ← kHz → **49,5 MHz** |
| flags / type | `preferred, driver` |

**Fonte ufficiale (non è una convenzione inventata da `modetest`):** UAPI DRM  
[`include/uapi/drm/drm_mode.h`](https://elixir.bootlin.com/linux/latest/source/include/uapi/drm/drm_mode.h) — `struct drm_mode_modeinfo`:

```c
__u32 clock;   /* pixel clock in kHz */
```

`modetest` stampa `mode->clock` subito dopo i timing H/V. Stesso campo in libdrm (`drmModeModeInfo.clock`, in kHz).

**Output più leggibile** (etichette esplicite):

```bash
modetest -M imx-drm -c 2>/dev/null | awk '
/^#/ {
  printf "\nMode:        %s\n", $2
  printf "Refresh:     %s Hz\n", $3
  printf "H:           disp=%s  sync_start=%s  sync_end=%s  total=%s\n", $4,$5,$6,$7
  printf "V:           disp=%s  sync_start=%s  sync_end=%s  total=%s\n", $8,$9,$10,$11
  printf "Pixel clock: %s kHz  (%.2f MHz)\n", $12, $12/1000.0
}'
```

**Controlli indipendenti:** `htot × vtot × refresh ≈ clock` (1214 × 634 × 64,31 ≈ 49,5 MHz) e `grep media_disp2_pix /sys/kernel/debug/clk/clk_summary`.

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

> **⚠️ Interferenza RT confermata (2026-07-22):** con `CAD_DIAG_CALCULATE` **attivo**, anche in pochi minuti di test compaiono spike **`nanosleep` / `rtc_handler_us` > 100 µs** (es. max **117 µs**, diversi conteggi sopra soglia). Stesso scenario con macro **commentate / off** → ritardi di nuovo in linea con le campagne Test 6 (max tipicamente ≤ ~100–102 µs).  
> **Causa:** `fprintf` + **`fflush(stderr)`** su console (SSH/seriale) → I/O bloccante e contesa CPU/DDR con il path RT su CPU3. Non è un “dialog CAD” in sé: sono le **stampe di diagnostica**.  
> **Regola:** campagne di misura RT / endurance → **`CAD_DIAG` sempre off** (nessun `DEFINES += CAD_DIAG_CALCULATE` nei `.pro`). Usare solo per debug crash Calculate, poi spegnere e rideployare.

#### Disattivare (stderr pulito — produzione / misure RT)

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
| **Campagna misure GUI/RT** | `RT_STATS` on · **`CAD_DIAG` off** (obbligatorio — interferenza RT 2026-07-22) · protocollo in [Protocollo misura](#protocollo-misura-standard) |
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
| `[LISTE] …` (ProgList/ListaView/FileView/MatListVw/ToolListTabVw…) | **Già spento (2026-07-30)**: guard `LISTE_DIAG_ENABLED` in `liste/liste_diag.h`, di default non definito → `ListeDiag()` è un no-op. Per riattivare, scommentare `#define LISTE_DIAG_ENABLED` in quel file. Copre 30 call site in 7 file |
| `[LISTE] SaveAs …` / `[LISTE] OnSelChange …` | 12 `fprintf` **hardcoded** in `liste/SaveAsListVw.cpp` (non passano da `ListeDiag`): scattano **solo** aprendo la pagina Save As, quindi non influenzano le misure se durante il test non la si apre |
| `[LISTE] InsertRow refused …` | `editorbase/SpreadSheetBase.cpp`: scatta solo in condizione anomala (guard anti-crash), lasciato attivo di proposito |
| `[CAD] diag LookForBends: …` | Rimuovere `CAD_DIAG_CALCULATE` da `sim2d.pro` + `ottimizzatore.pro` — **già commentato in entrambi** |
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

### Come abilitare `cpu.max` restando **solo cgroup v2** (rebuild kernel / BSP)

**Sì:** senza quelle `CONFIG_*` non c’è modo userspace di “accendere” il controller `cpu`. Serve **ricompilare (e ridistribuire) il kernel** dell’immagine avn8mp — tipicamente nel **BSP Yocto**, non in `pegenstein`.

**Non** serve (e non conviene) tornare a cgroup v1: avete già `cgroup_no_v1=all`. Obiettivo = **tenere v2 puro** e solo aggiungere il controller `cpu` + bandwidth CFS.

#### 1) Opzioni kernel da abilitare

Nel `defconfig` / fragment della macchina (kernel **6.6.23-rt28** o quello dell’immagine):

| Symbol | Valore | Perché |
|--------|--------|--------|
| `CONFIG_CGROUPS` | `y` | già attivo (avete già `cpuset`/`io`) |
| `CONFIG_CGROUP_SCHED` | **`y`** | espone il controller **`cpu`** |
| `CONFIG_FAIR_GROUP_SCHED` | **`y`** | scheduling a gruppi per CFS (dipendenza tipica) |
| `CONFIG_CFS_BANDWIDTH` | **`y`** | abilita **`cpu.max`** (quota/period) |
| `CONFIG_RT_GROUP_SCHED` | **`n`** (lasciare off) | su PREEMPT_RT + systemd, con RT group sched spesso **non** si riesce ad abilitare `+cpu` finché ci sono task RT fuori dal root cgroup. PegExec è CFS; i task RT (Lnk) restano fuori dal cgroup GUI |

**Non toccare** (per evitare mix v1/v2):

- cmdline: lasciare **`cgroup_no_v1=all`** (solo v2);
- **non** montare `/sys/fs/cgroup/cpu` legacy;
- **non** abilitare hybrid hierarchy.

#### 2) Percorso consigliato in Yocto (fragment, non patch a mano sul tree)

Nella **build machine** del BSP (path tipici: `build/`, recipe `linux-imx` / `linux-fslc-rt` / nome usato dal vendor avn8mp):

1. Creare un fragment, es. `recipes-kernel/linux/files/cgroup-cpu-bandwidth.cfg`:

```cfg
CONFIG_CGROUP_SCHED=y
CONFIG_FAIR_GROUP_SCHED=y
CONFIG_CFS_BANDWIDTH=y
# CONFIG_RT_GROUP_SCHED is not set
```

2. Collegarlo alla recipe kernel della macchina, es. in un `.bbappend`:

```bitbake
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append = " file://cgroup-cpu-bandwidth.cfg "
```

(Con kernel Yocto moderni i `.cfg` in `SRC_URI` vengono uniti al defconfig; se la recipe usa solo `KBUILD_DEFCONFIG`, seguire la doc del BSP: `DELTA_KERNEL_DEFCONFIG` / `KERNEL_FEATURES` / `configfragments`.)

3. Rebuild **solo** il kernel (+ eventuale image che lo include):

```bash
# dalla build directory Yocto (adattare MACHINE=… del BSP avn8mp)
bitbake -c cleansstate virtual/kernel   # opzionale ma evita config stale
bitbake virtual/kernel
# oppure immagine completa:
# bitbake <nome-image-avn8mp>
```

4. Deploy sul target: `Image`/`Image.gz` + DTB + moduli coerenti (stesso ABI), poi **reboot**.  
   Metodi tipici: aggiornamento WIC/sdcard, `boot` partition, oppure pacchetto `kernel-image`/`kernel-modules` del feed Yocto — **come già fate per gli altri update kernel del prodotto**.

> I path esatti (`MACHINE`, nome recipe, layout boot) dipendono dal **repo Yocto vendor** (non sono in `pegenstein`). Prima di bitbake: `bitbake -e virtual/kernel | grep -E '^(PREFERRED_PROVIDER_virtual/kernel|MACHINE|KMACHINE)='`.

#### 3) Verifica post-reboot (solo v2)

```bash
uname -r
zcat /proc/config.gz | grep -E 'CGROUP_SCHED|CFS_BANDWIDTH|RT_GROUP_SCHED'
# atteso:
# CONFIG_CGROUP_SCHED=y
# CONFIG_CFS_BANDWIDTH=y
# # CONFIG_RT_GROUP_SCHED is not set

cat /proc/cmdline | tr ' ' '\n' | grep cgroup
# atteso: cgroup_no_v1=all   (nessun ritorno a v1)

cat /sys/fs/cgroup/cgroup.controllers
# atteso: ... cpu ...  (oltre a cpuset io)

echo '+cpu' > /sys/fs/cgroup/cgroup.subtree_control
# atteso: nessun errore

# throttle PegExec ~10 ms ON / 10 ms OFF (= 50% di un core)
mkdir -p /sys/fs/cgroup/peg_gui_rt
echo '10000 20000' > /sys/fs/cgroup/peg_gui_rt/cpu.max
echo $(pidof PegExec) > /sys/fs/cgroup/peg_gui_rt/cgroup.procs
cat /sys/fs/cgroup/peg_gui_rt/cpu.stat   # nr_throttled / throttled_usec
```

Oppure lo script già presente: **`pressa/perf_terminale/peg_cgroup_throttle.sh`** — istruzioni complete in [Script peg_cgroup_throttle — uso](#peg-cgroup-throttle-uso).

#### 4) Caveat RT (importanti)

- `cpu.max` limita i task **CFS** (SCHED_OTHER/BATCH) nel cgroup — tipicamente **PegExec**.  
  **Non** limita i thread **SCHED_FIFO/RR** (es. `COM RTC Handler` / Lnk): restano fuori o non sono soggetti a quella quota.
- Non mettere i task RT dentro il cgroup GUI throttled (e con `CONFIG_RT_GROUP_SCHED=n` non è necessario per usare `cpu` su CFS).
- Misurare RT **prima/dopo** con lo stesso protocollo (warm-up 30–60 s, `CAD_DIAG` off).

#### 5) Alternativa se non si vuole rifare tutto il kernel tree a mano

Se il BSP espone già un defconfig editabile: `bitbake -c menuconfig virtual/kernel` → abilitare le stesse voci sotto *General setup → Control Group support → CPU controller* / *Group scheduling for SCHED_OTHER* / *CPU bandwidth provisioning for FAIR_GROUP_SCHED*, salvare, poi `bitbake virtual/kernel`. Preferibile comunque **fragment `.cfg` in layer** così resta riproducibile in CI.

### Sintesi (stato al 2026-07-21, pre-rebuild)

> Sull’immagine avn8mp di allora la limitazione con `cpu.max` **non era realizzabile** (`CONFIG_CGROUP_SCHED` off). Dopo rebuild kernel (`esa-image-qt5-dbg`, 2026-07-22/23) il controller **`cpu` è disponibile** — vedi campagna sotto.

<a id="cgroups-campagna-quota-2026-07-23"></a>

### Campagna quota PegExec + banda DDR + RT (2026-07-23 / 24)

<a id="peg-cgroup-throttle-uso"></a>

#### Script `peg_cgroup_throttle.sh` — istruzioni d’uso

**File (PC):** `pressa/perf_terminale/peg_cgroup_throttle.sh`  
**Prerequisito board:** kernel con controller `cpu` (`cat /sys/fs/cgroup/cgroup.controllers` deve contenere `cpu`).  
**Prerequisito runtime:** `PegExec` già in esecuzione; comandi da lanciare come **root**.

**Copia sulla board:**

```bash
# da WSL / PC
scp /home/leolevro/github/lvgl/pressa/perf_terminale/peg_cgroup_throttle.sh root@avn8mp:/root/

# sulla board
sed -i 's/\r$//' /root/peg_cgroup_throttle.sh   # se arriva da Windows
chmod +x /root/peg_cgroup_throttle.sh
```

**Comandi:**

| Comando | Effetto |
|---------|---------|
| `./peg_cgroup_throttle.sh detect` | Mostra se cgroup è v1/v2 e i mount |
| `./peg_cgroup_throttle.sh start` | Attiva throttle su PegExec — default **`10000 20000`** (10 ms ON / 10 ms OFF ≈ **50%** di un core) |
| `./peg_cgroup_throttle.sh start QUOTA PERIOD` | Stesso, con quota/period in **µs** (es. `5000 20000` = 25%, `2000 20000` = 10%) |
| `./peg_cgroup_throttle.sh status` | Path cgroup, `cpu.max`, PID PegExec, **tutto** `cpu.stat` |
| `./peg_cgroup_throttle.sh reset` | `stop` + `start` (default 50%) → **azzera** i contatori `cpu.stat` |
| `./peg_cgroup_throttle.sh reset QUOTA PERIOD` | Reset con quota scelta |
| `./peg_cgroup_throttle.sh stop` | Toglie PegExec dal cgroup e rimuove il throttle |

**Significato di `QUOTA PERIOD`:** ogni `PERIOD` µs il gruppo può usare al massimo `QUOTA` µs di CPU CFS (non vale per i thread RT di Lnk).

```text
10000 20000  →  10 ms / 20 ms  →  ~50% di un core
 5000 20000  →   5 ms / 20 ms  →  ~25%
 2000 20000  →   2 ms / 20 ms  →  ~10%
```

**Esempio sessione di misura:**

```bash
# PegExec già avviato; warm-up GUI ≥ 30–60 s; poi Lnk
cd /root
./peg_cgroup_throttle.sh reset 5000 20000          # 25%
watch -n0.5 ./peg_cgroup_throttle.sh status        # altro terminale
perf stat -a -I 1000 -M imx8mp_bandwidth_usage.lpddr4

# fine test
./peg_cgroup_throttle.sh stop
```

**Note:**
- Al **reboot** il cgroup sparisce (config volatile): rifare `start`/`reset`.
- Se `start` fallisce con `cpu.max missing` → controller `cpu` non abilitato (kernel senza `CONFIG_CGROUP_SCHED` / `CFS_BANDWIDTH`).
- `nr_throttled` / `throttled_usec` in `status` salgono solo se PegExec **sfora** la quota (a riposo possono restare a 0).

---

**Setup campagna sotto:** Test 6 (o 0-SDL), scenario **scroll grafico ~10 min**, `CAD_DIAG` off.  
**RT:** max `nanosleep` / proxy scheduler su CPU3.  
**Baseline DRM:** nessun `cpu.max`. **Test con quota:** PegExec in `peg_gui_rt`, `cpuset.cpus=0-2`.

#### Comando `perf` usato (banda totale)

```bash
perf list metric | grep -i imx8mp_bandwidth   # verifica nome metrica
perf stat -a -I 1000 -M imx8mp_bandwidth_usage.lpddr4
```

- `-a` = system-wide (tutti i master sulla DDR)  
- `-I 1000` = campione ogni **1 s**  
- `-M imx8mp_bandwidth_usage.lpddr4` = metrica NXP (%% di utilizzo LPDDR4)

In parallelo: `watch -n0.5 ./peg_cgroup_throttle.sh status` (solo test 1–3) e monitor RT (Lnk).

#### Cosa significa la % di `imx8mp_bandwidth_usage.lpddr4`

**Non** è “quanto è piena la RAM”, né “quanto usa PegExec”, né % CPU.

È:

```text
%  =  (byte letti + scritti sulla DDR in quell’1 s, tutti i master)
      /  (banda teorica di picco LPDDR4)
      ×  100
```

Su i.MX8MP NXP usa tipicamente un picco teorico ≈ **16 GB/s** (= **16 000 MB/s**) per questa metrica. Quindi:

```text
MB/s_totale  ≈  (% / 100)  ×  16 000
```

Esempio: **4,5 %** → `0,045 × 16 000` ≈ **720 MB/s** di traffico R+W in quell’intervallo.

Include **tutto**: CPU, LCDIF/scanout, GPU se attiva, DMA, audio, …  
La % resta ~4% anche con cgroup perché lo **scanout** legge il FB in continuo; PegExec è solo una fetta. Stringere la CPU di PegExec toglie soprattutto le **write** a burst → la % totale scende poco.

#### Tabella confronti (SDL vs DRM + quota cgroup × banda × RT)

Stesso comando `perf` (`imx8mp_bandwidth_usage.lpddr4`) e stesso scenario **scroll grafico ~10 min**, così i MB/s sono **confrontabili tra loro**.

| Test | Path GUI | `cpu.max` | Quota ≈ | Throttle | Banda DDR **%** | Banda DDR **MB/s** ≈ `%/100 × 16000` | **nanosleep max (10 min)** |
|------|----------|-----------|---------|----------|----------------:|---------------------------------------:|---------------------------:|
| **0-SDL** | Test 0 / branch **`lvgl-hmi`** (SDL) | *(nessuno)* | no throttle | — | ~**7,6–13,1 %** | ~**1 216–2 096 MB/s** | **130 µs** |
| **6-base** | Test 6 DRM | *(nessuno)* | no throttle | — | ~**4,0–4,2 %** | ~**640–672 MB/s** | **99 µs** |
| **6-50%** | Test 6 DRM | `10000 20000` | **50%** | quasi mai (3/204k) | ~**4,4–4,6 %** | ~**704–736 MB/s** | **93 µs** |
| **6-25%** | Test 6 DRM | `5000 20000` | **25%** | forte (~27% periodi) | ~**3,2–4,6 %** | ~**512–736 MB/s** | **91 µs** |
| **6-10%** | Test 6 DRM | `2000 20000` | **10%** | molto forte (~53%) | ~**2,5–3,4 %** | ~**400–544 MB/s** | **71 µs** |

**Calcolo MB/s (stesso per ogni riga):**  
`MB/s ≈ (percentuale_osservata / 100) × 16000`  
con `16000 MB/s` = picco teorico LPDDR4 usato dalla metrica NXP (~16 GB/s).

**Lettura:**

1. **0-SDL vs 6-base (stessa metrica %):** in scroll, SDL usa ~**7,6–13 %** della DDR (~1,2–2,1 GB/s) vs DRM ~**4,0–4,2 %** (~0,64–0,67 GB/s). Coerente col vecchio confronto cycles (SDL più traffico di DRM), ora misurato con lo **stesso** strumento NXP.  
2. **RT:** SDL **130 µs** vs DRM senza throttle **99 µs** — Test 6 migliora anche il worst-case senza cgroup.  
3. Su DRM, stringere la quota **25%→10%** porta nanosleep **99 → 91 → 71 µs**; il **50%** cambia poco rispetto a 6-base.  
4. La % DDR cala poco col throttle: resta dominata da **scanout/sistema**.  
5. **Trade-off:** sotto ~25% di quota, RT meglio ma GUI più a scatti.

**Nota sul vecchio confronto `read/write-cycles`:** quella tabella (SDL ~513 vs DRM ~328 MB/s medi) resta valida come confronto *relativo* col metodo cycles×8; i valori assoluti non coincidono con `% × 16000` perché sono **eventi diversi**. Qui, con `imx8mp_bandwidth_usage.lpddr4` su entrambe le build, il confronto assoluto SDL↔DRM è allineato.

**Comandi ripetitibili:**

```bash
# 0-SDL — branch lvgl-hmi, PegExec senza cgroup cpu
./peg_cgroup_throttle.sh stop 2>/dev/null
perf stat -a -I 1000 -M imx8mp_bandwidth_usage.lpddr4

# 6-base — Test 6 DRM senza throttle
./peg_cgroup_throttle.sh stop 2>/dev/null
perf stat -a -I 1000 -M imx8mp_bandwidth_usage.lpddr4

# 6-50% / 25% / 10%
./peg_cgroup_throttle.sh reset 10000 20000
./peg_cgroup_throttle.sh reset 5000 20000
./peg_cgroup_throttle.sh reset 2000 20000
perf stat -a -I 1000 -M imx8mp_bandwidth_usage.lpddr4
```

---

## File modificati vs Test 0

Inventario dei sorgenti toccati **dalla condizione iniziale di Test 0** allo stato attuale (Test 6 + stabilità CAD + experiment font #2).  
Baseline git pegenstein: commit `7acaf1d` (*Strumentazione RT e test riduzione risoluzione 800x600*). Branch corrente tipico: `experiment/test-6-with-new-font` (PegLib) / `lvgl-hmi` (pressbrakepeg) / `experiment/test-6-with-new-font` (kvuib).

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

Branch: `experiment/test-6-with-new-font` (ex `experiment/test6-drm`).

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

Branch: `experiment/test-6-with-new-font`. Commit tipico: *togliere bitmap Yahei embedded ridondanti*.

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

**Branch:** `experiment/test-6-with-new-font` in **kvuib** (twin di pegenstein).

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

<a id="editor-manual-defer-2026-07-28"></a>

## P — Editor/Manual: feedback immediato + lavoro pesante differito 500 ms (2026-07-28)

**Stato:** ⏳ **Promettente, da confermare** · **Repo/Branch:** `pressbrakepeg` → `experiment/test-6-deferred-ch0-feedback` · [← Tabella](#stato-test)

---

**Problema:** premendo molto velocemente e ripetutamente i tasti **Editor ↔ Manual** (cambio "stato macchina" IMP/MAN in `CPpgView`), il thread RT (`COM RTC Handler`) mostrava picchi di `nanosleep` fino a **145 µs**, ben oltre il rumore di fondo normale. Causa: `AttivaPaginaCH0()` esegue in modo **sincrono e bloccante**, ad ogni pressione, `SettaControlli()` → `SettaAssi()` (centinaia di `Add()`/`Remove()` su `PegThing` per mostrare/nascondere gli assi), `GetEntry()` → `UpdateData()` (rilettura/ridisegno di ~150 edit) e `AttivaMenu()` (toolbar/softkey). Altri cambi pagina "pesanti" (es. Program list ↔ Die list) non mostravano lo stesso pattern di picchi.

**Idea (prof. Paolo Valente):** separare il feedback visivo immediato del bottone dal lavoro vero, differendo quest'ultimo di un tempo fisso (500 ms) dall'ultima pressione: l'utente vede subito il bottone attivarsi, ma `SettaControlli`/`GetEntry` partono solo quando sono trascorsi davvero 500 ms senza altre pressioni. Martellando i tasti, resta in coda solo l'ultima richiesta.

**Modifica:** `AttivaPaginaCH0(nStato)` (`CMDINum`) diviso in due fasi:
- **Immediato, sempre:** `ApplyCH0ToolbarFeedback()` (evidenziazione tasto) + stato macchina (`ScriviStatoMacchina`, `QuotaPosOK`, flag `theApp.m_bFirstTimeInStop/Start`) — letto in modo sincrono da `GestionePagine::KeyCambiaPagina`/`OnImpostazioni`/`OnMan` per decidere la navigazione; se restasse "vecchio" per 500 ms i tasti sembrerebbero non rispondere.
- **Differito 500 ms, solo per IMP/MAN:** `CompletaAttivaPaginaCH0()` (`SettaControlli`, `GetEntry`, `SettaFocusOnNome`, `AttivaMenu`), tramite timer periodico `TIMER_CH0_DEFER` (poll ogni 100 ms; ad ogni tick ricalcola se sono trascorsi ≥ 500 ms dall'ultima pressione). Gli altri stati (AUTO/SAUTO/…) restano sincroni come prima.

File toccati: `editorprogrammi/MDINum.{h,cpp}`, `editorprogrammi/PpgViewBase.{h,cpp}`, `editorprogrammi/PpgView.cpp`, `IncPPG/CommonConst.h`.

**Come cambiare il ritardo (300/500/… ms):** valore unico, costante `CH0_DEFER_DELAY_MS` in cima a `editorprogrammi/MDINum.cpp` (subito dopo gli `#include`):

```cpp
static const DWORD CH0_DEFER_DELAY_MS = 300; // prima: 500
```

Usata dentro `CMDINum::HandleCH0DeferTimer()` nel confronto `if(elapsed < CH0_DEFER_DELAY_MS) return TRUE;` — è l'unico punto del codice con il valore effettivo, non serve toccare altro. Il timer di polling (`TIMER_CH0_DEFER`, `PpgViewBase.cpp`, `ScheduleCH0HeavyWork`) resta a **100 ms** di granularità (`ONE_SECOND/10`): con soglie ≥ 200 ms va bene così com'è; se si scendesse molto sotto i 100 ms andrebbe ridotto anche il periodo del poll. Dopo la modifica basta ricompilare `libeditorprogrammi.so` e rideployare.

**2026-07-28 (pomeriggio):** ritardo ridotto **500 → 300 ms** per lo stesso test, in attesa di ripetere la campagna di martellamento Editor/Manual e confrontare il nuovo massimo `nanosleep` con gli **88 µs** misurati a 500 ms. *(Risultato da aggiungere qui appena disponibile.)*

**Bug trovati e risolti durante l'implementazione (stesso branch):**
- **Crash SIGSEGV in `PegGroup::Add`:** `KillTimer()` non svuota i `PM_TIMER` già in coda in PegLib; il primo tentativo uccideva il timer troppo presto causando esecuzioni multiple/rientranti con puntatori incoerenti. Fix: il timer resta periodico e si ferma **da solo**, solo dopo aver verificato i 500 ms ed eseguito il lavoro.
- **Navigazione rotta ("indietro" che non rispondeva) + testo strano:** causato da `ScriviStatoMacchina()` inizialmente lasciato nella parte differita, creando una finestra di 500 ms in cui `LeggiStatoMacchina()` restava vecchio. Fix: stato macchina spostato nella parte immediata; solo il lavoro grafico pesante resta differito.
- **Campi sovrapposti premendo Editor→Editor ripetutamente:** comportamento preesistente (non introdotto qui) — ripremere Editor mentre si è già in Editor fa un vero cambio pagina Impostazioni ↔ Impostazioni-Zoom (`CambiaPagina`, `Remove()`/`Add()` reali tra due oggetti `CPpgView`/`CPpgViewZoom`). Il gestore `PM_HIDE` uccideva altri timer della pagina ma non il nuovo `TIMER_CH0_DEFER`: se restava armato mentre la pagina veniva nascosta, scattava più tardi rifacendo `SettaControlli`/`GetEntry` sopra al setup già fatto da `OnChiave0` in `PM_SHOW`, duplicando i controlli a video. Fix: `PM_HIDE` ora chiama `CancelCH0HeavyWorkSchedule()` e resetta lo stato completato.

**Scenario:** martellamento rapido Editor ↔ Manual, ~137 000 attivazioni consecutive.

| Attivazioni cumulative | `nanosleep` min | `nanosleep` max | Spike **> 100 µs** |
|---|---|---|---|
| 137 000 | **12 µs** | **88 µs** | **0** |
| ultime 7 000 | — | **61 µs** | — |

**Distribuzione (137 000 att.):**

| Intervallo (µs) | Conteggio |
|---|---|
| 60–70 | 104 |
| 71–80 | 17 |
| 81–90 | 2 |
| 91–99 | 0 |

**Log esempio:**
```text
****************************************************
valore massimo della nanosleep delle  137000 attivazioni vale = 88
valore minimo della nanosleep delle ultime 137000 attivazioni vale = 12
i valori sopra ai 100 us nelle ultime137000 attivazioni sono = 0
****************************************************

I valori nell'intervallo 60 70 sono = 104
I valori nell'intervallo 71 80 sono = 17
I valori nell'intervallo 81 90 sono = 2
I valori nell'intervallo 91 99 sono = 0
valore massimo della nanosleep delle ultime 7000 attivazioni vale = 61
****************************************************
```

**Confronto:** senza defer, lo stesso martellamento Editor/Manual arrivava a picchi ripetuti **102–145 µs**. Con il defer attivo: massimo sceso a **88 µs**, **zero** valori sopra i 100 µs su tutte le 137 000 attivazioni.

**Conclusione:** ⏳ **Promettente** — il pattern di picchi anomali (> 100 µs) tipico del martellamento Editor/Manual sembra eliminato dal defer del lavoro pesante. Da confermare con sessioni più lunghe e pattern di pressione diversi prima di considerarla soluzione definitiva.
**Stato codice:** attivo su branch experiment (`experiment/test-6-deferred-ch0-feedback`), non ancora mergiato in `test-6-with-new-font` / produzione.
**Prossimi passi:** ripetere il test più a lungo; valutare lo stesso pattern di defer per altre transizioni di stato macchina (Auto/Semiauto/Corrections); tenere in sospeso l'ipotesi alternativa IPI da `PerfMonitor` (`PerfMonitor_Init` resta disattivato in `PlcEsa/Lnk/main.cpp` per isolare l'effetto del solo defer).

---

<a id="guida-ai-defer-editor-manual"></a>

## Q — Guida prompt AI per risolvere il problema del ritardo Editor/Manual

> Questa sezione serve a **riaprire il problema in futuro** con una AI, partendo da zero, senza ripetere gli errori e i bug che abbiamo incontrato la prima volta. Copia il blocco "Prompt da incollare" in una nuova chat e la AI avrà tutto il contesto necessario.

---

### Contesto da sapere prima di aprire la chat

| Cosa | Dettaglio |
|------|-----------|
| Repo codice | `pressbrakepeg` (repo separato, non `pegenstein`) |
| Branch di riferimento | `experiment/test-6-deferred-ch0-feedback` (contiene già la soluzione funzionante) |
| File chiave | `editorprogrammi/MDINum.{h,cpp}`, `editorprogrammi/PpgViewBase.{h,cpp}`, `editorprogrammi/PpgView.cpp`, `IncPPG/CommonConst.h` |
| Metrica RT misurata | `nanosleep` max del thread `COM RTC Handler` (CPU3), log `PegExec` su target i.MX8M Plus |
| Obiettivo | Nessun valore `nanosleep` > 100 µs durante martellamento rapido Editor ↔ Manual |
| Soluzione adottata | Defer di 500 ms (ora 300 ms) del lavoro pesante (`SettaControlli`/`GetEntry`/`AttivaMenu`) dal momento dell'ultima pressione, con feedback toolbar e stato macchina **sempre immediati** |
| Costante da modificare per cambiare il delay | `CH0_DEFER_DELAY_MS` in cima a `editorprogrammi/MDINum.cpp` |

---

### Prompt da incollare in una nuova chat AI

```
Contesto: sto lavorando su un HMI industriale (pressa piegatrice) basato su PegLib
su i.MX8M Plus, Linux PREEMPT_RT. Il codice applicativo è nel repo "pressbrakepeg".

PROBLEMA:
Premere rapidamente e ripetutamente i tasti Editor ↔ Manual causa picchi di jitter
nel thread RT "COM RTC Handler" (nanosleep max fino a 145 µs, obiettivo < 100 µs).
Il percorso critico è CMDINum::AttivaPaginaCH0(int nStato), chiamato da
CPpgView::OnImpostazioni() (tasto Editor, stato IMP) e OnMan() (tasto Manual, stato MAN).
Ogni chiamata esegue in modo sincrono e bloccante: SettaControlli() → SettaAssi()
(centinaia di Add/Remove su PegThing), GetEntry() → UpdateData() (~150 edit),
AttivaMenu() (toolbar/softkey). Questi sono il "lavoro pesante".

SOLUZIONE DA IMPLEMENTARE:
Dividere AttivaPaginaCH0 in due fasi:
1. IMMEDIATA (eseguita subito a ogni pressione):
   - feedback toolbar: ApplyCH0ToolbarFeedback(nStato) → solo KvaraMenu()
   - aggiornamento stato macchina: ScriviStatoMacchina(nStato), QuotaPosOK(0., RES),
     theApp.m_bFirstTimeInStop = TRUE, theApp.m_bFirstTimeInStart = FALSE
   ATTENZIONE: ScriviStatoMacchina DEVE essere immediato. GestionePagine::KeyCambiaPagina,
   OnImpostazioni, OnMan leggono LeggiStatoMacchina() in modo sincrono subito dopo la
   pressione per decidere la navigazione. Se resta "vecchio" per qualche centinaia di ms,
   i tasti sembrano non rispondere o la navigazione si confonde.

2. DIFFERITA di CH0_DEFER_DELAY_MS ms dall'ultima pressione (solo per stati IMP e MAN):
   CompletaAttivaPaginaCH0(nStato): SettaControlli(), GetEntry(), SettaFocusOnNome(),
   AttivaMenu(nStato). Meccanismo: timer periodico TIMER_CH0_DEFER (poll ogni 100 ms);
   a ogni tick controlla se GetTickCount() - m_dwTimeLast >= CH0_DEFER_DELAY_MS; se sì,
   esegue il lavoro e si ferma. Per AUTO/SAUTO/CORR/POSI: nessun defer, lavoro immediato.

TRAPPOLE DA EVITARE (errori già incontrati in precedenza):

1. KillTimer NON svuota i PM_TIMER già in coda in PegLib.
   Se uccidi TIMER_CH0_DEFER nel gestore PM_TIMER dopo il primo tick, ci sono già altri
   PM_TIMER in coda che continuano ad arrivare. Il risultato è CompletaAttivaPaginaCH0
   chiamata più volte in rapida successione → PegGroup::Add con puntatori incoerenti →
   SIGSEGV. Soluzione: il timer deve essere periodico e fermarsi DA SOLO, solo dopo aver
   verificato l'elapsed e completato il lavoro (dentro HandleCH0DeferTimer, chiamare
   CancelCH0HeavyWorkSchedule() solo quando si decide di eseguire davvero il lavoro).
   Non killare il timer in PM_TIMER a ogni tick.

2. Il timer di defer NON deve sopravvivere all'hide della pagina.
   In PegLib, premere Editor mentre si è già in Editor provoca un vero CambiaPagina
   tra la pagina Impostazioni (CPpgView) e la pagina Impostazioni-Zoom (CPpgViewZoom):
   sono due oggetti C++ distinti. La pagina nascosta riceve PM_HIDE. Se TIMER_CH0_DEFER
   è ancora armato sulla pagina nascosta, scatterà più tardi e rifarà SettaControlli/
   GetEntry sopra al setup già fatto da OnChiave0 in PM_SHOW → Add() duplicati →
   controlli sovrapposti, testo "sporco" a video. Soluzione: nel gestore PM_HIDE di
   CPpgView::Message aggiungere CancelCH0HeavyWorkSchedule() e
   m_nLastCompletedCH0State = -1 prima di chiamare CPpgViewBase::Message(Mesg).

3. La guardia m_bCH0Completing protegge solo dalla rientranza nello stesso stack.
   Non protegge da due timer fires separati nel tempo. Per questo il timer deve essere
   periodico con autodistruzione, non un one-shot.

4. Prima configurazione del layout: non differire la prima volta.
   Se m_nLastCompletedCH0State < 0 (pagina non ancora configurata), eseguire
   CompletaAttivaPaginaCH0 subito senza defer: il layout non è ancora pronto e il defer
   causerebbe Add() su controlli non inizializzati.

5. Non skippare se nStato == m_nLastCompletedCH0State solo dentro HandleCH0DeferTimer.
   Va bene skippare lì (risparmio inutile), ma AttivaPaginaCH0 deve comunque aggiornare
   m_dwTimeLast e armare il timer per ogni pressione, perché l'utente potrebbe aver
   cambiato dati nel frattempo.

FILE DA LEGGERE PRIMA DI SCRIVERE CODICE:
- editorprogrammi/MDINum.h e MDINum.cpp (classe CMDINum, AttivaPaginaCH0)
- editorprogrammi/PpgViewBase.h e PpgViewBase.cpp (override ScheduleCH0HeavyWork/
  CancelCH0HeavyWorkSchedule con SetTimer/KillTimer, gestore PM_TIMER)
- editorprogrammi/PpgView.cpp (gestore PM_HIDE e PM_SHOW, OnChiave0, OnImpostazioni,
  OnMan)
- IncPPG/CommonConst.h (definizione TIMER_CH0_DEFER)

VALIDAZIONE PRIMA DI DICHIARARE "FUNZIONA":
1. Premi Editor → Manual → Editor normalmente, verifica che la UI cambi correttamente.
2. Premi Editor più volte di fila (già in Editor): verifica che non compaiano campi
   sovrapposti o testo "sporco" (questo è il bug del PM_HIDE/CambiaPagina).
3. Premi Manual più volte di fila (già in Manual): stesso controllo.
4. Martella velocemente Editor ↔ Manual ~10 volte: verifica che dopo la pausa la UI
   mostri lo stato corretto (non uno stato intermedio).
5. Vai in Auto o Semiauto: verifica che quei passaggi siano ancora immediati e corretti.
6. Solo dopo questi check, misurare nanosleep con Lnk attivo.
```

---

### Riepilogo bug incontrati la prima volta (con fix)

| # | Bug | Causa | Fix |
|---|-----|-------|-----|
| 1 | SIGSEGV in `PegGroup::Add` | `KillTimer` non svuota la coda PM_TIMER → `CompletaAttivaPaginaCH0` chiamata più volte con stato inconsistente | Timer periodico che si ferma da solo solo a lavoro completato |
| 2 | Navigazione rotta, "tasto indietro non risponde" | `ScriviStatoMacchina` messo nella parte differita → `LeggiStatoMacchina()` letto "vecchio" per ~500 ms da `GestionePagine` | Spostare stato macchina nella parte **immediata** |
| 3 | Testo/campi sovrapposti premendo Editor→Editor | `TIMER_CH0_DEFER` sopravviveva a `PM_HIDE` → rifaceva `SettaControlli` sopra setup fresco di `OnChiave0` in `PM_SHOW` | `CancelCH0HeavyWorkSchedule()` + reset `m_nLastCompletedCH0State` in `PM_HIDE` |

---

<a id="ch0-defer-estensione-corr-auto-sauto-2026-07-29"></a>

## R — Estensione defer CH0 a Correzioni/Auto/Semiauto: analisi rischio (2026-07-29)

**Stato:** ✅ **Defer CH0 validato funzionalmente (2026-07-30)** nel suo scenario (programma numerico, si resta sulla pagina numerica): `DEFER` da 2ª pressione in poi, con batching che coalesce davvero il lavoro pesante. Resta da fare la misura RT pulita. ⚠️ **Scoperto un secondo problema indipendente (filone B)**: con programma **grafico** caricato i tasti stato fanno due cambi pagina completi per pressione (27→0→27) — è da lì che veniva il jitter di ~130 µs misurato finora, e il defer CH0 non lo tocca. · **Repo/Branch:** `pressbrakepeg` + `pegenstein` → `experiment/test-6-ch0-defer-corr-auto-sauto-estensione` · [← Tabella](#stato-test) · Vedi anche [sezione P](#editor-manual-defer-2026-07-28)

### ⚠️ → ✅ Bug trovato in validazione (2026-07-29): `PM_HIDE` azzera il defer ad ogni pressione — RISOLTO

**Sintomo:** log di debug temporanei (`fprintf(stderr, "[AI-DEBUG-CH0] ...")`, poi rimossi da entrambi i repo) hanno mostrato che `m_nLastCompletedCH0State` è **sempre -1** in `AttivaPaginaCH0`, anche ripremendo più volte di fila **lo stesso** stato (es. Manual→Manual). Questo forzava sempre il ramo "prima configurazione, esegui subito" (`if(m_nLastCompletedCH0State < 0)`), che chiama `CompletaAttivaPaginaCH0` in modo **sincrono**, mai tramite il timer differito — il defer, di fatto, non si attivava mai oltre il primissimo ingresso in pagina.

**Causa reale (trovata analizzando un dump esteso di `PegThing::Remove()`/`MessageChildren()` con 100 righe di contesto prima dell'evento):** non è un attore esterno che "rompe" il defer — è **`CompletaAttivaPaginaCH0` stessa** che si auto-boicotta:

1. `CompletaAttivaPaginaCH0(nStato)` chiama `SettaControlli()`/`GetEntry()` (righe 124-125 di `MDINum.cpp`), che ricreano da zero tutti i controlli numerici della pagina (~40-50 widget: edit, label, bottoni). La `Remove()` dei vecchi controlli, essendo essi visibili, genera **sincronicamente** un `PM_HIDE` che risale fino al contenitore client-area della pagina.
2. Quel `PM_HIDE` arriva a `CPpgView::Message` (case `PM_HIDE`, `PpgView.cpp`), che **incondizionatamente** faceva `CancelCH0HeavyWorkSchedule(); m_nLastCompletedCH0State = -1;` — pensato per il caso di navigazione vera verso un'altra pagina (es. toggle con Zoom via `CambiaPagina`, vedi bug #3 in tabella sopra), ma scattava **anche per il rebuild interno della stessa pagina causato da noi stessi**.
3. Solo *dopo* essere tornati da `SettaControlli()`/`GetEntry()`/`AttivaMenu()`, `CompletaAttivaPaginaCH0` esegue `m_nLastCompletedCH0State = nStato;` (riga 138) — ma la finestra in cui lo stato resta a -1 durante il rebuild, unita al fatto che l'intera sequenza è sincrona all'interno della stessa pressione, manteneva il sistema sempre nel ramo "prima configurazione".

In pratica: ogni volta che il lavoro pesante veniva eseguito (anche una sola volta), il suo stesso side-effect (`PM_HIDE` autoindotto) cancellava la prova che fosse stato eseguito, quindi la pressione successiva ripartiva sempre da "mai configurato" — bloccando il defer per sempre, indipendentemente dallo stato o da quante volte si martellava lo stesso pulsante.

**Escluso durante l'indagine:** `CGestionePagine::CambiaPagina` (guard esplicito, e IMP/MAN/CORR/AUTO/SAUTO non lo chiamano nemmeno); `KDBUI_Salvataggio` (no-op senza tabella KDB attiva); tutti gli altri `Remove()` di PegLib (`PegGroup::Remove`, `PegPresentationManager::Remove`) — confermato che tutti confluiscono nello stesso `PegThing::Remove()` già strumentato.

**Fix applicato in `pressbrakepeg/editorprogrammi/PpgView.cpp`, case `PM_HIDE`:**

```cpp
// [AI] 2026-07-29 - BUG FIX: CompletaAttivaPaginaCH0() (SettaControlli/GetEntry/
// AttivaMenu) ricrea i controlli della pagina; la Remove() dei vecchi controlli
// genera essa stessa un PM_HIDE sincrono verso questa view (side-effect interno,
// non una vera navigazione via CambiaPagina). Senza guardia, quel PM_HIDE
// azzerava m_nLastCompletedCH0State ad OGNI pressione, quindi AttivaPaginaCH0
// prendeva sempre il percorso sincrono "prima configurazione" e il defer non
// scattava mai. Durante CompletaAttivaPaginaCH0, m_bCH0Completing e' TRUE: un
// PM_HIDE che arriva in quella finestra e' autoindotto e va ignorato. Un
// PM_HIDE vero (pagina nascosta per davvero, m_bCH0Completing==FALSE) continua
// a resettare tutto come prima.
if(!m_bCH0Completing)
{
    CancelCH0HeavyWorkSchedule();
    m_nLastCompletedCH0State = -1;
}
```

`m_bCH0Completing` è già `TRUE` per tutta la durata di `SettaControlli()`/`GetEntry()`/`AttivaMenu()` dentro `CompletaAttivaPaginaCH0` (impostato a riga 122, resettato a riga 139) — quindi la guardia ignora esattamente e solo i `PM_HIDE` autoindotti dal nostro stesso rebuild, mentre un `PM_HIDE` genuino (navigazione reale verso un'altra pagina, fuori da questa finestra) continua a resettare `m_nLastCompletedCH0State` come previsto dal fix del bug #3 in tabella.

**⚠️ RETTIFICA (2026-07-30) — questa conclusione era SBAGLIATA.** Qui era stato scritto che il defer Editor/Manuale originale (sezione P) non aveva mai davvero effettuato il batching, e che la misura di 88 µs su 137 000 attivazioni andava riletta come "comportamento sempre sincrono". **Non è vero.** Quell'ipotesi era stata tratta da log raccolti tutti in scenari in cui il defer *non può* funzionare per costruzione (toggle Zoom↔Normale `0↔58`, e pagina CAD `27→0→27`): in entrambi la pagina numerica viene realmente distrutta e ricreata, quindi il reset di `m_nLastCompletedCH0State` è legittimo. Nello scenario per cui il defer è stato scritto (programma **numerico**, si resta sulla pagina numerica, si alternano gli stati) il batching **funziona**, come verificato empiricamente il 2026-07-30 (vedi "Validazione funzionale filone A" più sotto): non c'è alcun `PM_HIDE` sulla pagina, quindi `m_nLastCompletedCH0State` sopravvive tra le pressioni **anche senza** il guard `m_bCH0Completing`. Le misure della sezione P sono quindi da considerare **valide**. Il guard resta in codice come difesa corretta ma, in quello scenario, non cambia il comportamento.

**Debug rimosso (2026-07-29, seconda ripulitura):** tutti i `fprintf(stderr, "[AI-DEBUG-CH0] ...")` e i relativi `#include <cstdio>` sono stati rimossi da `pressbrakepeg/editorprogrammi/PpgView.cpp` e `pegenstein/PegLib/source/pthing.cpp` (in `PegThing::Remove()` e `PegThing::MessageChildren()`). Nessuna istrumentazione di debug residua nei due repo.

**Prossimo passo:** ricompilare, distribuire, e ripetere il test di martellamento IMP/MAN/CORR/AUTO/SAUTO seguendo la checklist di validazione qui sotto — verificare sia l'effetto visivo (ritardo di ~300 ms nell'aggiornamento campi) sia il jitter RT (target <100 µs).

**Aggiornamento 2026-07-29 (dopo primo test col fix):** ricompilato e distribuito, ma Leonardo non nota ancora alcuna differenza (né visiva né RT). Il ragionamento sul fix (guard `m_bCH0Completing`) regge alla rilettura del codice, quindi prima di cercare un'altra causa serve la conferma empirica che: (a) il binario testato contenga davvero questa modifica, (b) `m_nLastCompletedCH0State` sopravviva ora tra una pressione e l'altra. Aggiunta un'unica riga di debug temporanea in `AttivaPaginaCH0` (`MDINum.cpp`):

```cpp
fprintf(stderr, "[AI-DBG2] AttivaPaginaCH0 nStato=%d ultimoCompletato=%d\n", nStato, m_nLastCompletedCH0State);
```

Test richiesto: martellare un solo pulsante (es. Manual, già in Manual) 5-6 volte e condividere il log `[AI-DBG2]`. Atteso se il fix funziona: 1ª pressione `ultimoCompletato=-1`, dalla 2ª in poi `ultimoCompletato=1` (MAN) — cioè il percorso differito viene preso. Se resta sempre `-1`, il fix non è efficace (o il binario non è quello nuovo) e serve riaprire l'indagine.

**Risultato test:** `[AI-DBG2]` compare nel log (conferma binario aggiornato), ma `ultimoCompletato` resta **sempre -1** anche dopo il fix. Il fix del guard non era sbagliato, ma **non era la causa reale** — serviva capire perché.

### ✅ Causa reale trovata (seconda indagine, 2026-07-29): non è un side-effect interno, è il toggle Zoom↔Normale

Aggiunti altri debug mirati (`CompletaAttivaPaginaCH0` entry/exit, `PM_SHOW`/`PM_HIDE` con indirizzo `this` e valore di `m_bCH0Completing`, entry/exit di `OnMan()`). Il log ha mostrato la sequenza esatta per ogni pressione:

```
AttivaPaginaCH0 nStato=4 ultimoCompletato=-1
CompletaAttivaPaginaCH0 ENTRY/EXIT -> m_nLastCompletedCH0State=4   (impostato correttamente!)
OnMan EXIT
PM_HIDE this=0xAAA m_bCH0Completing=0   (arriva DOPO che OnMan e' gia' tornato, non durante il rebuild)
PM_SHOW this=0xBBB -> chiamo OnChiave0   (indirizzo diverso da prima!)
OnMan ENTRY  (di nuovo, stato ancora -1)
```

L'indirizzo `this` **alterna tra due oggetti C++ distinti** (`0xAAA`/`0xBBB`) ad ogni pressione, anche premendo sempre lo stesso tasto. Non è un bug: è una funzionalità storica del 2007 (commento `12/10/07 183.8.1: "Pagine zoommate"` in `GestionePagine.cpp`). Per ciascun tasto-stato (F1/IMP, e lo stesso pattern per MAN/AUTO/SAUTO), se lo stato richiesto è **già quello attivo**, il codice non fa nulla di idempotente: esegue esplicitamente

```cpp
if(m_nPaginaAttiva==PAG_IMPOSTAZIONI)
    CambiaPagina(PAG_IMP_ZOOM);
else
    CambiaPagina(PAG_IMPOSTAZIONI);
```

cioè **alterna deliberatamente** tra la pagina "normale" (`CPpgView`) e la sua variante "zoom" (`CPpgViewZoom : public CPpgView`, file `PpgViewZoom.cpp`) — pensata per ingrandire il pannello numerico su schermi piccoli. Sono due istanze C++ separate, ognuna con il proprio `m_nLastCompletedCH0State` (membro ereditato da `CMDINum`): per questo lo stato non può mai "sopravvivere" da una pressione alla successiva quando si martella lo stesso tasto — non è lo stesso oggetto a ricevere le due pressioni consecutive.

**Conclusione:** il fix del guard `m_bCH0Completing` (par. precedente) resta corretto e utile per il suo scopo originale (side-effect interni durante il rebuild), ma il sintomo "martellando non cambia nulla" non dipendeva da quello: dipende dal fatto che **martellare lo stesso tasto già attivo non è un no-op in questa applicazione, è il gesto "attiva/disattiva zoom"**, che comporta di per sé lo smontaggio completo di una pagina e il montaggio dell'altra — lavoro pesante by design, non ottimizzabile dal defer CH0 così com'è strutturato (per-istanza).

**Decisione presa con Leonardo:** cambiare la metodologia di validazione invece di allargare lo scope del fix. D'ora in poi il martellamento va fatto **alternando stati diversi** (es. Editor→Manual→Auto→Manual→Corr, mai lo stesso stato due volte di fila), che è anche il pattern realistico di un operatore — martellare lo stesso tasto già attivo è un caso d'uso a parte (zoom toggle) esplicitamente lasciato fuori scope per ora. Se in futuro servisse ottimizzare anche quel caso, andrebbe condiviso lo stato di defer tra `CPpgView` e `CPpgViewZoom` (es. rendendo `m_nLastCompletedCH0State`/`m_nPendingCH0State`/`m_bCH0WorkPending`/`m_bCH0Completing` variabili di classe `static` invece che membri di istanza) — modifica più ampia, da valutare a parte.

**Debug rimosso (2026-07-29, terza ripulitura):** tutti gli `[AI-DBG2]` (in `AttivaPaginaCH0`, `CompletaAttivaPaginaCH0`, `PM_SHOW`, `PM_HIDE`, `OnMan` entry/exit) e i relativi `#include <cstdio>` sono stati rimossi da `MDINum.cpp` e `PpgView.cpp`. Il commento nel case `PM_HIDE` è stato corretto per riflettere la causa reale (toggle Zoom, non side-effect interno). Il guard `m_bCH0Completing` resta in codice (difesa valida, anche se non era la causa del sintomo osservato).

**Prossimo passo:** ripetere il test alternando stati diversi (non lo stesso tasto due volte di fila) e verificare sia l'effetto visivo (ritardo ~300 ms) sia il jitter RT.

### 🔑 Scoperta decisiva (2026-07-30): il test avveniva dalla pagina CAD 2D, non dalla pagina numerica

Anche alternando Piece Set↔Manual il defer non si innescava (sempre `SYNC`, `ultimoCompletato=-1`), e la pressione di Piece Set non produceva **nessun** log. Istrumentando il punto d'ingresso unico `CGestionePagine::KeyCambiaPagina` (tasto ricevuto, pagina attiva, stato macchina, flag di configurazione) e `CGestionePagine::CambiaPagina` (da→a), il quadro è emerso completamente.

**Mappatura icone toolbar (confermata dal codice):** i pulsanti touch della toolbar sintetizzano `PM_KEY` con `PK_F1+n` (vedi `SendToolBarBtnUsr`/`SendVertToolBarBtnUsr` in `pegmain/PegMain.cpp`). Quindi:

| Icona | Tasto | Stato | Handler |
|-------|-------|-------|---------|
| Documento con righe | `PK_F1` (650) | **IMP** — Piece Set | `case PK_F1` in `KeyCambiaPagina` |
| Mano | `PK_F2` (651) | **MAN** — Manual | `case PK_F2` |
| Chiave inglese | `PK_F3` (652) | **SAUTO** — Semi-automatico | `case PK_F3` |
| Fabbrica | `PK_F4` (653) | **AUTO** — Automatico | `case PK_F4` |

(Le due che davano "Program not optimized!" erano quindi chiave inglese = Semi-auto e fabbrica = Auto; la "C" che dava "General data are not complete!" è CORR.)

**Log rilevante (una pressione Piece Set + tre pressioni Manual):**

```
KeyCambiaPagina nKey=650 (F1) pagAttiva=27 stato=1 riassunto=0 semplified=0 typePpgView=1
CambiaPagina da=27 a=27 (SCARTATO: stessa pagina)          <- Piece Set: NO-OP totale
KeyCambiaPagina nKey=651 (F2) pagAttiva=27 stato=1 ...
CambiaPagina da=27 a=0                                      <- smonta CAD, monta pagina numerica
OnChiave0 pPaginaEditor=0xffffa0002320 nStatoSucc=4
AttivaPaginaCH0 nStato=4 -> SYNC (ultimoCompletato=-1)      <- lavoro pesante, sempre sincrono
CambiaPagina da=0 a=27                                      <- torna SUBITO alla pagina CAD
```

**Fatti accertati:**

1. **`pagAttiva=27` = `PAG_CAD2D_PEZZO`** — i test venivano fatti stando sulla **pagina CAD 2D del pezzo** (programma grafico caricato), non sulla pagina numerica. Il titolo "Piece Set." visualizzato appartiene a quella pagina.
2. **Premere Piece Set (F1) in quello scenario è un no-op completo**: `CambiaPagina(27→27)` viene scartato dal guard `if(m_nPaginaAttiva == nIDNextPag) return FALSE;`. Nessun lavoro, nessun jitter, nessun log — coerente con l'assenza di righe `nStato=1`.
3. **Premere Manual (F2) costa due cambi pagina completi + il rebuild dei controlli**: `27→0`, poi `OnChiave0`→`AttivaPaginaCH0(MAN)`, poi immediatamente `0→27`. Ogni cambio pagina distrugge e ricostruisce interi alberi di widget e ridisegna lo schermo (il ritorno automatico a 27 avviene perché il programma caricato è grafico: la logica `tOpenImpGraf`/CAD riporta sulla pagina del pezzo).
4. **Il defer CH0 non può strutturalmente agire in questo scenario**, e il reset di `m_nLastCompletedCH0State` a -1 è in questo caso **corretto**: la pagina numerica viene realmente nascosta e ri-mostrata da zero ad ogni pressione (cambio pagina genuino, non un falso positivo come si era ipotizzato).

**Conseguenza sull'interpretazione di tutti i test precedenti:** il jitter di ~130 µs misurato martellando i tasti stato **con un programma grafico caricato** non nasce da `SettaControlli`/`GetEntry` (il bersaglio del defer CH0), ma dalla **macchina dei cambi pagina** (`CambiaPagina`: teardown/rebuild alberi widget + ridisegno completo), eseguita due volte per pressione. Nessuna ottimizzazione del defer CH0 può ridurre quel costo.

**Il defer CH0 resta valido nello scenario per cui è stato scritto:** restare sulla pagina numerica e alternare gli stati (IMP/MAN/CORR/AUTO/SAUTO) — condizione che si verifica con un **programma numerico** caricato, dove `IsSettingNumericPage(m_nPaginaAttiva)` è vero e non c'è rimbalzo verso la pagina CAD. Va validato in quello scenario.

**Due filoni distinti da qui in avanti (da non confondere più):**

| Filone | Scenario | Costo dominante | Stato |
|--------|----------|-----------------|-------|
| **A — defer CH0** | Programma numerico, si resta sulla pagina numerica, si alternano gli stati | `SettaControlli`/`GetEntry`/`AttivaMenu` | ✅ **Validato funzionalmente** (vedi sotto) |
| **B — cambi pagina** | Programma grafico (CAD), i tasti stato fanno `CambiaPagina` avanti/indietro | Teardown/rebuild alberi widget + ridisegno completo, ×2 per pressione | ❌ Non affrontato; è la causa del jitter osservato finora |

### ✅ Validazione funzionale filone A riuscita (2026-07-30)

Test con **programma numerico** caricato (nessun rimbalzo verso la pagina CAD), alternando Piece Set (F1/IMP) ↔ Manual (F2/MAN) restando su `pagAttiva=0`:

```
CambiaPagina da=-1 a=0                                  <- ingresso pagina numerica
AttivaPaginaCH0 nStato=1 -> SYNC  (ultimoCompletato=-1) <- 1a volta: sincrono, corretto
tasto=651 pagAttiva=0 stato=1
AttivaPaginaCH0 nStato=4 -> DEFER (ultimoCompletato=1)  <- da qui in poi SEMPRE defer
tasto=650 pagAttiva=0 stato=4
AttivaPaginaCH0 nStato=1 -> DEFER (ultimoCompletato=1)  <- ultimo resta 1: lavoro pesante MAI eseguito
tasto=651 pagAttiva=0 stato=1
AttivaPaginaCH0 nStato=4 -> DEFER (ultimoCompletato=1)  <- idem: batching che coalesce
tasto=650 pagAttiva=0 stato=4
AttivaPaginaCH0 nStato=1 -> DEFER (ultimoCompletato=4)
...
```

**Esiti confermati:**

1. **Nessuna riga `CambiaPagina` tra le pressioni** → lo scenario testato è quello giusto (si resta sulla stessa istanza di pagina numerica).
2. **`SYNC` solo alla primissima attivazione**, poi **`DEFER` ad ogni pressione successiva** → il meccanismo di differimento si innesca correttamente. È la prima volta che questo viene osservato empiricamente da quando il defer è stato scritto.
3. **`ultimoCompletato` resta invariato per più pressioni consecutive** (es. resta `1` per tre pressioni di fila) → prova diretta che `CompletaAttivaPaginaCH0` **non è stata eseguita affatto** per quelle pressioni: il batching sta davvero coalescendo il lavoro pesante, non solo rimandandolo.
4. Nella coda dello stesso log si vede il **filone B ripresentarsi puntualmente** appena viene caricato un programma grafico dalla Program List (`0→29`, `29→27`, poi `27→0→27` con `SYNC` ad ogni Manual) — conferma che i due filoni sono indipendenti e che il filone B resta interamente aperto.

**Debug rimosso (2026-07-30, ripulitura finale):** tutti i marker `[AI-DBG3]`/`[AI-DBG4]`/`[AI-DBG5]` e i relativi `#include <cstdio>` rimossi da `MDINum.cpp`, `PpgView.cpp`, `EditorProgDef.cpp`, `GestionePagine.cpp` (pressbrakepeg) e `pthing.cpp` (pegenstein). Verificato con grep: 0 occorrenze in entrambi i repo. In codice resta solo il guard `m_bCH0Completing` nel case `PM_HIDE` di `PpgView.cpp`.

**Prossimo passo:** misura RT pulita (senza log, che con `fflush` sincrono falserebbero misure a 100 µs) nello scenario del filone A: programma numerico, martellamento IMP/MAN/CORR alternati, verifica `nanosleep`/`rtc_handler_us` < 100 µs. Poi eventualmente aprire il filone B.

### ⚖️ Cosa aggiunge realmente questo branch (chiarimento 2026-07-30)

Leonardo ha osservato che martellando Piece Set ↔ Manual il "ritardo enorme" non si presenta più, e ha chiesto se sia merito delle modifiche. Risposta onesta: **quasi certamente no, non di queste modifiche.**

Il defer su **IMP/MAN esisteva già prima** di questo branch (sezione P, già in produzione). Ciò che è cambiato nei test è lo **scenario**, non il codice: ora il test viene fatto con un **programma numerico** restando su `pagAttiva=0`, mentre prima veniva fatto dalla pagina CAD con programma grafico, dove ogni pressione costava `27→0→27`. Stesso codice, condizioni diverse, risultato molto diverso.

**Contributo effettivo e nuovo di questo branch:**

| Modifica | Effetto reale | Scenario in cui si vede |
|----------|---------------|--------------------------|
| Defer esteso a **CORR** | Prima CORR faceva `SettaControlli`/`GetEntry`/`AttivaMenu` **tutto sincrono** ad ogni pressione; ora è differito come IMP/MAN | Martellare Corr alternato ad altri stati |
| Defer **parziale** su **AUTO/SAUTO** | Prima tutto sincrono; ora `SettaControlli`/`GetEntry` differiti, `AttivaMenu` (→ `ConfigButtonsStartStopPlusMinus`, tasti ciclo macchina) resta **sempre immediato** per sicurezza | Martellare Auto/Semiauto alternati ad altri stati |
| Guard `m_bCH0Completing` in `PM_HIDE` | Difesa corretta contro reset da `PM_HIDE` autoindotti, ma nello scenario numerico non cambia il comportamento (nessun `PM_HIDE` sulla pagina) | Nessuno misurabile finora |
| Nessuna modifica | IMP/MAN | Il miglioramento osservato su Piece Set↔Manual è preesistente |

**Per stabilire con certezza il guadagno di questo branch serve un A/B**: stesso scenario (programma numerico, pagina numerica), stessa metodologia di martellamento, misurato su branch baseline vs. questo branch — e va martellato includendo **CORR/AUTO/SAUTO**, non solo IMP/MAN, perché è lì che il branch aggiunge qualcosa.

---

<a id="merge-ch0-defer-pan-scroll-2026-07-30"></a>

## T — Merge finale: defer CH0 + ottimizzazione pan/scroll (2026-07-30)

**Branch di integrazione:** `experiment/test-6-ch0-defer-plus-pan-scroll` (repo `pressbrakepeg`), creato da `experiment/test-6-deferred-ch0-feedback` con merge di `experiment/test-6-font-pan-scroll-opt`. I due branch originali restano intatti come via di rollback. · [← Tabella](#stato-test)

**Contenuto:** le due ottimizzazioni **validate**:

- **defer CH0 su IMP/MAN** (sezione P) — batching del lavoro pesante al cambio stato Editor/Manuale
- **ottimizzazione pan/scroll grafico** (sezione "Ottimizzazioni pan/scroll grafici") — `DrawPanIfDue` con coalescing temporale ~16 ms, skip griglia durante il tracking

**Esplicitamente NON incluso:** l'estensione del defer a **CORR/AUTO/SAUTO** (sezione R) e il guard `m_bCH0Completing`, che restano sul branch `experiment/test-6-ch0-defer-corr-auto-sauto-estensione`. Motivo: il loro guadagno non è dimostrato da misure, e AUTO/SAUTO toccano stati operativi reali della macchina — meglio chiudere con due modifiche validate che con tre di cui una incerta.

**Esito del merge:** nessun conflitto (`Merge made by the 'ort' strategy`), working tree pulito. Presenza di entrambe le funzionalità verificata con grep (`CH0_DEFER_DELAY_MS` in `MDINum.cpp`, `DrawPanIfDue` in `Sim2DView.cpp`).

**Osservazione importante sul rischio di regressione.** Il `git diff --stat` tra i due branch elencava **20 file** (inclusi `liste/ListaView.cpp` +67, `liste/MatListVw.cpp` +74, `liste/ToolListTabVw.cpp` +40, `editorbase/SpreadSheetBase.cpp` +46), il che aveva fatto temere un impatto sul sottosistema **Die List / Program List** — quello con lo storico di crash. Il merge però ha modificato **solo 8 file**:

```
cad2d/MatView.cpp      |  2 +-
cad2d/Pezzoview.cpp    |  2 +-
cad2d/Ppgviews.cpp     | 34 ++++++++++--
cad2d/Ppgviews.h       |  5 +++++
cad2d/Punzview.cpp     |  2 +-
liste/SaveAsListVw.cpp |  2 +-
sim2d/Sim2DView.cpp    | 31 +++++++++++--
sim2d/Sim2DView.h      |  5 +++++
8 files changed, 75 insertions(+), 8 deletions(-)
```

Se il merge non ha toccato i file `liste/` ed `editorbase/`, significa che **quelle modifiche erano già presenti nel branch baseline** (contenuto già identico). Questo spiega anche perché i log `[LISTE] ProgList PM_SHOW` / `FileView::Create` comparivano già durante i test sul branch CH0. **Conseguenza:** il sottosistema liste non è stato toccato da questo merge, il rischio di regressione lì è basso, e l'unica cosa realmente nuova da validare è il **pan/scroll grafico** (`cad2d/` + `sim2d/`).

**Checklist prima della misura RT definitiva:**

- [ ] `pegenstein` su un branch coerente e working tree pulito (i due repo devono corrispondere)
- [ ] Smoke test Die List ↔ Program List (percorso con storico di crash, ora a rischio ridotto)
- [ ] Smoke test scroll grafico (Die Set.) e cambio stato Editor↔Manual
- [x] **Diagnostica spenta (fatto 2026-07-30)** — stato verificato file per file:
  - `[LISTE]` → **spenta**: aggiunto guard `LISTE_DIAG_ENABLED` (non definito) in `liste/liste_diag.h`; `ListeDiag()` diventa un no-op, copre 30 call site in 7 file (`ListaView`, `MatListVw`, `ToolListTabVw`, `ProgListVw`, `PunListVw`, `FileView`, `ListeDef`)
  - `EMBEDDED_HMI_RT_STATS` → **già commentato** in `pegenstein/PegLib/PegLib.pro` (riga 9)
  - `EMBEDDED_HMI_RT_DIAG`, `EMBEDDED_HMI_RT_NATIVE_TEXTURE`, `EMBEDDED_HMI_RT_SAFE` → già commentati
  - `EMBEDDED_HMI_RT_DRM_DIRECT` → **resta attivo**: non è diagnostica, è la funzionalità del Test 6 (DRM dumb buffer RGB565)
  - `CAD_DIAG_CALCULATE` → **già commentato** in `sim2d.pro` e `ottimizzatore.pro`
  - `[AI-DBG*]` / `[AI-DEBUG-CH0]` → rimossi (grep: 0 occorrenze in entrambi i repo)
  - ⚠️ residuo noto: 12 `fprintf` hardcoded in `liste/SaveAsListVw.cpp` — scattano solo aprendo la pagina **Save As**: non aprirla durante la misura
- [x] Rebuild dopo aver spento la diagnostica, poi misura → **eseguita, vedi sezione U**

---

<a id="test-finale-merged-scroll-calculation-2026-07-30"></a>

## U — Test finale su branch merged + analisi scroll pagina "Calculation" (2026-07-30)

**Branch:** `experiment/test-6-ch0-defer-plus-pan-scroll` (defer CH0 IMP/MAN + ottimizzazione pan/scroll), diagnostica `[LISTE]` disattivata. · [← Tabella](#stato-test) · Vedi anche [sezione T](#merge-ch0-defer-pan-scroll-2026-07-30)

### Risultati misura

| Metrica | Valore |
|---------|--------|
| Attivazioni | **1 589 000** |
| `nanosleep` **max** | **113 µs** |
| `nanosleep` **min** | 11 µs |
| Valori **> 100 µs** | **8** |
| 60–70 µs | 495 |
| 71–80 µs | 121 |
| 81–90 µs | 58 |
| 91–99 µs | 16 |
| **Durata sessione** | 1 589 000 × 4 ms/attivazione = 6 356 s ≈ **1 h 46 min** |
| **Throttling cgroup** | ❌ **nessuno** (CPU piena) |

**Osservazione dell'operatore:** i valori più alti e il jitter sono comparsi **appena iniziato a martellare lo scroll del grafico della pagina "Calculation"** — non durante l'uso normale né durante i cambi stato.

### ⚠️ Confronto con la sezione S: NON valido come confronto codice-vs-codice

La sezione S era stata misurata **con throttling cgroup 6000/40000 (~15% CPU)**; questo test è stato fatto **senza alcun throttling**, a CPU piena. Sono quindi cambiate **due variabili contemporaneamente** — il codice *e* le condizioni di carico — perciò le differenze osservate **non sono attribuibili alle modifiche di questo branch**. È lo stesso errore metodologico che ci ha fatto perdere giorni all'inizio (misure prese in scenari diversi e confrontate tra loro): va registrato come tale e non usato per trarre conclusioni sul guadagno del codice.

Numeri normalizzati per milione di attivazioni, **da leggere solo come fotografia delle due sessioni**, non come A/B:

| Fascia | Sezione S — 529 k att., **con** throttling ~15% | Questo test — 1 589 k att., **senza** throttling |
|--------|------------------------------------------------|--------------------------------------------------|
| 60–70 µs | 1 134 /M | 311 /M |
| 71–80 µs | 108 /M | 76 /M |
| 81–90 µs | 34 /M | 36 /M |
| 91–99 µs | 3,8 /M | 10 /M |
| **> 100 µs** | 7,6 /M | 5,0 /M |
| **max** | 109 µs | 113 µs |

**Cosa si può dire con onestà a partire da questo test, senza confronti impropri:**

- In **1 h 46 min di uso reale**, a CPU piena, con martellamento volontario dello scroll grafico, il jitter ha superato i 100 µs **8 volte su 1 589 000 attivazioni** — cioè **5 volte per milione (0,0005%)**, con massimo **113 µs**.
- L'obiettivo "sempre < 100 µs" **non è raggiunto**, ma gli sforamenti sono eventi rari e, soprattutto, **circoscritti a una condizione nota e riproducibile**: il martellamento dello scroll sulla pagina Calculation. Durante l'uso normale e i cambi stato non si sono presentati.
- Il massimo di 113 µs è nello stesso ordine di grandezza di tutte le misure precedenti: **nessuna delle ottimizzazioni fatte finora ha abbassato il tetto massimo**, hanno agito sulla frequenza degli eventi, non sul caso peggiore.

**Per ottenere un confronto valido con la sezione S** serve rieseguire questo test **con lo stesso throttling cgroup 6000/40000**, stessa metodologia di utilizzo e durata comparabile. È l'unico modo per attribuire un guadagno al codice. → **Fatto, vedi sotto.**

### ✅ Test con throttling cgroup ~15% — confronto valido con la sezione S (2026-07-30)

Riesecuzione **con throttling cgroup 6000/40000 (~15% CPU)**, cioè le stesse condizioni di carico della sezione S.

| Metrica | Valore |
|---------|--------|
| Attivazioni | **848 000** |
| Durata | 848 000 × 4 ms = 3 392 s ≈ **57 min** |
| Throttling | ✅ cgroup 6000/40000 (~15% CPU) |
| `nanosleep` **max** | **113 µs** |
| `nanosleep` **min** | 11 µs |
| Valori **> 100 µs** | **4** |
| 60–70 µs | 237 |
| 71–80 µs | 41 |
| 81–90 µs | 14 |
| 91–99 µs | 14 |
| max **ultime 8 000** att. | **98 µs** (sotto soglia) |

**Confronto a parità di throttling** (valori per milione di attivazioni):

| Fascia | Sezione S — 529 k att. | Branch merged — 848 k att. | Variazione |
|--------|------------------------|----------------------------|------------|
| 60–70 µs | 1 134 /M | **279 /M** | ✅ **−75%** |
| 71–80 µs | 108 /M | **48 /M** | ✅ **−55%** |
| 81–90 µs | 34 /M | **16,5 /M** | ✅ **−51%** |
| 91–99 µs | 3,8 /M | 16,5 /M | ⚠️ **peggiorata** (×4,3) |
| **> 100 µs** | 7,6 /M | **4,7 /M** | ✅ **−38%** |
| **max** | 109 µs | 113 µs | ➖ invariato di fatto |
| max ultime ~8–9 k att. | 75 µs | 98 µs | ⚠️ peggiorato (ma < 100) |

### Conclusioni difendibili

1. **La frequenza delle interferenze di livello medio è calata in modo netto e coerente**: le fasce 60–70, 71–80 e 81–90 µs scendono tutte di circa la metà o più (−75%, −55%, −51%). Non è un singolo numero fortunato: è un andamento monotono su tre fasce contigue, che è il segno di un miglioramento reale e non di rumore statistico.
2. **Gli sforamenti oltre 100 µs calano del 38%** in frequenza relativa (7,6 → 4,7 per milione).
3. **Il caso peggiore NON è migliorato**: 113 vs 109 µs. Questo vale per **tutte** le ottimizzazioni fatte in questo lavoro — hanno ridotto *quante volte* si genera interferenza, non *quanto vale il picco massimo*. È il risultato più importante da riportare, ed è coerente con l'analisi del filone B: il caso peggiore nasce dai cambi pagina completi e dal costo per-frame del disegno, non dal lavoro che il defer CH0 differisce.
4. **La fascia 91–99 µs è peggiorata** (3,8 → 16,5 per milione). Interpretazione plausibile: parte del lavoro che prima produceva picchi distribuiti nelle fasce basse ora viene **accorpato** dal batching in eventi meno frequenti ma singolarmente più costosi, che si addensano appena sotto la soglia. È il comportamento atteso di un meccanismo di coalescing e va detto esplicitamente, perché è un compromesso, non un effetto collaterale da nascondere.

**Variabile ancora non controllata:** il *mix di utilizzo*. La sezione S era "uso comune + scroll Die Set"; in questa sessione è stato incluso il martellamento volontario dello scroll della pagina Calculation, che è il caso peggiore noto. Se così è, il miglioramento è stato ottenuto in condizioni d'uso **più severe** della sezione S, il che rafforza il risultato anziché indebolirlo — ma va verificato prima di affermarlo in tesi.

### Analisi: si può ottimizzare lo scroll della pagina "Calculation"?

**Identificazione della schermata (certa):**

- Caption `IDS_CAP_OTTIMIZZA = "Calculation"` (`Files/All/Squeeze/PegUsrRes.ENG:663`), impostato in `CSim2DFrame::InitFrame()` → `sim2d/Sim2DFrame.cpp:646`
- Page ID: **`PAG_OTTIM_SIM2D`**, frame `CSim2DFrame`, vista del grafico **`CSim2DView`** (`sim2d/Sim2DView.cpp`), istanziata a `Sim2DFrame.cpp:644` nel ramo `case IMP: case MAN:`
- Softkey confermate in `CSim2DFrame::AttivaMenu()` (`Sim2DFrame.cpp:1392-1414`): Simulate / Optimize / All the solution / Draft Wkp / Rotate / Bend
- Il ramo SAUTO/AUTO usa invece `CAutGView` (derivata da `CSim2DView`) con caption diverso — **non** è questa schermata

**L'ottimizzazione del pan è GIÀ applicata qui.** `CSim2DView::DrawPanIfDue(BOOL bForce)` (`Sim2DView.cpp:1549-1568`, dichiarata in `Sim2DView.h:173`) fa già coalescing a 16 ms (`kPanMinRedrawIntervalMs`) con membri `m_dwLastPanDrawMs` / `m_bPanRedrawPending`. Catena eventi: `PM_LBUTTONDOWN` (riga 130) → `PM_POINTER_MOVE` (riga 167) → `OnMouseMove()` (riga 1572-1610) che imposta `m_bPanRedrawPending` e chiama `DrawPanIfDue(FALSE)` → `PM_LBUTTONUP` (riga 142) con frame finale forzato `DrawPanIfDue(TRUE)` (riga 158). **Non** chiama `Draw()`/`Invalidate()`/`UpdateAllViews()` ad ogni evento, e ridisegna **solo il canvas** (la form a destra, statusbar e toolbar sono sibling e non vengono toccate; `BeginDraw()/EndDraw()` batchano off-screen).

Quindi il jitter residuo **non** viene da un throttling mancante: viene dal **costo interno di ogni singolo frame di pan**.

**Margine residuo concreto identificato — ricalcolo collisioni per frame.** In `CSim2DView::DrawDisView()` (`Sim2DView.cpp:225-349`) viene chiamata `DrawCollisioni()` (righe **324** e **339**), che al suo interno esegue `check_collisioni_pezzo(...)` **fino a 2 volte per frame** (`Sim2DView.cpp:372-374`). Durante il pan la geometria **non cambia** — cambia solo `m_OriginDis` — quindi il risultato è invariante e viene ricalcolato inutilmente ad ogni frame.

Due strade, la seconda è quella già collaudata altrove in questo progetto:

1. cachare `nElemPiegaColl` e ricalcolarlo solo al cambio di piega/geometria
2. **saltare `DrawCollisioni` mentre `m_bLeftButtonDown` è TRUE** e ridisegnarla nel frame finale forzato del `LBUTTONUP` — è esattamente l'analogo dello skip griglia con `m_bTracking` già usato nelle viste CAD (`MatView`/`Pezzoview`/`Punzview`)

**Rischi da tenere presenti prima di toccare:**

- `CAutGView` **deriva** da `CSim2DView` e condivide `Message()`/`OnMouseMove()`: una modifica in `DrawDisView` impatta anche le pagine AUTO/SAUTO e la Sequenza Manuale (`TYPE_SEQ_MAN_VIEW`), i cui rami (righe **326-347**) eseguono side-effect pesanti **dentro il draw** (`Posiziona()`, `ChiudiPiega()`, `SettaFocusSuSequenza()`). **Non toccare quel ramo.**
- Nel ramo IMP la pagina ospita la finestrella CAD sovrapposta `CDraftPieceWnd` (`Sim2DFrame.cpp:783`): attenzione ad artefatti grafici se si sperimenta sul redraw parziale.
- Il commento a `Sim2DView.cpp:1548` documenta che un tentativo precedente di **redraw parziale** (`RectMove` + strisce) è stato **ritirato** perché peggiorava la fluidità: quella strada è già stata esclusa, non riproporla.
- In sim2d **non esiste** una `DisegnaGriglia` (lo sfondo è il solo `DisegnaVideo(m_rvideo)`, riga 238, già economico): il trucco dello skip griglia non ha equivalente diretto qui, l'analogia vale solo per il *pattern* di skip durante il tracking.

**Valutazione costo/beneficio.** L'intervento è contenuto (skip condizionale di una chiamata durante il drag) e segue un pattern già validato nel progetto, ma tocca una gerarchia condivisa con le pagine AUTO/SAUTO, che sono stati operativi reali della macchina. Da affrontare solo con tempo sufficiente per una validazione dedicata.

### ❌ Idea SCARTATA con misura: cachare il calcolo collisioni non serve (2026-07-30)

Prima di implementare, il costo di `check_collisioni_pezzo` è stato **misurato** con strumentazione temporanea (`clock_gettime(CLOCK_MONOTONIC)` attorno alle due chiamate di `DrawCollisioni`, statistiche accumulate e stampate una riga ogni 200 chiamate per non falsare la misura con l'I/O), martellando lo scroll della pagina Calculation:

```
[AI-COLL] chiamate=200  media=6 us max=347 us (drag=1 tipo=220)
[AI-COLL] chiamate=1000 media=4 us max=347 us (drag=1 tipo=220)
...
[AI-COLL] chiamate=5200 media=4 us max=347 us (drag=1 tipo=220)
```

**Risultato: media 4 µs per frame** su 5 200 chiamate, tutte durante il drag (`drag=1`). Cachare il risultato farebbe risparmiare ~4 µs su frame che hanno un budget di 16 ms: **beneficio trascurabile, idea scartata**. La strumentazione è stata rimossa e nel codice è rimasto solo un commento che documenta la misura (`Sim2DView.cpp`, in `DrawCollisioni`).

Il `max=347 µs` si è verificato **entro le prime 200 chiamate e non si è più ripetuto** in 5 200: è il costo della prima invocazione a cache fredda (code/data cache, allocazioni lazy), non un costo ricorrente.

**Valore di questo risultato negativo:** esclude con un numero l'unica ottimizzazione residua individuata su quella schermata. Il jitter di quella pagina **non** viene dal calcolo collisioni; il throttling del pan è già in place; quindi su `CSim2DView` non restano margini semplici. Eventuali interventi futuri dovrebbero puntare al costo di riscalatura/disegno dei poligoni (refactoring con cache in coordinate mondo), che è invasivo, oppure al filone B (cambi pagina).

#### 📉 Sessione dedicata: martellamento del grafico 2D (2026-07-30)

Configurazione: branch merged, path DRM, fix PegGL applicato, **throttling cgroup 6000/40000 (~15% CPU) attivo** (confermato).

| Metrica | Valore |
|---------|--------|
| Attivazioni | **191 000** |
| Durata | 764 s ≈ **12 min 44 s** |
| `nanosleep` **max** | **115 µs** |
| `nanosleep` min | 11 µs |
| Valori **> 100 µs** | **9** |
| 60–70 / 71–80 / 81–90 / 91–99 µs | 494 / 251 / 77 / 41 |
| max ultime 1 000 att. | 57 µs (martellamento cessato) |

**Confronto normalizzato** (per milione di attivazioni):

Tutte e tre le sessioni con **throttling ~15%**, quindi confrontabili sulle condizioni di carico (resta non controllata l'intensità del martellamento manuale):

| Fascia | Uso generale (848 k) | Martellamento **3D**, + fix (77 k) | Martellamento **2D** (191 k) |
|--------|--------------------------------|---------------------------------------------|------------------------------|
| **totale eventi > 60 µs** | — | 4 792 /M | **4 565 /M** |
| **> 100 µs** | 4,7 /M | **0 /M** | **47,1 /M** |
| **max** | 113 µs | **100 µs** | **115 µs** |

**Osservazione diagnostica interessante.** Il **volume complessivo** di interferenza è praticamente identico a quello della sessione 3D (4 565 contro 4 792 eventi > 60 µs per milione), ma il grafico 2D produce una **coda oltre i 100 µs** che nella sessione 3D era assente. Stessa quantità di disturbo, distribuzione diversa: il 2D genera **occasionali operazioni più costose**, non un carico costante più alto.

**Verifiche fatte, che escludono le ipotesi più economiche:**

- **Nessuna allocazione per frame** nel percorso 2D: `grep` di `new` / `malloc` / `CreateBitmap` in `sim2d/Sim2DView.cpp` → **nessuna occorrenza**. Il meccanismo che è stato trovato e corretto in `PegGL` (riallocazione a ogni swap) **non si ripete qui**.
- **Throttling del pan già presente**: `DrawPanIfDue` con coalescing a 16 ms.
- **Calcolo collisioni già escluso**: 4 µs/frame misurati (vedi sopra).

**Il costo è dunque intrinseco: rasterizzazione software di 7 poligoni pieni per frame** — `PegDrawPolygon` chiamata per matrice (riga 896), punzone (930), superiore (988), inferiore (1024), pezzo (1095), riscontro (1299) e collisioni (407) — più il fondo. Tutto su CPU, coerente con il fatto che su questa architettura la GPU non è coinvolta.

**➡️ Leva residua, a costo di una riga: aumentare l'intervallo di coalescing del pan.**

`sim2d/Sim2DView.cpp:1554`:

```cpp
static const DWORD kPanMinRedrawIntervalMs = 16;   // [AI] allineato a ~60 Hz present
```

Portandolo a **33 ms (~30 fps)** si **dimezza** il numero di frame renderizzati durante il trascinamento, e con essi il lavoro di rasterizzazione e la banda consumata; a **50 ms (20 fps)** si riduce a un terzo. È una modifica di una riga, banalmente reversibile, e rappresenta in modo diretto il **compromesso tra fluidità grafica e determinismo real-time** che è il tema centrale di tutto questo lavoro (vedi TEST 5b).

**Costo:** il pan appare visibilmente meno fluido durante il trascinamento. È una scelta di prodotto, non tecnica: va valutata con chi definisce l'esperienza d'uso della macchina.

**Protocollo di validazione:** stesso martellamento del grafico 2D, stessa durata (~13 min), stesso throttling, confrontando con i valori di questa tabella. Attenzione al limite statistico: 9 sforamenti sono pochi, un dimezzamento atteso porterebbe a ~4-5, differenza al confine del rumore — meglio una sessione di ~30-40 min per parte.

##### ✅ Modifica applicata (2026-07-30): coalescing pan 16 → 33 ms

Portato `kPanMinRedrawIntervalMs` da **16 ms (~60 fps)** a **33 ms (~30 fps)** in **entrambi** i punti dove la costante è duplicata:

| File | Riga | Governa |
|------|------|---------|
| `sim2d/Sim2DView.cpp` | **1563** | pagina **Calculation** (`CSim2DView`) e, per derivazione, `CAutGView` (AUTO/SAUTO) e Sequenza Manuale |
| `cad2d/Ppgviews.cpp` | **875** | viste **CAD / Die Set** (`CPPGBaseView`, base di `MatView`/`Pezzoview`/`Punzview`) |

Entrambe modificate perché "grafico 2D" può riferirsi a una qualunque di queste schermate: cambiarne una sola renderebbe il test non interpretabile. **Per tornare indietro: rimettere 16 in entrambi i file.**

⚠️ **Nota di sicurezza:** `CSim2DView::DrawPanIfDue` è ereditata anche da `CAutGView`, usata nelle pagine **AUTO/SAUTO** durante il ciclo di piegatura reale. La modifica riguarda solo la **frequenza di ridisegno durante il trascinamento manuale** del grafico: non tocca la logica di ciclo, il posizionamento, né i side-effect presenti in `DrawDisView` per quei rami. Il rischio funzionale è quindi basso, ma in un test di produzione va verificato che il grafico segua correttamente il cambio passo in automatico.

**Atteso:** dimezzamento dei frame rasterizzati durante il pan ⇒ riduzione proporzionale del lavoro CPU e della banda contesa col thread RT. **Costo:** pan visibilmente meno fluido (30 fps invece di 60) — scelta di prodotto, da validare con chi definisce l'esperienza d'uso.

#### 🔴 NUOVO HOTSPOT: pagina "Manual Sequence" — logica di calcolo dentro la routine di disegno (2026-07-30)

Sessione: **246 000 attivazioni** (984 s ≈ **16 min 24 s**), path DRM, fix PegGL, coalescing 33 ms, **throttling 6000/40000**. Attività: uso della pagina **Manual Sequence** (`TYPE_SEQ_MAN_VIEW`, `CSeqManualeFrame`) — simulazione della sequenza di piegatura, avanzando tra le pieghe con *Go Down* / *Go Up*.

| Metrica | Valore |
|---------|--------|
| Attivazioni | **246 000** |
| Durata | **16 min 24 s** |
| `nanosleep` **max** | **135 µs** ⚠️ il più alto dai test SDL |
| `nanosleep` min | 11 µs |
| Valori **> 100 µs** | **11** (44,7 /M) |
| 60–70 / 71–80 / 81–90 / 91–99 µs | 338 / 40 / 21 / 10 |
| max ultime 6 000 att. | 62 µs |

**Profilo anomalo e diagnostico** — confronto con le altre sessioni a pari throttling:

| | Grafico 2D (191 k) | Uso vario (225 k) | 3D + fix (77 k) | **Manual Sequence (246 k)** |
|---|---|---|---|---|
| totale eventi > 60 µs | 4 565 /M | 3 471 /M | 4 792 /M | **1 707 /M** ← il più basso |
| **> 100 µs** | 47,1 /M | 4,4 /M | 0 /M | **44,7 /M** |
| **max** | 115 µs | 103 µs | 100 µs | **135 µs** ← il più alto |

È l'unica sessione con questo profilo: **pochissimi eventi elevati in totale, ma il picco più alto di tutti**. Non un carico costante, ma **operazioni occasionali e molto costose**.

**Causa individuata nel codice** — `sim2d/Sim2DView.cpp:326-347`, dentro `DrawDisView()`:

```cpp
if((Type()==TYPE_SEQ_MAN_VIEW) && ((CSeqManualeFrame*)m_pFrame)->GetStatoSeq()!=SIMULA)
{
    CSim2DFormSqManPieghe* pSqManVw = (...)->GetForm2();
    int nNumPiegaSel = pSqManVw->GetNumPiegaSel();
    ((CSeqManualeFrame*)m_pFrame)->Posiziona(nNumPiegaSel);   // riga 330 — ricalcolo geometrico
    ((CSeqManualeFrame*)m_pFrame)->ChiudiPiega();             // riga 331 — ricalcolo geometrico
    DisegnaSup/Punz/Inf/Mat/Riscontro/Pezzo(...);             // righe 333-338
    DrawCollisioni(...);                                     // riga 339
    if(!m_bBloccaAggFocusLista)
        pSeqMan->SettaFocusSuSequenza();                     // riga 345 — cambio focus tastiera
}
```

**Su questa pagina la routine di disegno non disegna soltanto: ricalcola la geometria della piega.** `Posiziona()` e `ChiudiPiega()` non hanno nulla a che vedere col rendering — calcolano lo stato del pezzo alla piega selezionata. `SettaFocusSuSequenza()` sposta perfino il focus della tastiera, dentro una funzione di paint. È logica applicativa nel percorso di disegno: un difetto di separazione delle responsabilità che si paga in jitter.

**Perché il coalescing a 33 ms non aiuta qui.** Quel freno agisce sul **pan col dito** (`DrawPanIfDue`, chiamato da `OnMouseMove`). Su questa pagina il redraw è scatenato dalla pressione di *Go Down* / *Go Up*, e ogni pressione ha legittimamente diritto a un redraw. Il problema non è la **frequenza** dei redraw ma il **costo di ciascuno**.

**Direzione di intervento (lavoro futuro, NON eseguito):** spostare `Posiziona`/`ChiudiPiega` **fuori** da `DrawDisView`, nell'handler che cambia la piega selezionata (*Go Up*/*Go Down*/selezione in lista), memorizzando la geometria risultante; il disegno dovrebbe limitarsi a disegnare. Analogamente `SettaFocusSuSequenza` non appartiene a una routine di paint. Beneficio atteso: il ricalcolo passerebbe da "una volta per repaint" a "una volta per azione utente" — e ogni repaint causato da altro (dialog che si chiude, invalidate parziale, timer) non lo pagherebbe più affatto.

⚠️ **Rischio ALTO, motivo per cui non è stato toccato.** `Posiziona` agisce sul posizionamento della piega in una pagina che governa la **sequenza reale di piegatura**. Prima di intervenire va accertato se scriva anche quote assi verso la macchina (dal nome non è escludibile): in quel caso spostare la chiamata cambierebbe il *quando* di un'azione sulla macchina, non solo di un calcolo. Richiede validazione su macchina reale, impossibile nel tempo residuo del tirocinio.

##### 📊 MISURA (2026-07-30): `Posiziona` è il 59% del costo di ogni redraw, e un draw costa **millisecondi**

Strumentazione temporanea in `DrawDisView` (statistiche accumulate, una riga ogni 10 redraw). Prima misura con `CLOCK_MONOTONIC` (tempo trascorso), poi corretta a `CLOCK_THREAD_CPUTIME_ID` — vedi caveat sotto.

**Costo totale del draw** (tutti i rami, 110 campioni): `med = 1 514 µs`, `max = 3 352 µs`.

**Breakdown sul ramo stepping** (SEQ_MAN, `statoSeq != SIMULA`, 10 campioni; somma ≈ 2 611 µs):

| Voce | Media | Max | Quota |
|------|-------|-----|-------|
| **`Posiziona(nNumPiegaSel)`** | **1 528 µs** | 1 761 µs | **58,5%** |
| `Disegni` (7 poligoni + collisioni) | 945 µs | 1 218 µs | 36,2% |
| `SettaFocusSuSequenza()` | ~96 µs | — | 3,7% |
| `ChiudiPiega()` | 42 µs | 49 µs | 1,6% |

**Conclusioni.**

1. **L'ipotesi è confermata con un numero**: il ricalcolo geometrico dentro la routine di disegno è la voce **dominante** — `Posiziona` da sola costa più di tutto il rendering. Il refactoring proposto (spostarla nell'handler che cambia piega) eliminerebbe ~59% del costo di ogni redraw di questa pagina, e il 100% di quello dei repaint causati da altro (dialog, invalidate, timer).
2. **La scala è il dato più importante di tutta la campagna**: un singolo `DrawDisView` costa **1,5 ms in media e 3,35 ms nel caso peggiore**, su un sistema il cui ciclo RT è di **4 ms**. Un disegno occupa quindi il **35-85% di un periodo real-time**. A questa luce non sorprende vedere picchi di 100-135 µs: sorprende che non siano peggiori. È anche la spiegazione retroattiva del perché ogni intervento sulla *frequenza* dei redraw (defer CH0, coalescing 16→33 ms) abbia funzionato così bene — si dimezzavano frame da un millisecondo, non da 50 µs.
3. I soli **`Disegni` costano ~945 µs**: è il costo intrinseco di rasterizzare in software 7 poligoni pieni a **1024×600** (risoluzione reale del target, confermata dal boot log: `mode=1024x600@64Hz`). Vale anche per il grafico 2D della pagina Calculation, e giustifica a posteriori il coalescing a 33 ms.
4. **`SettaFocusSuSequenza` costa ~96 µs** dentro una funzione di paint: da sola è dell'ordine dell'intero budget di jitter perseguito. Non appartiene a un percorso di disegno.

⚠️ **Caveat sulla prima misura.** I valori sopra sono stati raccolti con `CLOCK_MONOTONIC`, che misura **tempo trascorso**, non tempo di CPU. Con il throttling cgroup attivo il thread GUI viene descheduled continuamente, quindi i valori sono **gonfiati** di un fattore dipendente dal throttling. La strumentazione è stata poi corretta a **`CLOCK_THREAD_CPUTIME_ID`** (tempo di CPU effettivo del thread, immune alla deschedulazione): i valori assoluti attesi sono più bassi, ma le **proporzioni** tra le quattro voci restano valide, essendo misurate tutte allo stesso modo. Da rimisurare con la versione corretta.

##### 🔬 Analisi: il throttling cgroup migliora la media, non il caso peggiore

Osservazione emersa discutendo il caveat sopra, e coerente con **tutti** i dati raccolti.

`cpu.max = 6000 40000` non significa "la GUI gira al 15% di velocità". Significa: in ogni **periodo di 40 ms** il gruppo può consumare **6 ms di CPU**, e li consuma **a piena velocità** — parte al 100%, esaurisce la quota, poi viene **congelato** per i restanti ~34 ms. Il throttling quindi **concentra** il carico in raffiche invece di distribuirlo: abbassa la *media*, non l'*intensità di picco*.

**I dati lo confermano.** Serie dei massimi nelle sessioni con throttling: **113, 115, 135 µs**. Il caso peggiore non è mai migliorato grazie al throttling; sono migliorati i totali degli eventi elevati, cioè la media — esattamente ciò che il modello prevede.

**Sproporzione di granularità:** periodo cgroup **40 ms** contro ciclo RT **4 ms**. Il freno lavora su una scala **dieci volte più grossa** del fenomeno da proteggere: un draw da 1,5 ms può cadere interamente dentro una finestra di quota senza mai essere interrotto.

**➡️ Esperimento a costo zero (nessuna ricompilazione):** stessa percentuale di CPU, periodo allineato al ciclo RT.

```bash
# 15% con periodo 4 ms: la GUI non puo' mai girare piu' di 600 us consecutivi
echo "600 4000" > /sys/fs/cgroup/<gruppo>/cpu.max
# via di mezzo: 15% con periodo 10 ms
echo "1500 10000" > /sys/fs/cgroup/<gruppo>/cpu.max
```

Così la lunghezza della raffica è **limitata per costruzione**, e l'intervento agisce sul **caso peggiore** anziché sulla media. Sarebbe una leva di **configurazione**, non di codice — applicabile senza toccare l'applicazione.

**Prezzo:** interfaccia più a scatti (un draw da 1,5 ms viene spezzato in più tronconi) e più eventi di throttle/unthrottle, che hanno un costo proprio.

⚠️ **Vincolo del kernel:** `cpu.max` non accetta quota o periodo **inferiori a 1 ms** (1000 µs) — `min_cfs_quota_period`, serve a evitare starvation da arretrato cronico. Il tentativo `600 4000` restituisce `write error: Invalid argument`. Il 15% più fine ottenibile è quindi **`1000 6666`** (raffica max 1 ms, periodo 6,7 ms); alternativa **`1500 10000`** (raffica 1,5 ms, periodo 10 ms).

##### ❌ ESITO: il throttling a periodo fine NON migliora il caso peggiore (2026-07-30)

Provata la configurazione `1000 6666` (~15%, raffica limitata a 1 ms invece di 6 ms).

| | Manual Sequence, throttling `6000 40000` | Throttling fine `1000 6666` |
|---|---|---|
| Attivazioni | 246 000 (16 min) | **16 000 (1 min)** |
| **max** | 135 µs | **126 µs** |
| > 100 µs | 11 (44,7 /M) | 2 (125 /M) |
| totale > 60 µs | 1 707 /M | 1 875 /M |

⚠️ **Il campione è troppo piccolo** (1 minuto) perché i conteggi abbiano significato: 2 eventi hanno un'incertezza dello stesso ordine del valore. Ma il **massimo** è osservabile anche in sessioni brevi, e **non è sceso**: 126 contro 135 µs, differenza nel rumore.

**Inferenza — e spiega in modo unitario tutti i dati raccolti.** Limitando le raffiche da 6 ms a 1 ms il caso peggiore non è migliorato. Se il meccanismo fosse "la GUI monopolizza la CPU troppo a lungo", accorciare le raffiche avrebbe dovuto agire. Non agendo, il sospetto si sposta su un'interferenza **istantanea e non cumulativa**: la contesa sulla **banda di memoria**. Un millisecondo di rasterizzazione di poligoni a 1024×600 satura la DDR esattamente quanto sei millisecondi; accorciare la raffica riduce la **probabilità** di sovrapposizione col ciclo RT, non la **gravità** quando la sovrapposizione avviene. E il massimo misura la gravità.

⚠️ **RETTIFICA (2026-07-30) — questa generalizzazione era TROPPO AMPIA.** Era stato scritto che il throttling cgroup "non ha **mai** migliorato il massimo". **Falso in questa forma:** la campagna del 2026-07-20 ([tabella confronti](#confronto-branch-test6-font-2026-07-27)) mostra che nello scenario **scroll grafico** stringere la quota migliorava eccome il caso peggiore: **99 → 93 → 91 → 71 µs** passando da no-throttle a 50%/25%/10%.

**Formulazione corretta: l'effetto dipende dallo scenario.**

| Scenario | Natura dell'interferenza | Il throttling aiuta sul massimo? |
|----------|--------------------------|----------------------------------|
| **Scroll grafico** (campagna 2026-07-20) | continua, molti frame piccoli in sequenza | ✅ **sì**, 99 → 71 µs |
| **Manual Sequence / 3D** (sessioni 2026-07-30) | **singole operazioni pesanti** da 1-2 ms di CPU | ❌ **no**, max invariato 113-135 µs |

La spiegazione è coerente: il throttling limita **quanta CPU** la GUI ottiene per unità di tempo, quindi comprime un flusso continuo di lavoro. Ma non può accorciare una **singola operazione** che deve comunque essere eseguita per intero: un `DrawDisView` da 1,1-2,2 ms di CPU resta tale, e viene semplicemente spezzato in tronconi che si spalmano su più periodi.

##### 📉 Banda DDR durante il martellamento: 2,3–2,6% — la più bassa mai misurata

Misura `perf` (`imx8mp_bandwidth_usage.lpddr4`, campioni ~1 s) durante martellamento di *Continue* sulla pagina Manual Sequence, con throttling `1000 6666`:

- `axid-write` ≈ 103–119 M/s, `axid-read` ≈ 269–299 M/s
- **2,3–2,6 %** ⇒ con la formula del registro (`%/100 × 16000`) ≈ **368–416 MB/s**

**Confronto con la tabella storica** (stessa metrica):

| Configurazione | Banda DDR | nanosleep max |
|----------------|-----------|---------------|
| 0-SDL, no throttle | 7,6–13,1 % | 130 µs |
| 6-base DRM, no throttle | 4,0–4,2 % | 99 µs |
| 6-10% (`2000 20000`) | 2,5–3,4 % | 71 µs |
| **Manual Sequence, `1000 6666`** | **2,3–2,6 %** | **126 µs** |

⚠️ **Questo indebolisce l'ipotesi "banda di memoria" nella sua forma semplice**: la configurazione attuale usa **la banda più bassa di tutte** e ha comunque il jitter peggiore tra quelle DRM. La **banda media non è il fattore limitante**.

**Ma il dato non è conclusivo**, perché la granularità è sbagliata: sono medie su finestre di **1 secondo**, e una raffica da 1 ms al 50% di banda vi si annullerebbe completamente. La misura non vede il fenomeno che dovrebbe smentire o confermare.

**➡️ Verifica a 10 ms — ESEGUITA (2026-07-30):**

```bash
perf stat -a -I 10 -M imx8mp_bandwidth_usage.lpddr4
```

| Granularità | Banda DDR osservata | MB/s ≈ |
|-------------|---------------------|--------|
| 1 s (media) | 2,3–2,6 % | 368–416 |
| **10 ms (picchi)** | **4,7–6,9 %** | **752–1 104** |

**Il traffico È a raffiche**: rapporto picco/media ≈ **2,7×**. La misura a 1 secondo effettivamente nascondeva il fenomeno.

**Ma i picchi restano bassi in assoluto:** 6,9 % ≈ 1,1 GB/s su 16 GB/s disponibili. Per confronto, il path **SDL** consumava 7,6–13,1 % come **media su un secondo**, cioè più di quanto la configurazione attuale raggiunga nei *picchi* a 10 ms.

⇒ **La contesa di banda, da sola, non spiega ritardi di 126 µs.** A quel livello di utilizzo il controller DDR è lontano dalla saturazione, e la latenza aggiuntiva che introduce è nell'ordine delle decine di nanosecondi per accesso, non delle centinaia di microsecondi.

⚠️ **Limite dello strumento:** 10 ms sono comunque **2,5×** il ciclo RT (4 ms), quindi una raffica da 1 ms viene diluita ~10× dentro la finestra.

**➡️ Spinta a `-I 1` — ESEGUITA (2026-07-30), con cautela sui risultati:**

```bash
perf stat -a -I 1 -M imx8mp_bandwidth_usage.lpddr4
```

Campioni durante martellamento di *Continue*. La metrica ha riportato **15,3 %**, **36,4 %**, **7,8 %** su intervalli consecutivi.

⚠️ **La percentuale riportata NON è affidabile a questa granularità.** Ricalcolando dai conteggi grezzi:

| Campione | Eventi (write+read) | `duration_time` | Eventi/s | % ricalcolata | MB/s ≈ |
|----------|--------------------:|----------------:|---------:|--------------:|-------:|
| "36,4 %" | 5 827 648 | 2,92 ms | 1,996 G/s | **~12,4 %** | **~1 990** |
| "15,3 %" | 2 453 380 | 1,57 ms | 1,565 G/s | **~9,7 %** | **~1 560** |

Calibrazione usata: dal campione a 1 s, 386,1 M eventi/s ↔ 2,4 % ⇒ **1 % ≈ 161 M eventi/s ≈ 160 MB/s**.

**Perché la metrica sbaglia qui:** i `duration_time` mostrano che gli intervalli reali non sono da 1 ms ma da **1,5–2,9 ms**, irregolari — `perf stat -I 1` è oltre i suoi limiti pratici. In queste condizioni possono intervenire multiplexing dei contatori e disallineamento tra la finestra dei contatori e quella della metrica, con fattori di scala errati. **Fare riferimento ai conteggi grezzi, non alla percentuale.**

**Quadro completo della burstiness:**

| Granularità | Banda DDR | MB/s ≈ |
|-------------|-----------|--------|
| 1 s (media) | 2,4 % | 384 |
| 10 ms (picchi) | 5–7 % | 800–1 100 |
| **~2-3 ms (picchi, ricalcolati)** | **~10–12 %** | **~1 600–2 000** |

⇒ Rapporto picco/media ≈ **5×**. I picchi raggiungono **~2 GB/s**, cioè un livello **paragonabile alla media consumata dal path SDL** (7,6–13 % su finestre di 1 s).

**Conclusione intermedia sulla banda:** la contesa **è reale a scala millisecondo** — le medie a 1 secondo la nascondevano. Ma 2 GB/s restano il ~12 % di un bus da 16 GB/s: lontano dalla saturazione.

##### 🎯 CONTROLLO SPERIMENTALE (2026-07-30): la banda NON è il meccanismo — i picchi ci sono anche a riposo

Misura decisiva: **stessa metrica, stesso comando, confrontando carico e riposo**.

```bash
perf stat -a -I 10 -e imx8_ddr0/axid-read/,imx8_ddr0/axid-write/ sleep 30
```

Conteggi convertiti con la calibrazione del registro (**1 % ≈ 161 M eventi/s ≈ 160 MB/s**), intervalli reali ≈ 10,4 ms:

| Condizione | Intervalli tipici | **Picchi osservati** |
|------------|-------------------|----------------------|
| **A riposo** (nessuna interazione) | 0,7–1,0 % → 110–165 MB/s | **5,0–7,8 % → 810–1 250 MB/s** |
| **Premendo *Continue*** | alternanza 0,9 % ↔ 4–5 % | **6,5–9,1 % → 1 040–1 460 MB/s** |

**I picchi sono quasi identici nelle due condizioni**: massimo 7,8 % a riposo contro 9,1 % sotto carico, appena **+17 %**. Esempio dai dati a riposo: `9 703 976` read + `5 278 516` write in un singolo intervallo ⇒ **1,25 GB/s**, con nessuna interazione in corso.

**Cosa cambia sotto carico non è l'altezza delle raffiche, ma la loro frequenza** (il *duty cycle*): a riposo lunghe sequenze attorno all'1 % con burst sporadici; premendo, gli intervalli al 4-5 % diventano continui.

> **⇒ Conclusione: il picco di banda DDR NON è la causa del jitter.** Se il picco è praticamente lo stesso a riposo e sotto carico, ma il jitter peggiora **solo** sotto carico, allora quella variabile **non discrimina** tra le due condizioni e non può esserne la causa. Non è un'argomentazione teorica: è un controllo sperimentale con condizione di riferimento.

**Restano quindi in gioco i soli candidati che scalano con l'attività della GUI**, non con la banda:

1. **contesa su lock nel kernel** lungo i percorsi esercitati dal disegno
2. **page fault** (nel percorso GUI, con effetti cross-CPU) — meccanismo della stessa famiglia di quello trovato e corretto in `PegGL`
3. **IPI / TLB shootdown**
4. **costo diretto del lavoro CPU**: cicli sottratti e, soprattutto, **inquinamento delle cache** — un draw da 1,1 ms che tocca oltre un megabyte di pixel evince la cache di ogni altro thread, incluso quello RT, che poi paga i miss al proprio risveglio

Il punto 4 è ora il più plausibile: spiega perché il jitter scali con l'**attività** di disegno (che è ciò che l'esperimento mostra) e non con il **picco di banda** (che non discrimina).

**Osservazione secondaria, non indagata:** anche a riposo le raffiche ricorrono con periodicità regolare, e ogni ~550 ms compaiono in gruppetti. Origine ignota (candidati: refresh periodici di sistema, timer del framework, attività del display controller). Filo potenzialmente interessante per chi continua, ma **non correlato al jitter sotto carico**, che è il fenomeno in esame.

##### 🧭 Stato finale della questione "meccanismo del jitter residuo"

**Accertato:**

- il **lavoro** che disturba è identificato e quantificato in tempo di CPU (draw da 1,1-2,2 ms, di cui `Posiziona` ~50% sulla pagina Manual Sequence)
- l'efficacia del **throttling dipende dallo scenario** (aiuta sul flusso continuo dello scroll, non sulle singole operazioni pesanti)
- la **banda DDR è fortemente a raffiche** (media 2,4 % su 1 s, picchi 5-9 % a 10 ms), **ma NON è la causa del jitter**: controllo sperimentale con condizione di riposo → i picchi sono quasi identici con e senza interazione (7,8 % vs 9,1 %), quindi la variabile **non discrimina** tra le due condizioni. Sotto carico cambia il *duty cycle*, non l'altezza delle raffiche

**NON accertato — il meccanismo fisico con cui quel lavoro ritarda il thread RT.** Candidati residui, in ordine di plausibilità:

1. **contesa su lock nel kernel** (es. lungo il percorso di `mmap`/page fault, o driver)
2. **page fault** nel percorso RT o nel percorso GUI con effetti cross-CPU
3. **IPI / TLB shootdown** — meccanismo già trovato e corretto in `PegGL` (riallocazione per frame), potrebbero esistere altre sorgenti analoghe
4. **raffiche DDR su scala < 1 ms**, non risolvibili con `perf stat`
5. ⚠️ **gli eventi di throttle/unthrottle del cgroup stesso**: con periodo 6,7 ms sono ~150 al secondo, ciascuno con lavoro cross-CPU. È possibile che il periodo fine abbia **aggiunto** una sorgente di disturbo mentre ne toglieva un'altra — il che spiegherebbe perché il massimo sia rimasto invariato (126 vs 135 µs) nonostante le raffiche fossero limitate a 1 ms.

**Come procederebbe chi continua:** correlazione temporale diretta tra gli spike di `nanosleep` e ciò che accade nel sistema in quell'istante. → **FATTO, vedi sotto: la questione è stata chiusa con i contatori PMU.**

---

<a id="meccanismo-jitter-risolto-2026-07-30"></a>

## ⚠️ MECCANISMO DEL JITTER — indagato con i contatori PMU, NON risolto (2026-07-30)

> **RETTIFICA IMPORTANTE (2026-07-30, stessa giornata).** Questa sezione era stata scritta col titolo "RISOLTO" e concludeva che il meccanismo fosse una **contesa di latenza sul sistema di memoria** (`+57 %` di costo per transazione). **Quella conclusione è stata ritirata** dopo l'analisi statistica su 10 000 iterazioni. Due errori:
>
> 1. **Lettura errata di `BUS_CYCLES`.** Su Cortex-A53 questo evento conta i **cicli del clock di bus**, cioè è un proxy del *tempo trascorso*, non dell'occupazione del bus. Quindi `bus_cycles/bus_access` **non è** il costo per accesso, ma il tempo *tra* accessi — l'inverso della densità di traffico. Il "+57 % per transazione" non ha fondamento: quel valore più alto sotto carico indica accessi **più radi**, non più cari.
> 2. **Conclusione tratta da due campioni singoli.** La coincidenza numerica (75 180 cicli in più ≈ 47 µs ≈ i 50 µs di differenza di ritardo) reggeva sui due worst case, ma **non è confermata dalla distribuzione** (vedi analisi sotto).
>
> Il contenuto originale è mantenuto qui sotto perché la parte descrittiva (strumento, misure, dati grezzi) resta valida; le conclusioni sono corrette in fondo alla sezione.

### Strumento: `PerfMonitor` (SqCom), riattivato

Il modulo `SqCom_Library/SqCom/PerfMonitor.cpp` (+ `.h`, replicato negli `include/sqcom/`) usa `perf_event_open` per leggere i contatori PMU **dentro la finestra della singola `clock_nanosleep`** del thread RT, e mantiene il **worst case globale**. Risponde quindi direttamente alla domanda: *quando la nanosleep sfora, cosa stava succedendo dentro quella specifica iterazione?*

**Stato trovato:** i punti di misura erano **già cablati** in `SqCom_Library/SqCom/RTCHndlr.cpp` attorno alla `clock_nanosleep` (`PerfMonitor_Start()` / `PerfMonitor_StopAndSaveNextAndWorst(diff_us_nanosleep)`), ma **inattivi**, perché gated su `PerfMonitor_IsEnabled()` e la `PerfMonitor_Init()` non veniva mai chiamata: non era presente né in `PlcEsa/Lnk/main.cpp` né altrove (probabilmente persa insieme ad altro lavoro non committato).

**Modifiche fatte per riattivarlo:**

| File | Modifica |
|------|----------|
| `SqCom_Library/SqCom/RTCHndlr.cpp` | `PerfMonitor_Init({3}, 10000, "nanosleep_delay_us")` nel blocco `one_time`; CPU **3** = core del thread RT (la GUI è confinata su 0-2 dal cgroup) |
| `SqCom_Library/SqCom/RTCHndlr.cpp` | **warm-up**: `do_perf = PerfMonitor_IsEnabled() && (global_count > perfWarmupIter)`, default 15000 iterazioni (~60 s), override runtime con `PERF_WARMUP_ITER` |
| `SqCom_Library/SqCom/RTCHndlr.cpp` | `PerfMonitor_PrintWorstStats()` ogni 50000 attivazioni |
| `PlcEsa/Lnk/main.cpp` | alla terminazione (dopo il loop, nel thread principale): `PrintFinalStats` + `PrintWorstStats` + `SaveCsv("/tmp/perf_rt.csv")` + `SaveWorstCsv` + `Close` |

⚠️ **Il warm-up è indispensabile:** senza, sia il worst globale sia il buffer delle prime 10000 iterazioni descrivono il **transitorio di avvio** (i primi 40 s), che questo registro documenta già come sorgente di spike non rappresentativi. I primi campioni raccolti (worst alle iterazioni **383** e **5388**, cioè 1,5 s e 21 s dopo l'avvio) erano infatti artefatti di startup e sono stati scartati.

⚠️ **La misura perturba il misurato:** leggere i contatori a ogni iterazione aggiunge lavoro nel percorso RT. I valori assoluti di `nanosleep` con questa build **non sono confrontabili** con le altre sessioni del registro. Serve per **correlazione**, non per misurare il jitter.

### Il confronto decisivo

Due worst case, **stesso build, stesso warm-up**, uno a riposo e uno usando il simulatore di piegatura (pagina Manual Sequence):

| Metrica | **A riposo** | **Sotto carico** |
|---------|--------------|------------------|
| `nanosleep_delay_us` | **59 µs** | **109 µs** |
| **istruzioni** | **87 894** | **87 842** |
| `cpu_cycles` | 324 356 | **399 536** |
| **CPI** | 3,69 | **4,55** |
| `l2d_cache_refill` | 1 769 | **1 380** |
| `l2d_cache` | 7 543 | 7 516 |
| L2 miss % | 23,45 % | 18,36 % |
| `bus_access` | 7 085 | **5 553** |
| `bus_cycles` | 164 336 | 201 908 |
| **`bus_cycles`/`bus_access`** | **23,2** | **36,4** |

**Le istruzioni sono praticamente identiche (87 894 vs 87 842)**: è lo stesso percorso di codice al risveglio, eseguito lo stesso numero di volte. Il confronto è quindi pulito — ogni differenza riguarda **quanto ci mette**, non **cosa fa**.

### Il conto che chiude la questione

```
Cicli in più sotto carico:  399 536 − 324 356 = 75 180 cicli
A ~1,6 GHz:                 75 180 / 1,6e9    ≈ 47 µs
Ritardo in più misurato:    109 − 59          =  50 µs
```

**Coincidono entro il 6 %.** Il ritardo aggiuntivo **è** il rallentamento nell'esecuzione del percorso di risveglio. Non è latenza di scheduling, non è un interrupt, non è uscita da uno stato di idle profondo: il core esegue le stesse ~88 000 istruzioni e impiega ~47 µs in più a farlo.

### Il meccanismo: latenza per accesso, non numero di miss

Sotto carico il thread RT fa **meno** miss L2 (1 380 vs 1 769) e **meno** accessi al bus (5 553 vs 7 085), eppure consuma **più** cicli. Quindi **la GUI non gli causa più cache miss.**

Ciò che cambia è il **costo di ogni singolo accesso**: `bus_cycles/bus_access` passa da **23,2 a 36,4**, cioè **+57 % di tempo per transazione**. Il core completa meno accessi proprio perché resta bloccato in attesa di quelli già in volo.

> **Meccanismo accertato:** la GUI non fa mancare la cache al thread real-time — gli fa **pagare di più ogni accesso alla memoria**, perché interconnessione e DRAM sono occupate molto più spesso. È contesa sul sistema di memoria a livello di **latenza**, non di **banda**.

### Perché questo riconcilia tutte le osservazioni precedenti

| Osservazione | Spiegazione nel modello finale |
|--------------|-------------------------------|
| I **picchi** di banda DDR sono simili a riposo e sotto carico (7,8 % vs 9,1 %) | Non serve saturare il bus: basta che sia occupato *più spesso* |
| Ma il **duty cycle** sotto carico è molto più alto | ⇒ cresce la probabilità che un accesso del thread RT trovi la coda occupata |
| Il **numero** di cache miss non correla col ritardo | Corretto: non è quello il meccanismo |
| Il **CPI** correla col ritardo | È la misura diretta dello stallo in attesa della memoria |
| Il **throttling** migliora le medie ma non il caso peggiore | Riduce quanto spesso la GUI lavora, non quanto costa un accesso quando la collisione avviene |
| Le ottimizzazioni efficaci riducono i **pixel toccati** | Meno traffico ⇒ minore occupazione del sistema di memoria ⇒ minore probabilità di collisione |
| Isolare i core (`cpuset 0-2`) non ha protetto il thread RT | I core sono separati, ma **interconnessione, L2 e DRAM sono condivise** |

### Conseguenze operative

1. **Non esiste una manopola di scheduling che risolva.** Il problema non è quanta CPU ottiene la GUI, ma quanto occupa il sistema di memoria mentre lavora. Confermato dal fallimento del throttling a periodo fine.
2. **L'unica leva efficace è ridurre il traffico di memoria della GUI**, che è esattamente ciò che hanno fatto tutti gli interventi riusciti: path DRM (niente upload texture né conversione), coalescing del pan (meno frame), fix `PegGL` (niente riallocazione per frame).
3. **Priorità per il lavoro futuro**, in coerenza con il meccanismo: eliminare le **3 copie ridondanti** del percorso 3D, togliere `Posiziona` (~50 % del costo) dal percorso di disegno della pagina Manual Sequence, valutare la scrittura diretta del rasterizzatore nel framebuffer PegLib.
4. **Su questo SoC non è possibile partizionare la cache o la banda** (Cortex-A53, niente MPAM): non esiste una soluzione di configurazione, solo la riduzione del lavoro.

### ❌ VERIFICA STATISTICA su 10 000 iterazioni: il modello NON regge

Analisi del CSV completo (`perf_rt.csv`, **sessione interamente a riposo**, 10 000 iterazioni, warm-up attivo).

**Correlazioni ritardo ↔ contatori:**

| Coppia | r | r² (varianza spiegata) |
|--------|---|------------------------|
| ritardo ↔ **CPI** | **+0,368** | 14 % |
| ritardo ↔ refill L2 | +0,327 | 11 % |
| ritardo ↔ `bus_cycles/bus_access` | **−0,416** | 17 % |
| ritardo ↔ `cpu_cycles` | +0,210 | 4 % |

Su 10 000 punti sono statisticamente significative, ma **nessun contatore spiega più del 17 % della varianza del ritardo**.

**Medie per fascia di ritardo:**

| Fascia | n | ritardo | CPI | refill | bus_c/acc | cpu_cycles | istruzioni |
|--------|---|---------|-----|--------|-----------|------------|------------|
| < 25 µs | 8 710 | 17,6 | 2,66 | 2 562 | 37,2 | **670 377** | 269 505 |
| 25–34 | 865 | 29,6 | 3,43 | 3 863 | 26,5 | 816 934 | 270 041 |
| 35–44 | 277 | 37,7 | 3,53 | 3 881 | 26,0 | 810 363 | 266 803 |
| 45–54 | 107 | 50,1 | 3,31 | 3 931 | 26,8 | 842 107 | 283 494 |
| **≥ 55** | 41 | 57,0 | 3,30 | 3 142 | 26,8 | **670 062** | 232 234 |

**Tre osservazioni che smontano il modello:**

1. **Il CPI sale e poi si appiattisce.** Da 2,66 (fascia più veloce) a ~3,4, ma poi resta fermo (3,43 → 3,53 → 3,31 → 3,30) mentre il ritardo **raddoppia** da 30 a 57 µs. Il CPI distingue le iterazioni veloci dalle lente, ma non spiega *quanto* sono lente.
2. **I cicli non crescono col ritardo.** La fascia più lenta (≥ 55 µs) esegue **670 062** cicli, praticamente identici ai **670 377** della fascia più veloce (< 25 µs). Il modello "il ritardo è tempo di esecuzione in più" **non regge sulla distribuzione**, pur avendo funzionato sui due worst case.
3. **`bus_cycles/bus_access` correla in senso opposto** a quanto ipotizzato (r negativo): ritardi lunghi ↔ accessi **più densi**, non più costosi — coerente con la lettura corretta dell'evento come proxy temporale.

### Stato effettivo della questione

**Accertato:**

- lo strumento funziona e produce dati per-iterazione affidabili (con warm-up, per escludere lo startup)
- il **CPI discrimina** le iterazioni veloci da quelle lente (2,66 → ~3,4), quindi una componente di stallo in attesa della memoria **c'è**
- il numero di cache miss **non** spiega il ritardo (confermato sia sui worst sia sulla distribuzione)

**NON accertato — e questa resta la domanda aperta del lavoro:**

- **cosa determini il ritardo oltre i ~25 µs.** Superata quella soglia, i contatori PMU osservati (cicli, istruzioni, miss L2, accessi bus) sono sostanzialmente piatti mentre il ritardo raddoppia. La causa è quindi **fuori** da ciò che questi contatori misurano: candidati residui sono la latenza di uscita dagli stati di idle (`cpuidle`), la latenza di scheduling/wakeup, il traffico di interrupt, o attività di altri master sul bus (display controller, DMA) non visibile dai contatori del core.

**Il confronto che manca, ed è quello decisivo:** questo dataset è **interamente a riposo**. Serve lo stesso CSV da una sessione **sotto carico GUI**, per confrontare le due **distribuzioni** (non due worst case): a parità di ritardo i contatori differiscono? E di quanto si sposta la distribuzione dei ritardi? Il path di salvataggio è fisso, quindi va copiato prima di sovrascriverlo:

```bash
cp /tmp/perf_rt.csv /tmp/perf_rt_carico.csv
```

### ✅ CONFRONTO RIPOSO vs CARICO su distribuzioni complete (2026-07-30) — risultato principale

Secondo dataset da 10 000 iterazioni, stessa build e stesso warm-up, raccolto **martellando *Continue*** sulla pagina Manual Sequence (piegatura in simulazione). Confronto con il dataset a riposo.

**① Cosa cambia: la FREQUENZA delle iterazioni lente**

| Fascia | Riposo (n) | Carico (n) | Variazione |
|--------|-----------:|-----------:|-----------:|
| < 25 µs | 8 710 | 7 908 | −9 % |
| 25–34 µs | 865 | **1 423** | **+65 %** |
| 35–44 µs | 277 | **441** | **+59 %** |
| 45–54 µs | 107 | 133 | +24 % |
| **≥ 55 µs** | 41 | **95** | **+132 %** |
| **totale > 25 µs** | **1 290** | **2 092** | **+62 %** |

**② Cosa NON cambia: i contatori dentro le iterazioni lente**

| Fascia | CPI (riposo → carico) | refill L2 (riposo → carico) | `cpu_cycles` (riposo → carico) |
|--------|----------------------|------------------------------|-------------------------------|
| 25–34 | 3,43 → 3,28 | 3 863 → 3 592 | 816 934 → 784 399 |
| 35–44 | 3,53 → 3,41 | 3 881 → 3 862 | 810 363 → 824 196 |
| 45–54 | 3,31 → 3,46 | 3 931 → 3 474 | 842 107 → 748 993 |
| ≥ 55 | 3,30 → 3,35 | 3 142 → 3 459 | 670 062 → 779 751 |

**A parità di ritardo i contatori sono sostanzialmente identici**, senza differenze sistematiche in alcuna fascia. Anche le correlazioni restano dello stesso ordine (carico: CPI +0,297, refill +0,239, bus_c/acc −0,357, cicli +0,155).

> ### 🎯 Risultato: la GUI non cambia la **natura** di un'iterazione lenta, cambia **quanto spesso** capita.
>
> Il meccanismo che produce un'iterazione lenta è lo stesso a riposo e sotto carico. Il carico grafico ne aumenta la **probabilità** (+62 % di iterazioni sopra i 25 µs, +132 % sopra i 55 µs), non la gravità.

**③ Il ritardo NON è tempo di esecuzione**

Nella fascia ≥ 55 µs i cicli CPU sono **670–780 mila**, gli stessi della fascia < 25 µs (**670–703 mila**), con istruzioni comparabili. Ma il ritardo è **tre volte** tanto.

Se il core esegue lo stesso numero di cicli impiegando ~40 µs in più, **quei 40 µs sono tempo in cui non ha eseguito nulla**. Il ritardo non è esecuzione rallentata: è tempo in cui il thread RT **non stava girando**.

Su ARM il contatore dei cicli non avanza quando il clock è gated (idle profondo). Il sospetto principale si sposta quindi sulla **latenza di uscita dagli stati di idle (`cpuidle`)** o comunque sul percorso di wakeup, **non** sulla contesa del sistema di memoria.

**Perché questo riconcilia le osservazioni "strane" precedenti:**

| Osservazione | Spiegazione |
|--------------|-------------|
| Il throttling migliora le medie ma non il massimo | Riduce la frequenza degli eventi, non la loro gravità — che è fissata dal meccanismo di wakeup |
| I picchi di banda DDR sono simili a riposo e sotto carico | La banda non è il meccanismo |
| Il numero di cache miss non spiega il ritardo | Confermato su entrambe le distribuzioni |
| Isolare i core non ha protetto il thread RT | L'isolamento non incide sulla latenza di wakeup |

### ➡️ Test decisivo, non ancora eseguito (costo: 5 minuti)

```bash
# stato degli idle state sul core RT
cat /sys/devices/system/cpu/cpu3/cpuidle/state*/name
cat /sys/devices/system/cpu/cpu3/cpuidle/state*/latency
cat /sys/devices/system/cpu/cpu3/cpuidle/state*/usage

# disabilita gli stati profondi su CPU3 (state0 = WFI, resta attivo)
for s in /sys/devices/system/cpu/cpu3/cpuidle/state[1-9]; do echo 1 > $s/disable; done
```

Poi ripetere la sessione sotto carico. **Se la coda sopra i 50 µs collassa, il meccanismo è la latenza di uscita da idle** — e la mitigazione è una riga di configurazione (o `cpuidle.off=1` / `idle=poll` sul core RT), non una modifica alla GUI. In tal caso resterebbe comunque valido che ridurre il lavoro della GUI abbassa la *frequenza* degli eventi.

### Nota metodologica — perché la rettifica è il risultato più utile della giornata

La conclusione ritirata era **coerente con tutti i dati disponibili al momento**: due worst case con istruzioni identiche, differenza di cicli che tornava entro il 6 %, e un meccanismo plausibile. È stata smentita solo passando da 2 campioni a 10 000.

È esattamente il rischio contro cui il resto di questo registro mette in guardia (vedi la nota sulla durata minima dei test): **un campione piccolo può essere perfettamente coerente con un'ipotesi sbagliata**. Vale la pena riportarlo in tesi come tale, perché documenta il metodo — e perché una conclusione ritirata a fronte di dati migliori vale più di una conclusione difesa.

---

<a id="ipotesi-finale"></a>

## 🏁 IPOTESI FINALE (2026-07-30)

Sezione conclusiva dell'indagine sul meccanismo del jitter residuo. Raccoglie la catena di eliminazione, ciò che resta accertato, l'ipotesi che sopravvive a tutti i dati e le piste aperte.

### 1. Catena di eliminazione — ogni riga chiusa da una misura, non da un ragionamento

| # | Ipotesi | Misura eseguita | Esito |
|---|---------|-----------------|-------|
| 1 | **Banda DDR** satura sotto carico | `perf stat -I 10` / `-I 1`, confronto riposo vs carico | ❌ picchi quasi identici (7,8 % vs 9,1 %); media sotto carico **2,3-2,6 %**, la più bassa mai misurata |
| 2 | **Cache miss** causate dalla GUI | contatori PMU per singola iterazione RT | ❌ non correlano col ritardo; sotto carico sono **meno** (1 380 vs 1 769) |
| 3 | **Contesa di latenza** sul bus (+57 % per transazione) | analisi statistica su 10 000 iterazioni | ❌ **ipotesi ritirata**: fondata su 2 soli campioni e su una lettura errata di `BUS_CYCLES` (è un contatore di clock, non di occupazione) |
| 4 | **Processi utente concorrenti** su CPU3 | `function_graph` attorno a uno spike | ❌ nella finestra critica ci sono solo i due thread RT del test e il kernel |
| 5 | **Stati di idle profondi** (`cpuidle`) | `latency` e `usage` degli stati | ❌ esistono solo `WFI` (uscita **1 µs**) e `cpu-pd-wait` (1500 µs) usato **573 volte su 9,7 M** |
| 6 | **Ciclo di idle/wakeup** in generale | PM QoS `/dev/cpu_dma_latency` = 0, sessione completa | ❌ distribuzione invariata (iterazioni > 25 µs: 2 092 → 2 040, −2,5 %); worst 108 → 103 µs |
| 7 | **Scaling di frequenza** (DVFS) | `scaling_governor` | ❌ governor `performance`, frequenze disponibili solo 1,2 e 1,6 GHz |
| 8 | **Throttling termico** | trip point e temperatura | ❌ 50 °C contro soglia passiva a **95 °C** — 45 °C di margine |
| 9 | **Bilanciamento del carico** su CPU3 | `/sys/devices/system/cpu/isolated` | ❌ `isolcpus=3` già attivo, CPU3 fuori dai sched domain |

### 2. Cosa resta accertato

1. **La GUI cambia la frequenza degli eventi lenti, non la loro gravità.** Confronto fra due distribuzioni complete da 10 000 iterazioni: sotto carico le iterazioni > 25 µs crescono del **+62 %** e quelle > 55 µs del **+132 %**, ma **a parità di ritardo i contatori PMU sono identici** nelle due condizioni.
2. **Il ritardo è accompagnato da lavoro kernel aggiuntivo, ed è "a gradino".** Le iterazioni pulite consumano ~670-700 k cicli; tutte quelle sopra i 25 µs ne consumano ~750-840 k. Circa **100 000 cicli in più (~60 µs a 1,6 GHz)**, e il salto è netto, non graduale: o quel lavoro c'è, o non c'è.
3. **Il ritardo si colloca nel percorso kernel di timer/risveglio.** Il trace mostra la sequenza `arch_timer_handler_phys` → `hrtimer_interrupt` → `__hrtimer_run_queues` → `hrtimer_wakeup` → `try_to_wake_up` → `ttwu_do_activate` → `enqueue_task_rt` → context switch, senza alcun processo utente estraneo.
4. **CPU3 riceve ~673 interrupt di timer al secondo con Lnk attivo**, contro **0** a Lnk fermo. Di questi, ~250/s sono i risvegli del thread RT stesso (periodo 4 ms): restano **~420/s** di tick dello scheduler. Non sono 1 000/s perché `NO_HZ_IDLE` ne sopprime già gran parte.
5. **Un dettaglio strutturale dal trace**: la CPU entra in idle **22 µs prima** di dover risvegliare il thread RT, perché `SimPLCFAST` rilascia il core appena prima. Lo scheduler paga l'intero percorso di uscita (`balance_rt`, `balance_fair`, `newidle_balance`, `update_blocked_averages`, context switch) per nulla.

### 3. L'ipotesi finale

> **Il jitter residuo nasce nel percorso kernel di risveglio del thread real-time, il cui costo non è costante ma bimodale.**
>
> La GUI **non rallenta direttamente** quel percorso: nessuna misura mostra contesa di memoria, di banda o di cache. Ciò che fa è **aumentare la probabilità che il risveglio coincida con altro lavoro kernel periodico** — tick dello scheduler, accounting cgroup, aggiornamento PELT, percorso di bilanciamento pre-idle — che aggiunge il "gradino" di ~100 000 cicli osservato.
>
> Da qui la firma caratteristica dei dati: sotto carico **cambia la frequenza degli eventi lenti, non la loro gravità**, e il caso peggiore resta ancorato attorno ai 100-135 µs in ogni configurazione provata.

**Perché questa formulazione regge a tutti i dati:**

| Osservazione | Spiegazione nell'ipotesi finale |
|--------------|--------------------------------|
| Contatori identici a parità di ritardo | Il meccanismo è lo stesso; cambia solo quanto spesso si innesca |
| Salto "a gradino" nei cicli | O il lavoro kernel periodico cade nella finestra, o non ci cade |
| Il throttling migliora le medie ma non il massimo | Riduce quanto spesso la GUI è attiva, non il costo del percorso quando la collisione avviene |
| Isolare i core non ha protetto | `isolcpus` toglie il bilanciamento, non il tick né gli hrtimer |
| Le ottimizzazioni efficaci riducono i pixel toccati | Meno lavoro GUI ⇒ meno occasioni di collisione ⇒ meno eventi lenti |
| PM QoS senza effetto | Il costo non è nell'ingresso/uscita da idle |

**Livello di confidenza:** l'ipotesi è **coerente con tutti i dati raccolti** e ogni alternativa è stata esclusa con una misura. Non è però **dimostrata**: manca l'esperimento che rimuova la sorgente periodica e mostri il collasso della coda. Va presentata come l'ipotesi che sopravvive al vaglio, non come un fatto accertato.

### 4. Piste aperte, in ordine di rapporto beneficio/rischio

1. **`nohz_full=3 rcu_nocbs=3` nella cmdline** (attualmente c'è solo `isolcpus=3`; `/sys/devices/system/cpu/nohz_full` è **vuoto**). Rimuoverebbe i ~420 tick/s residui su CPU3. ⚠️ Guadagno **limitato e incerto**: `NO_HZ_IDLE` già sopprime la maggior parte del tick, e `nohz_full` è efficace **solo con un unico task runnable** sulla CPU — condizione non soddisfatta finché due thread RT condividono il core. Richiede riavvio.
2. **Il secondo thread real-time (`SimPLCFAST`) sul core isolato.** È ciò che impedisce a `nohz_full` di funzionare e che provoca l'ingresso in idle 22 µs prima del risveglio. Da chiarire **se esista un equivalente in produzione o se sia un artefatto del banco di prova**: la risposta cambia completamente la valutazione. Se è reale, spostarlo su un altro core o fonderlo col thread RTC renderebbe efficace il punto 1.
3. **Continuare a ridurre il lavoro della GUI.** È l'unica leva che ha dimostrato empiricamente di funzionare, ed è coerente con l'ipotesi: meno lavoro grafico ⇒ meno occasioni di collisione. Interventi già individuati e documentati: le 3 copie ridondanti del percorso 3D, `Posiziona` (~50 % del costo) dentro il disegno della pagina Manual Sequence, la scrittura diretta del rasterizzatore nel framebuffer PegLib.

### 5. Cosa NON provare (già escluso, con la misura che lo esclude)

Partizionare la banda o la cache (impossibile su Cortex-A53, niente MPAM), abbassare ulteriormente la quota cgroup (migliora la media, non il massimo — dimostrato), disabilitare gli stati di idle (fatto, nessun effetto), forzare la frequenza (già a `performance`), agire sul raffreddamento (45 °C di margine termico).

> **Conclusione operativa del lavoro (formulazione prudente).** Il caso peggiore residuo **non** è spiegato dalla banda DDR media, né si riduce con le manopole dello scheduler quando è dominato da singole operazioni pesanti. La direzione con maggiore probabilità di successo resta **ridurre il lavoro per disegno**: meno passate sui pixel (le 3 copie del 3D viewer), meno frame (coalescing del pan — già fatto, efficace), e togliere dal percorso di disegno ciò che non disegna (`Posiziona`, ~50% di un redraw della pagina Manual Sequence). Quale sia il meccanismo *fisico* preciso con cui questo lavoro disturba il thread RT — banda a raffiche, cache, page fault, lock — **resta da determinare** con la misura a 10 ms sopra.

##### 📊 MISURA CORRETTA con `CLOCK_THREAD_CPUTIME_ID` (2026-07-30) — i numeri difendibili

Rimisurato con tempo di **CPU effettivo del thread** invece di tempo trascorso, quindi immune alla deschedulazione da throttling.

**Costo totale del draw:** `med = 1 142 µs`, `max = 2 248 µs`. Un disegno costa quindi **oltre un millisecondo di CPU**, con picchi a 2,2 ms, su un ciclo RT di **4 ms**.

**Breakdown** (SEQ_MAN non-SIMULA, n=30; somma ≈ 1 672 µs):

| Voce | CPU media | CPU max | Quota | (misura elapsed precedente) |
|------|-----------|---------|-------|------------------------------|
| **`Posiziona`** | **832 µs** | 977 µs | **49,8%** | (1 528 µs) |
| `Disegni` | 693 µs | 768 µs | 41,4% | (945 µs) |
| `SettaFocusSuSequenza` | 102 µs | 131 µs | 6,1% | (~96 µs) |
| `ChiudiPiega` | 45 µs | 71 µs | 2,7% | (42 µs) |

**Il caveat sulla prima misura era fondato**: i valori elapsed erano gonfiati (Posiziona 1 528 → 832 µs, quasi la metà), e in modo **non uniforme** — le voci più lunghe erano inflazionate di più, essendo più probabile che venissero interrotte.

**Ma la conclusione non cambia**, ed è ora quantificata correttamente:

1. **`Posiziona` resta la singola voce più costosa**, ~50% del costo di ogni redraw della pagina, e **non disegna nulla**. Spostarla nell'handler che cambia piega dimezzerebbe il costo di ogni redraw di questa schermata, e lo azzererebbe per i repaint causati da altro.
2. **I `Disegni` (693 µs) sono il costo intrinseco** della rasterizzazione software di 7 poligoni pieni a 1024×600. Riducibile solo disegnando meno spesso (coalescing) o meno cose.
3. **`SettaFocusSuSequenza` costa 102 µs di CPU** dentro una routine di paint — dell'ordine dell'intero budget di jitter perseguito, per un'operazione che non c'entra col disegno.

**Da verificare come primo passo, prima di qualunque modifica:**

1. cosa fanno esattamente `CSeqManualeFrame::Posiziona(int)` e `ChiudiPiega()` — solo calcolo geometrico, o anche scrittura verso la macchina / memoria condivisa?
2. quanto costano, misurandoli con `clock_gettime` come già fatto per `check_collisioni_pezzo` (che risultò 4 µs, escludendo quella pista) — serve a confermare che siano loro i responsabili dei 135 µs prima di rifattorizzare
3. quali altri eventi, oltre a *Go Up*/*Go Down*, provocano un repaint di questa vista (e quindi pagano il ricalcolo inutilmente)

##### ✅ ESITO: il coalescing a 33 ms funziona — il grafico 2D non è più un hotspot (2026-07-30)

Sessione finale: **225 000 attivazioni** (900 s = **15 min esatti**), path DRM, fix PegGL, coalescing 33 ms, **throttling 6000/40000 (~15% CPU)**. Attività: uso intensivo e vario ("provato di tutto e di più"), incluso martellamento dei grafici.

| Metrica | Valore |
|---------|--------|
| Attivazioni | **225 000** |
| Durata | **15 min** |
| `nanosleep` **max** | **103 µs** |
| `nanosleep` min | 11 µs |
| Valori **> 100 µs** | **1** |
| 60–70 / 71–80 / 81–90 / 91–99 µs | 672 / 90 / 14 / 4 |
| max ultime 5 000 att. | 76 µs |

**Confronto con la sessione pre-modifica** (191 k att., stesso throttling, martellamento grafico 2D), normalizzato per milione:

| Fascia | Prima (coalescing 16 ms) | **Dopo (33 ms)** | Variazione |
|--------|--------------------------|------------------|------------|
| 60–70 µs | 2 586 /M | 2 987 /M | +15% |
| 71–80 µs | 1 314 /M | **400 /M** | ✅ **−70%** |
| 81–90 µs | 403 /M | **62 /M** | ✅ **−85%** |
| 91–99 µs | 215 /M | **18 /M** | ✅ **−92%** |
| **> 100 µs** | 47,1 /M | **4,4 /M** | ✅ **−91%** |
| **max** | 115 µs | **103 µs** | ✅ **−12 µs** |
| **totale > 60 µs** | 4 565 /M | **3 471 /M** | −24% |

**Lettura.** Tutte le fasce **sopra i 70 µs** crollano del 70-92%, e il massimo scende di 12 µs. L'unico valore in leggero aumento è la fascia più bassa (60-70 µs), coerente con un carico che si redistribuisce verso il basso: meno frame, ciascuno con lo stesso costo, quindi meno eventi che si accumulano fino alle fasce alte.

**Il dato più significativo:** gli sforamenti > 100 µs scendono a **4,4 per milione**, praticamente identici ai **4,7 /M** dell'uso generale (sessione 848 k). ⇒ **Il grafico 2D non è più un hotspot**: è rientrato al livello di rumore di fondo dell'applicazione.

**Solidità statistica.** Il confronto sugli sforamenti (1 evento contro 9) preso da solo sarebbe debole. Ma le fasce 71–80, 81–90 e 91–99 contengono **decine o centinaia di eventi** per parte e calano tutte, in modo monotono, del 70-92%: questo è statisticamente robusto e non attribuibile al rumore. ⚠️ Resta non controllata la **natura dell'attività**: la sessione precedente era martellamento puro del 2D, questa un uso vario. Poiché "uso vario e intensivo" non è meno esigente del martellamento puro, l'inferenza è comunque favorevole.

**➡️ Dove si è verificato l'unico sforamento residuo:** secondo l'osservazione dell'operatore, passando **da Piece Set a Manual martellando velocemente**. Cioè **non** nel disegno dei grafici, ma nella **macchina dei cambi pagina** — il [filone B](#test-finale-merged-scroll-calculation-2026-07-30) identificato e documentato, mai affrontato per scelta. È una chiusura coerente: l'ultimo picco rimasto sta esattamente nell'unico punto che si era deciso consapevolmente di non toccare, e di cui si conosce il meccanismo (due cambi pagina completi per pressione, teardown/rebuild di alberi di widget).

### 📐 Nota metodologica: frequenza di base degli sforamenti e durata minima dei test

Osservazione emersa provando a riprodurre gli sforamenti in sessioni brevi: **non si riproducono**, e non perché siano stati risolti, ma per pura statistica.

Con **5 sforamenti > 100 µs per milione di attivazioni** e un ciclo RT di **4 ms** (250 attivazioni/s):

- 250 × 60 = **15 000 attivazioni/minuto**
- 15 000 / 1 000 000 × 5 = **0,075 sforamenti/minuto**
- ⇒ **uno sforamento ogni ~13 minuti** in media

Conseguenze operative:

| Durata test | Sforamenti attesi | Utilità |
|-------------|-------------------|---------|
| 2 min | 0,15 | ❌ inutile: "zero" è il risultato più probabile anche senza alcun miglioramento |
| 10 min | 0,75 | ❌ ancora troppo rumoroso |
| 30 min | 2,3 | ⚠️ minimo indispensabile |
| 60 min | 4,5 | ✅ confrontabile con le sessioni già registrate |

**Regola da seguire per qualunque misura futura:** sotto la mezz'ora un risultato "zero sforamenti" **non dimostra niente**, e non va usato come prova di miglioramento. Tutte le sessioni confrontabili di questo registro (sezione S: 529 k att.; sezione U: 848 k e 1 589 k att.) durano tra ~35 min e ~1 h 46 min proprio per questo motivo.

⚠️ La soglia dei 30 minuti deriva dalla frequenza di base **del path DRM** (5/M). Se il path in prova ha una frequenza molto più alta — come l'SDL, vedi sotto, con 215/M — anche pochi minuti bastano per raccogliere eventi statisticamente significativi. La regola vera è: **servono abbastanza eventi**, non abbastanza minuti.

---

### 🏆 Confronto principale del lavoro: path SDL (Test 0) vs DRM diretto (Test 6)

**Metodologia — una sola variabile.** Il confronto è stato fatto **sullo stesso branch** (`experiment/test-6-ch0-defer-plus-pan-scroll`) e **sullo stesso codice applicativo**, commentando la sola riga 12 di `pegenstein/PegLib/PegLib.pro`:

```
#DEFINES += EMBEDDED_HMI_RT_DRM_DIRECT
```

Il path SDL è ancora integralmente presente negli `#else` di `peglvglwindow.cpp` (righe 978, 1060), quindi disattivare quel define fa ricadere l'applicazione sul path originale (`SDL_UpdateTexture` + renderer). **Nessun cambio di branch**, quindi defer CH0, ottimizzazioni pan/scroll e tutto il resto sono identici nelle due sessioni: l'unica differenza è il percorso di output verso il display.

| Metrica | **SDL** (Test 0) | **DRM diretto** (Test 6) |
|---------|------------------|--------------------------|
| Attivazioni | 65 000 | 848 000 |
| Durata | 260 s ≈ **4 min 20 s** | 3 392 s ≈ **57 min** |
| `nanosleep` **max** | **158 µs** | **113 µs** |
| `nanosleep` min | 11 µs | 11 µs |
| Valori **> 100 µs** | **14** | **4** |
| max ultime ~5–8 k att. | **108 µs** (sopra soglia) | **98 µs** (sotto soglia) |

**Normalizzato per milione di attivazioni** (necessario: le due sessioni hanno durata molto diversa):

| Fascia | **SDL** | **DRM diretto** | Rapporto |
|--------|---------|-----------------|----------|
| 60–70 µs | 2 369 /M | 279 /M | **8,5×** |
| 71–80 µs | 431 /M | 48 /M | **9,0×** |
| 81–90 µs | 215 /M | 16,5 /M | **13,0×** |
| 91–99 µs | 138 /M | 16,5 /M | **8,4×** |
| **> 100 µs** | **215 /M** | **4,7 /M** | **45,7×** |
| **max assoluto** | 158 µs | 113 µs | **−45 µs** |

**Frequenza degli sforamenti in tempo reale:**

- **SDL:** 14 sforamenti in 260 s → **uno ogni ~18 secondi**
- **DRM diretto:** 4 sforamenti in 3 392 s → **uno ogni ~14 minuti**

### Conclusione

Il passaggio dal path SDL al **DRM dumb buffer RGB565 diretto** riduce gli sforamenti oltre i 100 µs di un fattore **~46×** (**−98%**) e abbassa il **caso peggiore** da **158 µs a 113 µs** (−45 µs, −28%). È l'unico intervento di tutto il lavoro che ha spostato il **tetto massimo** del jitter, non solo la frequenza degli eventi: tutte le altre ottimizzazioni (defer CH0, pan/scroll) hanno agito sulla frequenza lasciando il picco invariato.

Il miglioramento è coerente su **tutte** le fasce della distribuzione (8–13× su ogni intervallo), il che esclude che si tratti di un artefatto di misura o di un singolo outlier fortunato.

**Robustezza del risultato nonostante la brevità della sessione SDL.** La sessione SDL dura solo 4 minuti, ben sotto la soglia dei 30 minuti stabilita nella nota metodologica sopra. In questo caso specifico il risultato resta però **conclusivo**, perché la frequenza degli eventi nel path SDL è 46 volte più alta: in 4 minuti ha già raccolto 14 sforamenti, cioè **più del triplo** di quanti il path DRM ne produca in un'ora. L'effetto è talmente grande da emergere anche con un campione breve. Una sessione SDL più lunga renderebbe più precisi i valori della distribuzione, ma non cambierebbe la conclusione.

⚠️ **Da confermare per completezza:** che nella sessione SDL fosse attivo lo stesso throttling cgroup 6000/40000 della sessione DRM. Se non lo fosse, l'SDL sarebbe stato misurato in condizioni **più favorevoli** e il vantaggio del DRM risulterebbe quindi ancora maggiore di quanto riportato.

---

### 🥇 SDL vs DRM sotto il carico più pesante: martellamento del grafico 3D (2026-07-30)

Coppia di sessioni dedicate, con lo **stesso numero di attivazioni** in entrambe (63 000) e la stessa attività: martellamento del **3D viewer** (`PAG_3D_VIEWER` = 59). Essendo identico il numero di attivazioni, i conteggi sono **direttamente confrontabili senza normalizzazione** — è il confronto metodologicamente più pulito di tutto il registro.

| Metrica | **SDL** | **DRM diretto** | Differenza |
|---------|---------|-----------------|------------|
| Attivazioni | 63 000 | 63 000 | — |
| Durata | 252 s ≈ **4 min 12 s** | 252 s ≈ **4 min 12 s** | — |
| `nanosleep` **max** | **180 µs** | **106 µs** | **−74 µs (−41%)** |
| `nanosleep` min | 12 µs | 13 µs | — |
| Valori **> 100 µs** | **33** | **4** | **−88% (8,3×)** |
| 60–70 µs | 363 | 212 | −42% |
| 71–80 µs | 227 | 179 | −21% |
| 81–90 µs | 87 | 63 | −28% |
| 91–99 µs | 43 | 30 | −30% |
| max ultime 3 000 att. | **109 µs** (sopra soglia) | **96 µs** (sotto soglia) | −13 µs |

**Conferma del risultato principale.** Il vantaggio del path DRM si conferma e si rafforza proprio sotto il carico grafico più severo: gli sforamenti oltre i 100 µs passano da **33 a 4** (−88%) e il caso peggiore da **180 a 106 µs** (−41%). Con SDL anche la finestra recente (ultime 3 000 attivazioni) resta sopra soglia a 109 µs, mentre con DRM scende a 96 µs, **sotto** l'obiettivo.

**Il 3D viewer è il carico più pesante individuato in tutto il lavoro.** Normalizzando per milione di attivazioni, gli sforamenti > 100 µs valgono:

| Scenario (path DRM) | Sforamenti > 100 µs |
|---------------------|---------------------|
| Uso comune + scroll 2D (848 k att., sez. U) | **4,7 /M** |
| Martellamento **3D viewer** (63 k att.) | **63,5 /M** |

Cioè il 3D viewer genera **~13 volte più sforamenti** dell'uso normale, anche sul path DRM. È quindi la **condizione peggiore rimasta** e il candidato naturale per il prossimo intervento di ottimizzazione.

**Meccanismo.** L'analisi architetturale completa dei due path — pipeline passo per passo, conteggio delle copie, i quattro fattori che spiegano i numeri, terminologia e principio del determinismo — è nella **[sezione V](#sdl-vs-drm-architettura)**. In sintesi: il path SDL comporta **due copie di pixel** invece di una, ridisegna **tutto lo schermo** ad ogni present anche per un aggiornamento minimo, **si blocca in attesa del vsync** dentro il thread GUI, e subisce una **conversione RGB565→ARGB8888 nascosta**; il DRM è RGB565 nativo end-to-end, copia solo la regione sporca e il page flip è asincrono.

Sul **3D viewer** in particolare: il sospetto della contesa GPU è documentato con riscontri nel codice (contesto EGL e `peglSwapBuffers` propri) ma **non è stato misurato** → vedi [3D viewer](#3d-viewer-gpu-2026-07-30).

---

---

<a id="3d-viewer-gpu-2026-07-30"></a>

### 🔍 3D viewer: sospetto identificato, NON risolto — lavoro futuro (2026-07-30)

Osservazione dell'operatore: **il 3D viewer dà molto fastidio** al jitter RT. Indagine sul codice (nessuna misura effettuata, il tempo residuo del tirocinio non lo consentiva).

**⚠️ Chiarimento architetturale: nell'applicazione ci sono DUE rasterizzatori software distinti.** È una distinzione facile da confondere ma essenziale per interpretare le misure.

| | **Grafici 2D** (Calculation, Die Set, viste CAD) | **3D viewer** |
|---|---|---|
| Rasterizzatore | primitive 2D di **PegLib** (`PegDrawPolygon`, linee, rettangoli) | **PegGL** — OpenGL ES software (triangoli, clipping, Z-buffer, luci) |
| Dove disegna | **direttamente** nel framebuffer PegLib | nel color buffer della `Surface` PegGL, area **separata** |
| Passi intermedi | nessuno | `renderToNative` (conversione + `memcpy`), poi `Bitmap()` (blit nel framebuffer PegLib) |
| Verso il display | `memcpy` → dumb buffer DRM → page flip | idem |
| **Copie dopo il disegno** | **1** | **3** |

`Surface` / `PegGL` sono usati **esclusivamente dal 3D viewer**: `PegGL` è linkata solo da `sim2d.pro:85` e da `pegmain.pro:66` (l'eseguibile, che linka tutto), e gli header `GLES/*` sono inclusi solo in `sim2d/3DViewerView.cpp:32` e `sim2d/ug/uglu.cpp:30`. Nessun altro modulo li tocca.

⇒ Questo spiega perché il 3D viewer risultasse il carico peggiore: oltre alla rasterizzazione 3D, intrinsecamente più costosa, si porta dietro **due copie a pieno frame più una passata di conversione** che i grafici 2D non hanno. E spiega perché i due interventi sono stati di natura diversa: sul 3D si è potuta rimuovere un'allocazione **dentro quella catena extra**; sui 2D, non esistendo catena extra, la sola leva disponibile era **disegnare meno spesso** (coalescing 16 → 33 ms).

**Verificato nel codice:**

| Fatto | Riferimento |
|-------|-------------|
| Pagina `PAG_3D_VIEWER` = 59, classe **`CAutG3D`** (deriva da `CPegDeskOgles`) | `sim2d/AutG3D.cpp`; `IncPPG/PegGenericDef.h:335`; apertura da `sim2d/Sim2DFrame.cpp:2323` |
| Disegna con API OpenGL ES: `glDrawArrays` su TRIANGLE_STRIP/FAN/LINES… | `sim2d/3DViewerView.cpp:1099-1123` |
| ⚠️ Ma **NON è OpenGL ES hardware**: le chiamate sono `pegl*` / `gl*` risolte da **`libPegGL.so`**, implementazione **software** di OGLES dentro pegenstein | `sim2d/ug/ug_win32.cpp:345-347` (commento "entry point exportati da libPegGL.so"); `pegenstein/PegGL/` |
| `peglGetDisplay` riceve un **`PegWindow*`** e `peglCreateWindowSurface` un **widget PEG** — non un display EGL né una finestra nativa | `sim2d/ug/ug_win32.cpp:347`, `:386` |
| `PegGL` contiene un **rasterizzatore software completo** (triangoli, clipper, virgola fissa) e persino un **JIT ARM** per i fragment | `pegenstein/PegGL/Rasterizer.cpp`, `RasterizerTriangles.cpp`, `TriangleClipper.inc`, `fixed.cpp`, `arm/CodeGenerator.cpp`, `arm/GenFragment.cpp` |
| `PegGL.pro` linka **solo `-lPegLib`**: nessun `-lGLESv2`, `-lEGL`, `-ldrm`, `-lgbm` ⇒ **nessun accesso alla GPU** | `pegenstein/PegGL/PegGL.pro:148-151` |
| `peglSwapBuffers` non fa un page flip: fa `ctx->Flush()` (rasterizzatore software), `renderToNative(..., BCF_INPUT_RGB16)` e poi **blit nella finestra PEG** → finisce nel framebuffer PegLib e da lì nel normale percorso DRM | `pegenstein/PegGL/egl.cpp:863-899` |
| ⚠️ Ad **ogni** swap fa `delete [] ...->pStart` e riallocazione del bitmap nativo | `pegenstein/PegGL/egl.cpp:891-894` |
| Timer armato a `ONE_SECOND/30` ≈ 33 ms (**30 Hz**) in `OnShow`, ucciso in `OnHide` | `sim2d/AutG3D.cpp:316-326`, `:332` |
| Il timer **NON ridisegna** ad ogni tick: legge registri di stato macchina e chiama `PassoAuto()`, che invoca `AggiornaView()` **solo se** piega/sezione sono cambiate | `sim2d/AutG3D.cpp:501-579`, `:581-599` (condizione alle righe 591-598) |
| La pagina è **usata in automatico durante la produzione**: legge i contapezzi (`HD_PIECEP`/`HD_PIECEM`), gestisce il cambio passo del ciclo, può salvare il programma | `sim2d/AutG3D.cpp:521-577` |

⚠️ **Ipotesi iniziale smentita:** si era supposto un render loop GPU continuo a 30 fps. **Non è così**: il redraw è condizionato al cambio piega/sezione. Il timer a 30 Hz fa comunque letture di registri in memoria condivisa ad ogni tick, ma non rendering.

### ❌ RETTIFICA (2026-07-30): l'ipotesi "contesa GPU" era SBAGLIATA

Qui era stato scritto che il meccanismo più probabile fosse la **contesa GPU**, sul presupposto che il 3D viewer avesse un proprio contesto EGL hardware e un proprio swap in parallelo al page flip DRM. **Non è così.** Le chiamate `pegl*` non sono EGL di sistema: sono entry point di **`libPegGL.so`**, un'implementazione **puramente software** di OpenGL ES interna a pegenstein (rasterizzatore, clipper, virgola fissa, JIT ARM per i fragment; `PegGL.pro` non linka alcuna libreria GPU). Il 3D viewer **non tocca la GPU**, e nemmeno presenta per conto proprio: `peglSwapBuffers` fa un blit nella finestra PEG, quindi l'immagine finisce nel framebuffer PegLib e da lì segue il normale percorso DRM come qualunque altro widget.

**Causa reale, verificata nel codice:** il 3D viewer è pesante perché fa **rasterizzazione 3D software sulla CPU**, nel thread della GUI, e per ogni frame esegue questa catena:

1. rasterizzazione software della geometria (triangoli, clipping, illuminazione) — `pegenstein/PegGL/Rasterizer*.cpp`
2. `ctx->Flush()` del rasterizzatore — `egl.cpp:875`
3. **`delete []` + riallocazione** del bitmap nativo, di dimensione pari alla finestra, **ad ogni swap** — `egl.cpp:891-894`
4. `renderToNative(..., BCF_INPUT_RGB16)` — conversione di formato dell'intera area — `egl.cpp:894`
5. `nativeDisplay->Bitmap(Put, ...)` — blit nella finestra PEG — `egl.cpp:897`

È tutto lavoro di **CPU e banda di memoria**, con in più un'**allocazione dinamica di un buffer grande ad ogni frame** (punto 3), che è particolarmente ostile al real-time: può causare page fault e contesa sul lock dell'heap, con latenze non prevedibili.

**Perché il divario SDL vs DRM si allarga proprio qui.** Non per contesa GPU, ma perché il 3D viewer produce aggiornamenti **grandi e frequenti** dell'area di disegno: con SDL ognuno di quegli aggiornamenti innesca conversione di formato, blit GPU a schermo intero e attesa vsync; con DRM innesca un solo `memcpy` della regione sporca. Più grandi e numerosi sono gli aggiornamenti, più il costo per-aggiornamento del path SDL si moltiplica.

⚠️ **Resta non misurato** il peso relativo dei cinque passi elencati sopra. In particolare l'allocazione per frame (punto 3) è il sospetto principale ma non è stata quantificata.

**Perché NON è stato toccato — decisione consapevole:** `CAutG3D` non è un visualizzatore passivo, è la pagina attiva **durante il ciclo di piegatura reale** (contapezzi, cambio passo, salvataggio programma). Un intervento sul suo modello di aggiornamento o sul path di presentazione richiede validazione durante un ciclo di produzione, che non era possibile nel tempo residuo. Il rapporto rischio/beneficio non lo giustificava.

**Conclusione generalizzabile.** Il 3D viewer non contraddice il risultato del Test 6, lo completa: l'intera grafica di questa applicazione — 2D e 3D — è **rasterizzata in software sulla CPU**. La GPU del SoC, nella configurazione DRM, è **praticamente inutilizzata**. Ne segue che:

> Su questa architettura il jitter RT è governato da **CPU e banda di memoria**, non dalla GPU. Il guadagno del Test 6 è venuto dall'aver eliminato dal percorso di presentazione l'unico consumatore di GPU che c'era (il renderer SDL), sostituendolo con un `memcpy` della sola regione sporca. Il 3D viewer resta il carico peggiore non perché usi la GPU, ma perché è il generatore di lavoro CPU/memoria più intenso dell'applicazione.

### ✅ FIX APPLICATO (2026-07-30): rimossa la riallocazione per frame in `peglSwapBuffers`

**Scoperta chiave: la logica di riuso del buffer esisteva già, ma il chiamante la disattivava.**

`renderToNative()` (`pegenstein/PegGL/egl.cpp:548-576`) gestisce correttamente il riuso:

```cpp
if (!native->pStart || nativeBytes != neededBytes)
{
    if (native->pStart)
        delete [] native->pStart;
    native->pStart = new UCHAR[neededBytes];
    ...
}
```

Rialloca **solo** se il puntatore è nullo oppure se la dimensione richiesta è cambiata. Ma `peglSwapBuffers` (righe **891-892**) faceva, prima di chiamarla:

```cpp
delete [] draw->GetBitmapNative()->pStart;
draw->GetBitmapNative()->pStart = NULL;
```

Azzerando `pStart`, la condizione `!native->pStart` risultava **sempre vera** ⇒ `delete[]` + `new[]` ad **ogni swap**, cioè ad ogni frame del 3D viewer.

**Perché costa così tanto.** Il buffer è grande quanto l'area di disegno: a 800×600 in RGB16 sono **~960 KB**, sopra la soglia `MMAP_THRESHOLD` di glibc (128 KB). Quindi `new[]`/`delete[]` non usano le free list dell'heap ma **`mmap`/`munmap`** diretti, e ogni frame comporta:

- **`munmap`** → smontaggio di pagine → **TLB shootdown**, che invia IPI (interrupt inter-processore) a **tutti i core**, incluso quello isolato dove gira il thread RT ⇒ stallo diretto del thread real-time
- **`mmap` + first touch** → circa **240 page fault** da 4 KB sulla nuova mappatura

È un meccanismo che spiega precisamente picchi di centinaia di µs su un core isolato, ed è coerente con il fatto che il 3D viewer sia il carico peggiore misurato.

**Origine probabile delle due righe.** Un residuo: il codice commentato in coda a `renderToNative` (`// *native = *convertedBitmap;`) mostra che l'implementazione **precedente** assegnava la struct, perdendo quindi il buffer precedente. La `delete` nel chiamante era il rimedio a quel leak. Riscritta la funzione con la gestione corretta del riuso, la free è diventata superflua ma non è stata rimossa. Conferma indiretta: l'altro chiamante, `peglCopyBuffers()` (riga **918**), **non** fa quella free.

**Fix:** rimosse le due righe, sostituite da un commento esplicativo. Nessuna modifica al comportamento visibile — stessi pixel, stesso formato, stessa dimensione. La rete di sicurezza a valle è già scritta **e già collaudata**, perché scatta ogni volta che la finestra cambia dimensione.

**Verifiche di correttezza fatte prima di applicare:**

- il bitmap nativo nasce da `m_pWin->Screen()->CreateBitmap(width, height)` (`PegGL/Surface.cpp:96-97`) e viene distrutto da `Screen()->DestroyBitmap(...)` nel distruttore (`Surface.cpp:143-146`) ⇒ il teardown **non cambia**: prima come dopo, alla distruzione `pStart` punta a un buffer non nullo
- nel caso comune (dimensione invariata) il fix **evita del tutto** la free del buffer allocato da `CreateBitmap`, quindi è se possibile più conservativo del codice precedente
- ⚠️ pre-esistente, **non introdotto** da questa modifica: `renderToNative` può sostituire `pStart` con un `new UCHAR[]`, che poi verrà liberato da `DestroyBitmap`. Se l'allocatore di `Screen()` non fosse `new[]/delete[]`, ci sarebbe un mismatch — ma la situazione è identica a prima del fix

**Validazione:** rifare il **martellamento del 3D viewer** con lo stesso protocollo già usato (63 000 attivazioni, path DRM), e confrontare con il dato esistente: **4 sforamenti > 100 µs, max 106 µs**. Attenzione: serve un rebuild di **PegGL** (pegenstein), non solo di pressbrakepeg.

#### ⚖️ Esito della validazione (2026-07-30): INCONCLUSIVA

Sessione post-fix: **70 000 attivazioni** (280 s ≈ 4 min 40 s), path DRM, martellamento 3D. Max **102 µs**, min 12 µs, **3** valori > 100 µs; 60–70: 851, 71–80: 207, 81–90: 95, 91–99: 51.

Confronto normalizzato per milione di attivazioni:

| Fascia | Prima del fix (63 k att.) | Dopo il fix (70 k att.) | Variazione |
|--------|---------------------------|-------------------------|------------|
| 60–70 µs | 3 365 /M | 12 157 /M | ⚠️ **3,6×** |
| 71–80 µs | 2 841 /M | 2 957 /M | ≈ invariata |
| 81–90 µs | 1 000 /M | 1 357 /M | ⚠️ +36% |
| 91–99 µs | 476 /M | 729 /M | ⚠️ +53% |
| **> 100 µs** | 63,5 /M | **42,9 /M** | ✅ **−32%** |
| **max** | 106 µs | **102 µs** | ✅ −4 µs |
| **totale eventi > 60 µs** | 7 746 /M | **17 243 /M** | ⚠️ **2,2×** |

**Perché l'esito è inconclusivo, e non va presentato come un successo.**

1. **Il carico non è stato lo stesso.** Il totale degli eventi sopra i 60 µs è **2,2× più alto** nella sessione post-fix. Rimuovere lavoro non può *aumentare* gli eventi: la spiegazione è che in quella sessione il 3D ha disegnato molti più frame. Il martellamento manuale **non è un carico riproducibile** — le attivazioni RT sono confrontabili (63 k vs 70 k) ma il numero di frame 3D renderizzati non è controllato, ed è proprio quello il carico dominante.

2. **Il campione sugli sforamenti è troppo piccolo.** 3 eventi contro 4 sono **statisticamente indistinguibili**. Per confrontare una frequenza dell'ordine di 50 per milione servono almeno ~30 eventi per parte, cioè circa **600 000 attivazioni ≈ 40 minuti per sessione**. Le due sessioni durano 4-5 minuti: un ordine di grandezza sotto il necessario per quella specifica metrica.

**Lettura prudente, dichiarata come inferenza.** Se la sessione post-fix ha comportato circa il doppio del lavoro di rendering e *nonostante questo* gli sforamenti > 100 µs sono scesi (63,5 → 42,9 /M) e il massimo è calato di 4 µs, l'indizio è **a favore** del fix: agirebbe sulla **coda** della distribuzione, che è esattamente l'effetto atteso eliminando `munmap` (TLB shootdown/IPI) e page fault. Ma resta un'inferenza su una variabile non controllata, non un risultato misurato.

**Il fix resta comunque giustificato indipendentemente da questa misura**, per due ragioni: (a) rimuove un'operazione **oggettivamente non deterministica** (allocazione/deallocazione di ~960 KB via mmap) da un percorso di disegno, che è una pratica corretta in un sistema real-time a prescindere dal guadagno misurabile; (b) **ripristina il comportamento per cui `renderToNative` era stata scritta**, eliminando codice ridondante — è una semplificazione, non un'aggiunta di complessità.

**Per chiudere la questione servirebbe:**

- un **carico riproducibile** al posto del martellamento manuale: es. rotazione automatica del 3D a cadenza fissa per un tempo fisso, così il numero di frame renderizzati è identico nelle due sessioni
- **≥40 minuti per sessione**, per avere ~30 sforamenti per parte
- idealmente, contare anche i **frame 3D renderizzati** in ciascuna sessione, per normalizzare rispetto al carico effettivo invece che rispetto alle attivazioni RT

#### 🥇 Sessione con throttling 15% + fix: primo risultato con ZERO sforamenti (2026-07-30)

Configurazione: branch merged, path **DRM**, **fix PegGL applicato**, **throttling cgroup 6000/40000 (~15% CPU)**, martellamento del 3D viewer.

| Metrica | Valore |
|---------|--------|
| Attivazioni | **77 000** |
| Durata | 308 s ≈ **5 min 8 s** |
| `nanosleep` **max** | **100 µs** |
| `nanosleep` min | 12 µs |
| Valori **> 100 µs** | **0** ✅ |
| 60–70 / 71–80 / 81–90 / 91–99 µs | 189 / 144 / 24 / 12 |
| max ultime 7 000 att. | **99 µs** |

**Confronto normalizzato con le due sessioni 3D precedenti** (per milione di attivazioni):

| Fascia | Pre-fix, no throttling (63 k) | Post-fix, no throttling (70 k) | **Post-fix + throttling 15% (77 k)** |
|--------|-------------------------------|--------------------------------|--------------------------------------|
| 60–70 µs | 3 365 /M | 12 157 /M | **2 455 /M** |
| 71–80 µs | 2 841 /M | 2 957 /M | **1 870 /M** |
| 81–90 µs | 1 000 /M | 1 357 /M | **312 /M** |
| 91–99 µs | 476 /M | 729 /M | **156 /M** |
| **> 100 µs** | 63,5 /M | 42,9 /M | **0 /M** ✅ |
| **max** | 106 µs | 102 µs | **100 µs** |
| **totale eventi > 60 µs** | 7 746 /M | 17 243 /M | **4 792 /M** |

**È il miglior risultato di tutto il registro**: prima sessione in assoluto con **zero** sforamenti, massimo esattamente a soglia, e totale degli eventi elevati più basso mai misurato (un terzo della sessione precedente).

⚠️ **Cosa NON si può attribuire.** Rispetto alla sessione da 70 k sono cambiate **due cose**: il fix era già presente in entrambe, ma qui è stato aggiunto il **throttling al 15%**. Il throttling riduce di per sé la capacità della GUI di interferire (meno CPU ⇒ meno lavoro nell'unità di tempo), e **non esiste una sessione pre-fix con throttling e martellamento 3D** che faccia da termine di confronto. Quindi il contributo del fix resta non isolato.

**Valutazione statistica dello zero.** Con la frequenza della sessione precedente (~43 sforamenti/M) su 77 000 attivazioni ci si aspetterebbero 3-4 eventi; la probabilità di osservarne **zero** è dell'ordine del **2-4%**. È quindi un indizio moderatamente favorevole, **non** una dimostrazione: la durata (5 minuti) resta un ordine di grandezza sotto la soglia utile stabilita nella nota metodologica.

**➡️ Test raccomandato per chiudere il lavoro** (una sola sessione, ~40 min): ripetere **esattamente questa configurazione** — DRM + fix + throttling 6000/40000 + martellamento 3D — per ~600 000 attivazioni. Se lo zero tenesse, si otterrebbe l'affermazione più forte e difendibile possibile:

> *Nella configurazione finale, sotto il carico più pesante dell'applicazione e con la GUI limitata al 15% della CPU, nessun sforamento oltre i 100 µs su ~600 000 attivazioni.*

che coincide con l'**obiettivo dichiarato** del lavoro ([→ Obiettivo](#obiettivo)).

**Piste per chi continuerà**, in ordine di rapporto beneficio/rischio:

1. **Misurare prima**: sessione RT dedicata con 3D viewer aperto vs chiuso, a parità di throttling e durata (≥30 min ciascuna, vedi nota metodologica sulla durata minima). Serve a quantificare il costo prima di toccare altro codice.
2. ~~Eliminare l'allocazione per frame~~ → ✅ **fatto**, vedi sopra. Da validare con misura.
3. **Eliminare le copie ridondanti — pista principale rimasta.** Il color buffer della surface GL è già a **16 bpp** (`PegGL/Surface.cpp:103`, `uBitsPix = 16`; i pixel sono scritti dal rasterizzatore via `m_ColorBuffer`, che è un alias dello stesso `m_pBitmap->pStart` — riga **117**), e lo schermo del target è **RGB565**, quindi anch'esso 16 bpp. Ne segue che `renderToNative` sta convertendo **da 16 bpp a 16 bpp**: l'unica operazione probabilmente indispensabile è il **ribaltamento verticale** (origine OpenGL in basso, origine PEG in alto), più eventualmente una correzione dell'ordine dei canali.

   Dopo la rasterizzazione gli stessi pixel vengono però copiati **tre volte**:

   | # | Copia | Dove |
   |---|-------|------|
   | 1 | `memcpy` verso il bitmap nativo | `PegGL/egl.cpp:575` (in `renderToNative`) |
   | 2 | blit del bitmap nativo nella finestra PEG | `PegGL/egl.cpp:897` (`nativeDisplay->Bitmap`) |
   | 3 | `memcpy` verso il dumb buffer DRM | `PegLib/pegdrmoutput.cpp` (`blitDirtyRegion`) |

   Tre passate su ~960 KB per ogni frame del 3D, per portare pixel **già nel formato di destinazione** dal buffer di rasterizzazione allo schermo. È la ridondanza più concreta emersa in tutto il lavoro. Direzione da valutare: far scrivere il rasterizzatore direttamente nel framebuffer PegLib (o almeno fondere i passi 1 e 2), gestendo il flip in fase di scrittura invece che con una passata dedicata. ⚠️ Invasivo e su libreria condivisa: da fare con misure prima e dopo, non per principio.

   Nota minore sulla memoria (non è un problema RT, allocazione una sola volta): per surface vengono allocati color 16 bpp, alpha 8 bpp, depth 16 bpp e **stencil 32 bpp** (`Surface.cpp:89-91`) ⇒ ~4,3 MB a 800×600. Lo stencil a 32 bit è probabilmente **non utilizzato** da questa applicazione: da verificare, eventualmente recuperabile.
4. Valutare se le letture di registri a 30 Hz del timer contendano lock/sezioni critiche con il lato RT (non indagato).
5. **Non** ridurre banalmente la frequenza del timer senza aver capito il punto 4: quel timer governa anche il cambio passo del ciclo automatico, non solo la grafica.
6. **Valutare l'uso della GPU vera** per il 3D: oggi è inutilizzata, e spostare la rasterizzazione 3D dalla CPU alla GPU potrebbe liberare CPU/banda a beneficio del RT. È però un intervento di portata ben maggiore (richiede EGL/GLES reali, integrazione con il path DRM) e va valutato con misure, non per principio: il TEST 5b insegna che spostare lavoro sulla GPU può peggiorare il RT.

---

<a id="sdl-vs-drm-architettura"></a>

## V — SDL vs DRM diretto: confronto architetturale completo

Sezione di riferimento sulle differenze tra le due architetture di presentazione. Raccoglie in un unico posto pipeline, meccanismi, terminologia e risultati misurati. · [← Tabella](#stato-test) · Dati e misure: [sezione U](#test-finale-merged-scroll-calculation-2026-07-30) · Confronto con Qt: [Differenze strutturali interfacce](#differenze-strutturali-interfacce)

### 1. Come si commuta tra i due path

Una sola riga in `pegenstein/PegLib/PegLib.pro` (riga **12**):

```
DEFINES += EMBEDDED_HMI_RT_DRM_DIRECT     # attivo  -> path DRM diretto (Test 6)
#DEFINES += EMBEDDED_HMI_RT_DRM_DIRECT    # commentato -> path SDL (Test 0)
```

Il path SDL **non è stato rimosso**: vive negli `#else` di `peglvglwindow.cpp` (righe **978** e **1060**). Questo permette un A/B a **una sola variabile**, senza cambiare branch — ed è il motivo per cui i confronti della sezione U sono metodologicamente solidi: defer CH0, ottimizzazioni pan/scroll e tutto il resto del codice applicativo restano identici nelle due sessioni.

### 2. Le due pipeline, passo per passo

**Path SDL** — `pegenstein/PegLib/peglvglwindow.cpp`

| # | Passo | Riferimento | Natura |
|---|-------|-------------|--------|
| 1 | PegLib/LVGL disegna in `m_framebuffer` (bitmap software) | — | CPU |
| 2 | `SDL_UpdateTexture(m_texture, &rect, src, pitch)` | riga **1062** | **copia** bitmap → texture (creata `SDL_TEXTUREACCESS_STREAMING`, riga **492**) |
| 3 | `SDL_RenderClear` + `SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr)` | riga **671** | **la GPU** rasterizza la texture nel backbuffer |
| 4 | `SDL_RenderPresent(m_renderer)` | riga **672** | flip backbuffer↔frontbuffer, **con attesa vsync** |

**Path DRM diretto** — `pegenstein/PegLib/pegdrmoutput.cpp`

| # | Passo | Riferimento | Natura |
|---|-------|-------------|--------|
| 1 | PegLib/LVGL disegna in `m_framebuffer` — **identico**, questa parte non cambia | — | CPU |
| 2 | `blitDirtyRegion` → `memcpy` in `m_buffers[m_backIndex]` | righe **525**, **629** | **copia** della sola **regione sporca** |
| 3 | `drmModePageFlip(m_fd, m_crtcId, nextFb, DRM_MODE_PAGE_FLIP_EVENT, this)` | riga **691** | flip **asincrono**, ritorna subito |

### 2-bis. Sequenza completa di chiamate: cosa accade trascinando un grafico 2D

Tracciata sul path DRM, dal touch al pixel sullo schermo. Utile per capire dove agiscono i freni e dove si spende il tempo.

| Fase | Passo | Riferimento |
|------|-------|-------------|
| **Input** | `processEvents()` interroga il touch via evdev: `m_drmEvdev->poll(&drmEvdevTouchThunk, this)` | `peglvglwindow.cpp:705-710` |
| | `drmEvdevTouchThunk` → `handleDrmEvdevTouch(fbX, fbY, action)` | `:1261`, `:1270` |
| | → `emitMouseEvent(...)`: inietta l'evento nella coda messaggi PEG | `:1275` (down), `:1284` (motion) |
| **Dispatch** | PegLib consegna il messaggio al widget con la cattura del puntatore | — |
| | `CSim2DView::Message()`: `PM_LBUTTONDOWN` / `PM_POINTER_MOVE` / `PM_LBUTTONUP` | `Sim2DView.cpp:130` / `:167` / `:142` |
| **Pan** | `OnMouseMove()`: calcola lo spostamento, aggiorna `m_ptTo`, `m_bPanRedrawPending = TRUE`, chiama `DrawPanIfDue(FALSE)` | `Sim2DView.cpp` (dopo `DrawPanIfDue`) |
| | **🚦 Freno 1** — `DrawPanIfDue()`: se < **33 ms** dall'ultimo redraw **ritorna**; altrimenti `Invalidate()` + `Draw()` | `Sim2DView.cpp:1563` (costante) |
| **Disegno** | `Draw()` → `BeginDraw()` … `EndDraw()` | `Sim2DView.cpp:181` |
| | `DrawDisView()`: fondo (`DisegnaVideo`, `:238`), poi Sup/Punz/Inf/Mat (`:305-308`), Riscontro/Pezzo (`:318-319`), `DrawCollisioni` (`:324`) | `Sim2DView.cpp:225` |
| | ogni `Disegna*` → `PegDrawPolygon(...)`: riempimento a scanline **dentro `m_framebuffer`** | `:896, 930, 988, 1024, 1095, 1299` |
| | `EndDraw()` → PegLib registra la regione sporca | — |
| **→ DRM** | `processPendingUpdates()` legge il rettangolo sporco | `peglvglwindow.cpp:585-613` |
| | → `uploadDirtyRegion(...)` | `:608` (impl. `:971`) |
| | → sotto `PegFrameBufferLock`: `blitDirtyRegion(...)` = `memcpy` riga per riga nel back buffer (2 byte/px) | `:1033`; `pegdrmoutput.cpp:525,629` |
| | `m_pendingPresent = true` | `:609` |
| **Present** | main loop → `flushPresent(false)` | `peg_run.cpp:1515` |
| | **🚦 Freno 2** — se < `rtPresentIntervalMs()` dall'ultimo present **ritorna** | `peglvglwindow.cpp:628` |
| | svuota la coda dirty fino a 8 volte (evita frame incompleti in scroll) | `:636-643` |
| | **`syncBackFromPeg(...)`**: snapshot **completo** del framebuffer PEG nel back buffer | `:650` |
| | `pageFlip()` → `drmModePageFlip(..., DRM_MODE_PAGE_FLIP_EVENT, ...)` — **asincrono** | `:653`; `pegdrmoutput.cpp:691` |
| | al vblank il display controller inizia a scandire il nuovo buffer | — |

**Due osservazioni pratiche che emergono dalla sequenza:**

1. **Esistono due freni indipendenti**: uno sul **disegno** (`DrawPanIfDue`, 33 ms dopo la modifica del 2026-07-30) e uno sul **present** (`flushPresent` / `rtPresentIntervalMs()`). Agiscono su stadi diversi della catena e vanno considerati separatamente.
2. Il freno sul present è **regolabile a runtime** tramite la variabile d'ambiente **`PEG_PRESENT_INTERVAL_MS`** (vedi commento a `peglvglwindow.cpp:39`), quindi si possono provare cadenze di presentazione diverse **senza ricompilare** — utile per campagne di misura rapide sul target.

### 3. Buffer e copie: chiarimento sul conteggio

Un fraintendimento facile è contare "5 buffer con SDL contro 3 con DRM". In realtà:

- **`render` non è un buffer**, è l'operazione GPU che *produce* il backbuffer
- **frontbuffer e backbuffer non sono due copie**: sono la stessa coppia di superfici che si alternano, e il flip **non copia nulla** — cambia solo quale delle due il display controller legge
- **anche il DRM è double buffered**: alloca `m_buffers[0]` e `m_buffers[1]` (riga **284**)

Quindi il vantaggio **non** è "meno buffer", è **meno copie di pixel**:

| | Copie reali di pixel | Flip |
|---|---|---|
| **SDL** | **2** — bitmap→texture (CPU/driver), texture→backbuffer (GPU) | sì, + attesa vsync |
| **DRM** | **1** — bitmap→dumb buffer (`memcpy`) | sì, asincrono |

### 4. Le quattro differenze che spiegano i risultati

**① Due copie di pixel contro una.** Vedi tabella sopra. Il flip, in entrambi i casi, non sposta dati.

**② Regione sporca sugli aggiornamenti incrementali; entrambi però fanno un'operazione a pieno schermo per present.**

⚠️ **RETTIFICA (2026-07-30):** qui era scritto che il path DRM copia *solo* la regione sporca. È vero per gli aggiornamenti incrementali, **ma non per il present**: `flushPresent` esegue `syncBackFromPeg(m_framebuffer, framePitchBytes())` (`peglvglwindow.cpp:650`), cioè uno **snapshot completo** del framebuffer PEG prima di ogni page flip. Il motivo è documentato alle righe **645-646**: con il double buffering il back buffer conterrebbe solo l'ultima regione sporca e mancherebbe quanto disegnato nell'altro buffer, producendo artefatti (rettangoli bianchi / frame incompleti).

Quadro corretto:

| | Aggiornamento incrementale (per disegno) | Operazione per present |
|---|---|---|
| **SDL** | `SDL_UpdateTexture` della sola regione sporca (riga 1062) | `SDL_RenderCopy(..., nullptr, nullptr)` (riga **671**) = **blit GPU a pieno schermo** + `SDL_RenderPresent` con **attesa vsync** |
| **DRM** | `blitDirtyRegion` = `memcpy` della sola regione sporca (`pegdrmoutput.cpp:525,629`) | `syncBackFromPeg` = **`memcpy` a pieno schermo** (riga **650**) + `drmModePageFlip` **asincrono** |

⇒ Entrambi fanno un'operazione a pieno frame per present. La differenza non è quindi "pieno schermo contro regione sporca", ma **la natura di quell'operazione**: un `memcpy` deterministico contro lavoro GPU (con conversione di formato) più un'attesa bloccante sul vsync. Il che rafforza, anziché indebolire, l'argomento del punto ⑥ (determinismo, non throughput) — che resta la spiegazione principale.

**③ Attesa bloccante contro operazione asincrona** — probabilmente il fattore dominante per il jitter. Il renderer SDL è creato con `SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC` (righe **314-315** e **464-465**) e in più viene forzato `SDL_RenderSetVSync(renderer, 1)` (riga **323**): `SDL_RenderPresent` **si blocca fino al vsync**, cioè fino a ~16 ms di attesa dentro il thread della GUI, con la relativa pressione sullo scheduler al risveglio. `drmModePageFlip` con `DRM_MODE_PAGE_FLIP_EVENT` **registra la richiesta e ritorna immediatamente**; la conferma arriva come evento.

**④ Conversione di formato nascosta contro RGB565 nativo end-to-end.**

Il **DRM è RGB565 nativo dall'inizio alla fine**: `createReq.bpp = 16` (`pegdrmoutput.cpp:314`), `DRM_FORMAT_RGB565` nel `drmModeAddFB2` (riga **364**), blit con `left * 2` perché 2 byte/pixel (riga **512**), e `initialize()` rifiuta esplicitamente qualunque bpp ≠ 16 (righe **448-450**). Framebuffer PEG RGB565 → dumb buffer RGB565 → scanout RGB565: **zero conversioni**.

Il **path SDL chiede RGB565 ma non lo ottiene**: a `peglvglwindow.cpp:475` viene richiesto `SDL_PIXELFORMAT_RGB565`, ma il **TEST 5** aveva diagnosticato che sull'i.MX8MP con renderer `opengles2` il formato **nativo è ARGB8888**. SDL accetta la richiesta e poi **converte internamente ad ogni `SDL_UpdateTexture`**, in un passaggio invisibile nel codice applicativo. La conversione **raddoppia i byte scritti** (2 → 4 byte/pixel): a 800×600 sono **1,83 MB per frame invece di 938 KB**.

### 5. Terminologia, per evitare equivoci

| Termine | Cos'è | Usato nel path SDL? | Usato nel path DRM? |
|---------|-------|---------------------|---------------------|
| **Display controller** | Il blocco hardware che legge una zona di memoria e genera il segnale per il pannello | ✅ sempre | ✅ sempre |
| **GPU** (GC7000 su i.MX8MP) | L'acceleratore grafico: compone, scala, rasterizza, 3D | ✅ sì (rasterizza la texture) | ❌ **no** (per la presentazione) |
| **Dumb buffer** | Memoria "stupida", non accelerata, che il display controller può scandire direttamente | — | ✅ è la destinazione del `memcpy` |

Nel ramo DRM, SDL viene inizializzato con `SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER)` (riga **394**): il sottosistema **video non viene nemmeno avviato**, SDL serve solo per gli eventi di input. Il commento a riga **389** lo dice esplicitamente: "niente SDL video/texture/GPU".

✅ **E nemmeno il 3D viewer la usa** (verificato 2026-07-30): le sue chiamate `pegl*`/`gl*` sono risolte da **`libPegGL.so`**, un'implementazione **software** di OpenGL ES interna a pegenstein (rasterizzatore, clipper, virgola fissa, JIT ARM; nessuna libreria GPU linkata). Quindi nella configurazione DRM la **GPU del SoC è praticamente inutilizzata**: tutta la grafica, 2D e 3D, è rasterizzata dalla CPU → vedi [3D viewer](#3d-viewer-gpu-2026-07-30).

### 6. Il principio: determinismo, non throughput

Il path DRM non fa necessariamente *meno lavoro di CPU*: un `memcpy` sposta byte, e in certi casi può spostarne quanti l'upload di una texture. Il vantaggio è che è lavoro **deterministico**: un `memcpy` dura quanto dura, sempre. L'invio di comandi alla GPU, l'attesa del suo completamento e la sincronizzazione col vsync introducono latenze **variabili**, che il thread real-time non può né controllare né prevedere.

Ed è la **varianza**, non il carico medio, a produrre il jitter misurato. È la ragione per cui questo è **l'unico intervento di tutto il lavoro che ha abbassato il caso peggiore** e non solo la frequenza degli eventi: le altre ottimizzazioni (defer CH0, pan/scroll) hanno ridotto *quante volte* si fa lavoro, questa ha reso il lavoro *prevedibile*.

**La prova sperimentale del principio è il TEST 5b.** Rendendo la conversione di formato esplicita e usando il formato nativo ARGB8888 (`peglvglwindow.cpp:1049-1056`, oggi compilate fuori): **GUI +150%** ma **RT peggiorato a 191 µs** → rollback. Più veloce nel throughput grafico, peggiore nel determinismo, perché ARGB8888 raddoppia il traffico verso la memoria e la **banda DDR** è la risorsa che il thread RT si contende con la GUI. ⇒ **Prestazioni grafiche e determinismo real-time sono due assi indipendenti**, e ottimizzare il primo può peggiorare il secondo.

### 7. La sequenza logica del lavoro

| Test | Cosa ha fatto | Esito |
|------|---------------|-------|
| **TEST 5** | Diagnosi: SDL non ha RGB565 nativo su questa piattaforma → conversione nascosta ad ogni upload | ✅ diagnosi, nessun fix |
| **TEST 5b** | Tentativo "ovvio": rendere la conversione esplicita + formato nativo | ❌ GUI +150% ma **RT 191 µs** → rollback. Dimostra che l'asse ottimizzato era quello sbagliato |
| **TEST 6** | Cambio di strada: **eliminare il bisogno di convertire** e rimuovere la GPU dal percorso di presentazione | ✅ sforamenti −98%, caso peggiore 158 → 106 µs |

Il 5b non è un fallimento da nascondere: è il test che **giustifica** la scelta del 6. Senza quel tentativo andato male non ci sarebbe la prova che l'approccio "rendiamo la conversione più efficiente" era la strada sbagliata.

### 8. Risultati misurati (sintesi)

Dettagli e caveat in [sezione U](#test-finale-merged-scroll-calculation-2026-07-30).

| Confronto | SDL | DRM diretto | Guadagno |
|-----------|-----|-------------|----------|
| Uso generale — sforamenti > 100 µs | 215 /M | **4,7 /M** | **−98% (~46×)** |
| Uso generale — caso peggiore | 158 µs | **113 µs** | **−45 µs** |
| Uso generale — frequenza sforamenti | 1 ogni ~18 s | **1 ogni ~14 min** | ~46× |
| Martellamento 3D viewer — sforamenti > 100 µs (63 k att. per parte) | 33 | **4** | **−88%** |
| Martellamento 3D viewer — caso peggiore | 180 µs | **106 µs** | **−74 µs (−41%)** |
| Distribuzione 60–90 µs | — | — | 8–13× meglio su ogni fascia |

Il miglioramento è **coerente su tutte le fasce**, il che esclude un artefatto di misura o un singolo outlier.

### 9. Corollario operativo

> Su questa architettura il jitter RT è governato da **CPU e banda di memoria**, non dalla GPU. Tutta la grafica — 2D e 3D — è rasterizzata in software: nella configurazione DRM la GPU del SoC è praticamente **inutilizzata**. Il guadagno del Test 6 è venuto dall'aver eliminato dal percorso di presentazione l'unico consumatore di GPU esistente (il renderer SDL, con la sua conversione di formato, il blit a schermo intero e l'attesa vsync), sostituendolo con un `memcpy` della sola regione sporca. Ne segue che ogni futura ottimizzazione va cercata sul fronte **CPU / banda / determinismo delle allocazioni**, non sull'accelerazione grafica.

### 10. Cosa resta NON verificato

- **Contesa GPU come meccanismo del jitter del 3D viewer**: il sospetto è documentato con riscontri nel codice (contesto EGL proprio, `peglSwapBuffers` indipendente — vedi [3D viewer](#3d-viewer-gpu-2026-07-30)), ma **non è stato misurato**. Servirebbe una sessione dedicata 3D aperto vs chiuso, a parità di throttling e durata ≥30 min.
- **Peso relativo dei quattro fattori** ① ② ③ ④: sappiamo che insieme producono il risultato, non quanto contribuisca ciascuno. Isolarli richiederebbe build intermedie (es. DRM ma con vsync bloccante, oppure SDL con `RenderCopy` limitato alla regione sporca).
- **Se nella sessione SDL fosse attivo lo stesso throttling cgroup** della sessione DRM. In caso negativo l'SDL sarebbe stato misurato in condizioni più favorevoli, e il vantaggio del DRM risulterebbe **ancora maggiore**.

---

---

**Contesto:** Leonardo ha osservato che martellando rapidamente anche gli **altri pulsanti in alto** (non solo Editor/Manuale) si riproduce lo stesso pattern di jitter RT già visto e risolto per Editor↔Manuale (sezione P).

**Analisi codice (branch `experiment/test-6-deferred-ch0-feedback`, commit `a05d77e52aa8d3b9955b4256deb2bfeb4988a0db`):**

`CMDINum::AttivaPaginaCH0()` è già il percorso comune anche per altri stati, non solo IMP/MAN:

| Stato | Chiamato da | Defer oggi? |
|-------|-------------|--------------|
| IMP | `CPpgView::OnImpostazioni()` | ✅ sì (300 ms) |
| MAN | `CPpgView::OnMan()` | ✅ sì (300 ms) |
| SAUTO | `CPpgView::OnSauto()`, `GoniometroView.cpp:1433` (uscita goniometro) | ❌ no — lavoro pesante immediato |
| AUTO | `CPpgView::OnAutomatico()` | ❌ no — lavoro pesante immediato |
| CORR | `CorrBaseVw.cpp:331` (uscita da Correzioni) | ❌ no — lavoro pesante immediato |

Il ramo `else` di `AttivaPaginaCH0` ("AUTO/SAUTO/…: nessun defer") esegue **la stessa identica sequenza pesante** (`SettaControlli`/`GetEntry`/`AttivaMenu`) di IMP/MAN, solo senza il timer di defer — questo spiega perché martellare quei pulsanti riproduce lo stesso jitter.

**Rischio trovato — Start/Stop/Plus/Minus:** `AttivaMenu(nStato)` per gli stati **SAUTO** e **AUTO** chiama `ConfigButtonsStartStopPlusMinus(this)` (`PpgView.cpp` righe **6874** e **7216**) — è la funzione che cablea i tasti touch **Start/Stop/Plus/Minus**, cioè il comando reale di avvio/stop ciclo di piegatura. Se si differisse tutto `AttivaMenu` anche per SAUTO/AUTO, per 300 ms dopo il cambio toolbar quei tasti resterebbero cablati come nello stato precedente (nel caso migliore: tasto che sembra non rispondere; nel caso peggiore: esegue il comando mappato in quella posizione nello stato precedente). **Decisione: non toccare in alcun modo Start/Stop/Plus/Minus.**

**Scoperta collaterale (non bloccante):** `AttivaMenu(MAN)` — già in produzione, già validato con le 137 000 attivazioni di martellamento Editor↔Manuale (sezione P) — chiama **anch'esso** `ConfigButtonsStartStopPlusMinus` (`PpgView.cpp` riga **6753**). Questa stessa finestra di 300 ms su Start/Stop esiste quindi **già oggi** per Manuale, nel codice già shippato; non ha causato problemi osservati nei test fatti finora. Non bloccante, ma da tenere a mente.

**`CORR` è invece sicuro:** `CCorrBaseVw::AttivaMenu(CORR)` (`CorrBaseVw.cpp` righe 351+) non chiama mai `ConfigButtonsStartStopPlusMinus` — imposta solo tasti `+1/-1/+0.1/-0.1/…` e tabella materiali/coefficienti correzione. Nessun comando macchina coinvolto.

**Piano concordato:**

| Stato | Trattamento previsto |
|-------|----------------------|
| **CORR** | Defer **completo**, stesso pattern di IMP/MAN (toolbar feedback immediato; `SettaControlli`/`GetEntry`/`AttivaMenu` differiti 300 ms) |
| **AUTO**, **SAUTO** | Defer **parziale**: differire solo `SettaControlli`/`GetEntry` (lavoro editor pesante); `AttivaMenu` resta **sempre immediato** per questi due stati → Start/Stop/Plus/Minus si aggiornano di scatto come oggi, zero finestra di rischio |

**Esplicitamente fuori scope per ora (subsystem diverso, da valutare a parte prima di toccare):**

- **Program List / Tool List** (icone "stack DB" in toolbar): passano da `GestionePagine::CambiaPagina` + `liste/ListeDef.cpp`, **non** da `AttivaPaginaCH0` — stesso sottosistema del vecchio crash Die List↔Program List (rollback descritto nell'handoff). Il pattern defer CH0 non si applica lì senza un'analisi dedicata.
- **Posiziona**: `CPpgView::OnPosiziona()` apre un dialog modale (`CPosDlg`), non passa da `AttivaPaginaCH0` — causa del jitter (se presente) da indagare a parte.
- **Salva / frecce / cestino**: probabili funzioni dirette di `CMDINum` (`OnPrev`/`OnNext`/`OnDel`/salvataggio) non ancora verificate, presumibilmente non legate a questo meccanismo.

**Da fare prima di scrivere codice:** mappare con certezza quale icona della toolbar in alto corrisponde a quale stato/funzione (toccare ogni icona sul target e annotare il titolo pagina mostrato — come già fatto per "Piece Set." / "Manual"), per scoprire lo scope esatto del branch.

**Branch:** ✅ creato da Leonardo a partire da `experiment/test-6-deferred-ch0-feedback` (commit `a05d77e52aa8d3b9955b4256deb2bfeb4988a0db`) — nome effettivo **`experiment/test-6-ch0-defer-corr-auto-sauto-estensione`**.

---

### Mappatura icone toolbar in alto (confermata sul target, 2026-07-29)

Verificata da Leonardo toccando le icone e leggendo titolo pagina / messaggi di errore mostrati:

| Icona | Stato/funzione | Come confermato |
|-------|-----------------|------------------|
| Documento | **IMP** (Piece Set.) | Titolo pagina |
| Manina | **MAN** (Manual) | Titolo pagina |
| Chiave inglese | **SAUTO** o **AUTO** | Errore "Program not optimized!" (= `ControlloProgramma()` di `OnSauto`/`OnAutomatico`) |
| Fabbrica | **AUTO** o **SAUTO** (l'altro dei due) | Stesso errore "Program not optimized!" |
| "C" | **CORR** (Correzioni) | Errore "General data are not complete!" |
| 2 icone "stack dischi" | Program List / Die List | Titolo pagina, quando si carica un programma |
| Salva / frecce / trattino / cestino | Funzioni dirette `CMDINum` (`OnPrev`/`OnNext`/`OnDel`/salvataggio…) | Non passano da `AttivaPaginaCH0` |

Non serve distinguere con certezza quale tra chiave inglese/fabbrica sia AUTO e quale SAUTO: ricevono lo stesso trattamento (defer parziale), vedi sotto.

---

### Implementazione (2026-07-29, `MDINum.cpp`)

**`CompletaAttivaPaginaCH0(int nStato)`** — aggiunta una condizione prima della chiamata ad `AttivaMenu`:

```cpp
// [AI] AUTO/SAUTO: AttivaMenu (tasti touch Start/Stop/Plus/Minus, via
// ConfigButtonsStartStopPlusMinus) e' gia' stato eseguito SUBITO in
// AttivaPaginaCH0, ad ogni pressione - non richiamarlo qui per non
// raddoppiare il lavoro e per non lasciarlo mai in batch su questi due stati.
if((nStato != AUTO) && (nStato != SAUTO))
	AttivaMenu(nStato);
```

**`AttivaPaginaCH0(int nStato)`** — `AttivaMenu` chiamato subito per AUTO/SAUTO, prima di decidere il defer; condizione del defer estesa a CORR/AUTO/SAUTO:

```cpp
// AUTO/SAUTO: AttivaMenu pilota anche ConfigButtonsStartStopPlusMinus -> sempre
// immediato, ad ogni pressione, mai in batch. Solo il lavoro editor
// (SettaControlli/GetEntry) viene differito.
if((nStato == AUTO) || (nStato == SAUTO))
	AttivaMenu(nStato);

// IMP/MAN/CORR/AUTO/SAUTO: differisci il lavoro pesante di CH0_DEFER_DELAY_MS ms.
// CORR verificato sicuro (CCorrBaseVw::AttivaMenu non tocca Start/Stop/Plus/Minus).
if((nStato == IMP) || (nStato == MAN) || (nStato == CORR) || (nStato == AUTO) || (nStato == SAUTO))
{
	if(m_nLastCompletedCH0State < 0)
	{
		CancelCH0HeavyWorkSchedule();
		CompletaAttivaPaginaCH0(nStato);
		return;
	}

	m_nPendingCH0State = nStato;
	m_bCH0WorkPending = TRUE;
	m_dwTimeLast = GetTickCount();
	ScheduleCH0HeavyWork(nStato);
	return;
}

// Altri stati (es. POSI): nessun defer, comportamento invariato
CancelCH0HeavyWorkSchedule();
CompletaAttivaPaginaCH0(nStato);
```

**Effetto per AUTO/SAUTO ad ogni singola pressione, anche martellando:** toolbar, stato macchina e Start/Stop/Plus/Minus si aggiornano **sempre atomicamente e subito** (mai stale, zero finestra di rischio); solo `SettaControlli`/`GetEntry` (edit numerici) vengono differiti e fatti una volta sola all'ultimo stato richiesto, dopo 300 ms di pausa.

**Effetto per CORR:** identico a IMP/MAN — tutto il lavoro pesante (incluso `AttivaMenu`) differito, nessun rischio essendo verificato che non tocca comandi macchina.

**Non ancora fatto:** build, deploy (`libeditorprogrammi.so`), smoke test UI, campagna di martellamento su tutti gli stati coinvolti (vedi checklist sotto).

### Checklist di validazione prima di dichiarare "funziona"

1. Naviga normalmente Editor → Manuale → Semiauto → Automatico → Correzioni → Editor: verifica che ogni pagina mostri i dati corretti.
2. Con un programma **non ottimizzato**: verifica che Auto/Semiauto mostrino ancora correttamente "Program not optimized!" (il controllo `ControlloProgramma()` avviene *prima* di `AttivaPaginaCH0`, non deve essere toccato da questa modifica).
3. In Automatico/Semiauto: verifica che i tasti **Start/Stop/Plus/Minus** rispondano **immediatamente** ad ogni pressione del pulsante di stato, anche martellando velocemente Auto↔Semiauto↔Manuale.
4. Martella velocemente Correzioni ↔ Editor ↔ Manuale ~10 volte: verifica nessun campo sovrapposto o testo sporco (stesso check già fatto per Editor/Manuale).
5. Martella velocemente Automatico ↔ Semiautomatico ~10 volte: verifica che alla fine la UI mostri lo stato corretto e i dati (angoli, quote) siano quelli giusti per l'ultimo stato richiesto.
6. Solo dopo questi check, misurare `nanosleep`/`rtc_handler_us` con Lnk attivo durante il martellamento di tutti e 3 gli stati nuovi (CORR, AUTO, SAUTO), stesso protocollo della sezione P.

---

<a id="test-cgroup15-uso-comune-scroll-dieset-2026-07-29"></a>

## S — Test cgroup ~15% (6000/40000), uso comune + scroll Die Set (2026-07-29)

**Stato:** ⏳ **Dato raccolto, da interpretare con cautela** (vedi note sotto) · **Repo/Branch:** `pressbrakepeg` + `pegenstein` → `experiment/test-6-ch0-defer-corr-auto-sauto-estensione` · [← Tabella](#stato-test)

---

**Config:** cgroup `cpu.max = 6000 40000` (6 ms CPU / 40 ms → **~15%** medi)

**Scenario:** **non** martellamento pulsanti CH0 — uso "comune" dell'HMI, con una parte consistente di tempo passata a **scrollare/trascinare il disegno su Die Set** (CAD).

**⚠️ Note importanti per interpretare questo dato:**
- Questo branch **non ha le ottimizzazioni pan/scroll** (punti 1–3, vedi [sezione ottimizzazioni pan/scroll](#ottimizzazioni-pan-scroll)) — verificato assente `DrawPanIfDue` in `Sim2DView.cpp`, `Ppgviews.cpp::OnMove` chiama ancora `UpdateAllViews()` non throttled ad ogni movimento, `MatView.cpp::Paint()` chiama `TracciaGriglia()` sempre, senza skip in `m_bTracking`. Quello scrolling su Die Set qui è quindi il path **non ottimizzato**.
- L'estensione defer CH0 (CORR/AUTO/SAUTO, sezione R) probabilmente **non era comunque attiva** durante questo test, per il bug `PM_HIDE`/`m_nLastCompletedCH0State` descritto sopra — ma qui non era comunque lo scenario testato (niente martellamento pulsanti).

**Risultati RT** (`nanosleep`, thread `COM RTC Handler`):

| Metrica | Valore |
|---------|--------|
| Attivazioni | **529 000** |
| `nanosleep` max | **109 µs** |
| `nanosleep` min | **11 µs** |
| Valori **> 100 µs** | **4** |

**Distribuzione valori elevati (sotto soglia):**

| Intervallo (µs) | Occorrenze |
|------------------|----------:|
| 60–70 | 600 |
| 71–80 | 57 |
| 81–90 | 18 |
| 91–99 | 2 |

**Ultime 9 000 attivazioni:** `nanosleep` max **75 µs**

```text
*********************************************************
valore massimo della nanosleep delle  529000 attivazioni vale = 109
valore minimo della nanosleep delle ultime 529000 attivazioni vale = 11
i valori sopra ai 100 us nelle ultime529000 attivazioni sono = 4
*********************************************************

I valori nell'intervallo 60 70 sono = 600
I valori nell'intervallo 71 80 sono = 57
I valori nell'intervallo 81 90 sono = 18
I valori nell'intervallo 91 99 sono = 2
valore massimo della nanosleep delle ultime 9000 attivazioni vale = 75
*********************************************************
```

**Lettura:** 4 spike sopra soglia su 529k attivazioni (~0,0008%) con carico "uso comune" + molto scroll Die Set non ottimizzato, a cgroup ~15%. Confrontabile in ordine di grandezza con la campagna 4×30min del 2026-07-27 (baseline senza cgroup: 0 spill su 631k; cgroup `2000 20000`/10%: 1 spill su 280k) — qui il carico aggiuntivo di scroll CAD non ottimizzato non sembra spingere drammaticamente oltre soglia, ma **non è un confronto pulito** (branch, scenario e config diversi). Da ripetere dopo aver portato le ottimizzazioni pan/scroll su questo branch, per isolare l'effetto.

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
