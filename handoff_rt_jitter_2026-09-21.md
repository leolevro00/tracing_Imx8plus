# Handoff — jitter RT su HMI i.MX8M Plus

**Data:** 2026-09-21 · **Precedente:** `handoff_rt_jitter_2026-09-15.md` (superato)
**Scadenza imminente:** riunione aziendale **23/09** — punto della situazione con il titolare

---

## 0. Cosa è cambiato rispetto all'handoff precedente

Il 15/09 l'interferenza di memoria sulla L2 condivisa era **l'ipotesi che sopravviveva al vaglio**. Oggi è **dimostrata**, e il lavoro ha raggiunto il suo limite naturale sul percorso di display.

| | 15/09 | **21/09** |
|---|---|---|
| causa del jitter | ipotesi coerente coi dati | ✅ **dimostrata** (`stress_mem`, con controllo negativo) |
| confronto fra architetture | numeri di provenienza mista | ✅ **tre campagne allo stesso protocollo** |
| carico di prova | manuale, non ripetibile | ✅ **benchmark automatico**, tre carichi caratterizzati |
| obiettivo < 100 µs | raggiunto una volta, a mano | ✅ **raggiunto con una variabile d'ambiente** |
| dove agire dopo | sul trasporto dei pixel | ⚠️ **sulla rasterizzazione**: il trasporto non è più il limite |

---

## 1. ⚠️ LA COSA PIÙ URGENTE: tre repo non committati

**Settimane di lavoro esistono solo nel working tree.** Aperto dal 15/09, mai fatto.

| Repo | File | Branch |
|---|---|---|
| `pegenstein` | `PegLib.pro`, `pegdrmoutput.cpp` (+272), `peglvglwindow.cpp` | `experiment/test-6-ch0-defer-corr-auto-sauto-estensione` |
| `SqCom_Library` | `PerfMonitor.cpp/.h` (nuovi, +2 338), `RTCHndlr.cpp`, `SqCom.pro` | `lvgl-hmi` |
| `PlcEsa` | `Lnk/main.cpp` (+18) | `lvgl-hmi` |

**Piano dei commit pronto** nel diario `21.09.2026.md` §7, con i messaggi già scritti (Conventional Commits, inglese, ≤ 72 caratteri).

### Due trappole da conoscere prima di committare

⚠️ **`PegLib.pro` va committato SOLO in configurazione DRM.** Contiene gli interruttori dell'A/B: se lo committi mentre `EMBEDDED_HMI_RT_DRM_DIRECT` è commentato (configurazione SDL o Qt), registri «DRM disattivato» come stato permanente del branch. **Verificare la riga 12 prima di ogni commit di quel file.**

⚠️ **`peglvglwindow.cpp` contiene due modifiche non correlate**: il fix della build SDL (dichiarazioni `[AI-DIRTY]` fuori dalla guardia DRM) e la patch `[AI-NOSYNC]`. Per separarle: rimuovere temporaneamente la patch, committare il fix, ri-applicarla, committarla a parte.

⚠️ **`SqCom_Library` ha due copie identiche dell'header**: `SqCom/PerfMonitor.h` (in staging) e `Inc/PerfMonitor.h` (non tracciato). Il `.pro` dichiara `../Inc/PerfMonitor.h` e `Inc/` è la cartella tracciata degli header pubblici. **Decisione non presa.** Raccomandazione: tenere quello in `Inc/`.

---

## 2. Dove sta la documentazione — leggerla, non riscriverla

| Percorso | Contenuto |
|---|---|
| `…/perf_terminale/registro_test_rt.md` | Registro completo. **Sezione X** = confronto definitivo delle architetture; **sezione Y** = copie o rasterizzazione + disegno 2×2; **sezione 8 dell'ipotesi finale** = prova causale `stress_mem`; **sezione W** = costo reale delle copie. Le prime due voci della legenda puntano a X e Y |
| `Giornate_lavoro/2026/settembre/18.09.2026.md` | `stress_mem`, curva dose-risposta, analisi del meccanismo (§8) |
| `Giornate_lavoro/2026/settembre/21.09.2026.md` | **Due blocchi**: §1-8 chiusura del 18, §9-10 lavoro del 21 |
| `Giornate_lavoro/pressa/` | Documentazione tecnica per argomento: i tre path grafici, `carico_gui`, il passaggio SDL→DRM |
| `tracing_Imx8plus/carico_gui/` | Generatore di carico + `PROCEDURA.md` |
| `tracing_Imx8plus/stress_mem/` | Esperimento causale + `PROCEDURA.md` (⚠️ aggiornata: serve `PERF_ENABLE=1`) |
| `tracing_Imx8plus/patch_drm_no_sync.txt` | Patch `PEG_DRM_NO_SYNC`, **applicata** al sorgente |
| `tracing_Imx8plus/patch_qt_paint_stats.txt` | Contatore `[QT-PAINT]`, **non applicata** |

---

## 3. I risultati, in forma compatta

### Le tre architetture (benchmark vecchio, 2 h ciascuna, ~1,9 M attivazioni)

| | > 60 µs /M | > 100 µs /M | massimo |
|---|---:|---:|---:|
| Qt | 22 525 | **1 431** | 263 µs |
| SDL | 1 738 | **43,0** | 190 µs |
| DRM | 223 | **1,06** | 143 µs |
| **DRM + `present=33`** | **36,4** | **0** | **99 µs** |

🏆 **Obiettivo raggiunto**: zero sforamenti su 1 785 000 attivazioni, con **una sola variabile d'ambiente**, senza throttling e senza strumentazione — condizioni più severe del test finale di tirocinio.

### I tre carichi (benchmark nuovo, DRM, per milione)

| carico | > 60 µs | > 100 µs | massimo | specifica |
|---|---:|---:|---:|---|
| pulsanti e liste | 1 213 | **0** | 83 µs | ✅ |
| scroll grafico 2D | 1 913 | 33,3 | 128 µs | ❌ raramente |
| scroll grafico 3D | 10 111 | **227** | 139 µs | ❌ **51 volte in 15 min** |
| *nessuna interfaccia* | **0** | **0** | **53 µs** | pavimento del sistema |

### Il disegno 2×2 (architettura × carico) — la scoperta del 21/09

```
rapporto SDL/DRM per fascia:
  senza grafico 2D:   1,38 → 7,75 → 4,5  →  ∞  →  ∞      DIVERGE
  con grafico 2D:     2,75 → 4,51 → 5,42 → 3,89 → 2,39    RICADE
  con grafico 3D:     1,73 → 0,97 → 1,50 → 1,51 → 2,76    ~PIATTO
```

⇒ **Il DRM elimina la coda prodotta dal trasporto, non quella prodotta dalla rasterizzazione.** Sul 3D la fascia 71-80 è **indistinguibile** fra i due path (735 contro 759): a quel livello la sorgente è comune, ed è `libPegGL`, rasterizzatore **software**.

### La causa, dimostrata

`stress_mem`, quattro fasi su `Lnk` senza HMI:

| fase | banda | > 60 µs /M | massimo |
|---|---:|---:|---:|
| baseline | — | **0** | 53 µs |
| `l2fit` (384 KB, dentro la L2) | **21 261 MB/s** | 56 | 72 µs |
| `stream` (48 MB) | 6 505 MB/s | 883 268 | 134 µs |
| `thrash` (48 MB, banda bassa) | **1 013 MB/s** | 137 958 | 134 µs |

> **Il campione che muove più byte al secondo è quello che non produce jitter.** L'ordine delle bande è **l'inverso** di quello del danno.

E la curva dose-risposta ha il **flesso sulla dimensione della L2** (512 KB) entro mezzo punto percentuale, con la banda costante entro lo 0,7 % lungo tutto il fronte di salita.

---

## 4. ⚠️ Le trappole metodologiche imparate — la parte più trasferibile

**1. Il campione corto dà conclusioni opposte.** Lo stesso test, tre durate:

| campione | fascia 60-70 /M | conclusione |
|---:|---:|---|
| 59 000 (4 min) | 2 644 | *«il corpo peggiora del 58 %»* |
| 136 000 (9 min) | 1 596 | *«il corpo è identico»* |
| 225 000 (15 min) | 1 187 | *«il corpo è 1,41× più basso»* |

⇒ **Non trarre conclusioni prima della corsa piena.** È successo due volte in un giorno.

**2. Il massimo è una statistica d'ordine, non una soglia fisica.** Campagne di durata diversa **non hanno massimi confrontabili**. Il «tetto» del meccanismo esiste, ma il massimo *osservato* scende se scende l'intera distribuzione.

**3. La riproduzione a catena aperta vale solo a reattività costante.** Il test del throttling è **deragliato**: rallentando l'interfaccia, i tocchi sono caduti su elementi diversi e il benchmark si è bloccato su una tastiera. ⇒ Per ogni intervento che cambia la velocità di risposta serve un carico **robusto ai tempi**: soli pan e scroll, niente tap su pulsanti che aprono dialoghi.

**4. Verificare che la leva sia ingaggiata prima di investire ore.** Tre minuti di misura del tasso di present hanno evitato una campagna inutile (il tetto non mordeva). Per il throttling l'equivalente è `nr_throttled` in `cpu.stat`.

**5. Le previsioni quantitative derivate dal modello sbagliano spesso.** Il meccanismo (continuo contro impulsivo) ha retto ogni volta; le predizioni numeriche no — almeno cinque smentite registrate. **Scriverle comunque prima**: è ciò che rende interpretabile l'esito.

---

## 5. Cose aperte, in ordine di priorità

### Entro il 23/09

- [ ] **Committare i tre repo** (§1)
- [ ] **Finire la presentazione.** La slide «Confronto chiave» è stata rifatta su indicazione del prof, ma ⚠️ **le altezze delle barre non corrispondono ai valori** (130 disegnato a ~220, 98 a ~147): con l'asse graduato è visibile. Sostituire le forme disegnate a mano con un grafico vero. Refusi da correggere: `RGB656` → RGB565, `costum` → custom
- [ ] Struttura delle slide successive proposta nel diario del 21

### Misure aperte, a basso costo

- [ ] **Baseline lunga senza HMI** (≥ 1 h): i 53 µs vengono da 5 minuti, le barre da 2 ore. Non confrontabili. Basta non avviare `PegExec`
- [ ] **Durata di un ciclo del benchmark**: il valore stava ancora scendendo a 15 minuti, quindi le campagne potrebbero non contenere un numero intero di cicli
- [ ] `MALLOC_MMAP_THRESHOLD_=67108864` su Qt — test dell'ipotesi TLB/IPI, costo zero
- [ ] `stream` per 2 ore — porta i campioni alla pari con Qt e chiude la questione del massimo a 263 µs
- [ ] PMU a istruzioni costanti lungo la curva dose-risposta — **seconda figura della tesi, gratis**

### Progetto grande, mai iniziato

- [ ] **Strumentazione per separare rasterizzazione e trasporto.** Richiesta dettagliata formulata da Leonardo (8 punti). Metà esiste già (`RT_STATS`, `[AI-CATCHUP]`, `[AI-SYNC]`); il nuovo è **(A)** cronometrare la rasterizzazione PEG e **(§4)** correlare temporalmente con gli sforamenti RT.
  ⚠️ Tre avvertenze date: (a) misurare PEG richiede di **toccare i sorgenti PEG**, contro un vincolo posto da Leonardo stesso; (b) la riga «tempo GPU» della sua tabella è in gran parte **incompilabile** (sul DRM la GPU non è coinvolta); (c) la correlazione richiede di strumentare il **thread RT**, cioè proprio ciò che non si vuole perturbare

### Interventi valutati, con verdetto

| intervento | verdetto |
|---|---|
| `PEG_DRM_NO_SYNC` (togliere lo snapshot) | ❌ **provato: 33× peggio.** Lo snapshot è il percorso **veloce** (memcpy contigua); toglierlo sposta il lavoro su copie frammentate |
| `mergeDirtyRegion` da solo | ❌ depennato: il meccanismo predice zero effetto finché si resta sopra i 512 KB |
| scendere sotto i 512 KB/present | ⚠️ agirebbe sul **corpo**, non sulla coda. E il disegno di PEG sfonda comunque la cache |
| blitter hardware **G2D** | ⚠️ soluzione strutturale del trasporto (traffico in cache → **zero**), ma incognita sulla coerenza di cache, e non tocca la coda |
| **cache coloring** | ⭐ **la più promettente**: protegge il thread RT **indipendentemente dalla sorgente**. Dimensionamento noto: servono **~40-60 KB**, cioè **un colore su otto** |
| ridurre / spezzare il costo di disegno | ⭐ l'unica che agisce sulla coda dal lato sorgente. Codice applicativo (`pressbrakepeg`, `CSim2DView`, `cad2d`) |

⚠️ **Correzione al §9 del registro:** *«partizionare la cache (impossibile su A53, niente MPAM)»* confonde il partizionamento **hardware** (MPAM, assente) con il **cache coloring**, che è software e non richiede supporto architetturale. Da riscrivere.

---

## 6. Chiarimento su lvgl — chiuso il 21/09

**Il percorso chiamato «lvgl» non contiene lvgl.** Verificato nel codice:

- finestra, renderer e texture SDL creati **direttamente** (`peglvglwindow.cpp:525, 543, 570`)
- uniche chiamate lvgl in tutto il file: `lv_init`, `lv_deinit`, `lv_timer_handler`
- **nessun** `lv_display_create`, nessuna registrazione di display
- il buffer di disegno è una **`calloc()`** (`createSurface`, riga 1025): non lo fornisce né lvgl né SDL

⇒ Disegna **PEG**, trasporta **SDL** (o DRM). lvgl è linkata, inizializzata e **scavalcata**.

Confermato da Tommaso: *«in questa configurazione non le stiamo utilizzando come libreria»*.

**Rilevante per il futuro:** lvgl 9.1.0 ha driver nativi per Linux embedded — `lv_linux_drm.c` (DRM/KMS con **API atomica non bloccante**) e `lv_evdev.c`. Cioè **la stessa architettura costruita a mano**, in una versione che elimina anche l'attesa bloccante su `select()`.

⚠️ **Se la migrazione a lvgl passasse da SDL2, reintrodurrebbe il problema rimosso** (40× sugli sforamenti). Le misure dicono quale backend scegliere.

📌 `third_party/` è in **`.gitignore`**: lvgl 9.1.0 non è versionata, e il `.pro` va in `error()` se manca. Chi clona il repo **non compila**, e la versione richiesta non è scritta da nessuna parte.

---

## 7. Vincoli da rispettare

- **`ConfigButtonsStartStopPlusMinus`** (tasti fisici Start/Stop/Plus/Minus, da `AttivaMenu` per AUTO/SAUTO) deve restare **sempre immediata**. È sicurezza macchina.
- **Test brevi non sono validi.** Minimo un'ora per configurazione sui carichi leggeri; sui carichi pesanti 15-36 minuti bastano perché gli eventi sono frequenti.
- **`PerfMonitor` e `EMBEDDED_HMI_RT_STATS` spenti** nelle campagne: gonfiano il jitter (misurato).
- **`PEG_DRM_SYNC_STATS` ha default acceso**: metterlo a `0`.
- **`Lnk` va terminato con `SIGTERM`**, non `Ctrl+C`.
- **Confrontare a istruzioni costanti** quando si usano i contatori PMU.
- **`bus_cycles` è inutilizzabile**: è un orologio a metà frequenza CPU (dieci conferme indipendenti).
- **Salvare in `/root`, non in `/tmp`** (tmpfs).

---

## 8. Nota metodologica da conservare

Durante questa campagna sono state **ritirate molte conclusioni** a fronte di dati migliori — comprese diverse formulate in questa stessa settimana. Le rettifiche sono documentate **come tali**, con la misura che le smentisce accanto al testo superato.

**Mantenere questa pratica.** Un risultato ritirato con la misura che lo confuta vale più di una conclusione difesa. È anche ciò che ha permesso di scoprire che il campione corto dava conclusioni opposte, e di evitare due campagne inutili.
