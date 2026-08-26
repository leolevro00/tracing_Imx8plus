# Riduzione del jitter real-time in una HMI industriale

## Modifiche apportate al progetto — dal branch `lvgl-hmi` alla configurazione di test finale

**Piattaforma:** NXP i.MX8M Plus, 4× Cortex-A53 @ 1,6 GHz, Linux PREEMPT_RT
**Applicazione:** HMI di una pressa piegatrice — framework grafico PegLib, rasterizzazione software
**Periodo:** campagna di misura documentata in `registro_test_rt.md`

---

## 1. Obiettivo e risultato

### 1.1 Il problema

Il thread real-time `COM RTC Handler`, vincolato su CPU3, esegue un ciclo periodico da **4 ms** basato su `clock_nanosleep()`. La grandezza da controllare è il **ritardo di risveglio**: la differenza fra l'istante teorico di scadenza del timer e l'istante in cui il thread torna effettivamente in esecuzione.

$$\text{ritardo} = t_{\text{risveglio effettivo}} - t_{\text{scadenza timer}}$$

**Requisito:** ritardo costantemente **sotto i 100 µs**, anche durante l'interazione dell'operatore con l'interfaccia grafica.

Il problema osservato era che l'attività della GUI — scroll di grafici, cambi rapidi di pagina, visualizzatore 3D — produceva picchi di ritardo ben oltre quella soglia.

> ⚠️ **Precisazione importante sull'obiettivo.** Il lavoro **non** mirava ad aumentare il frame rate né a migliorare la fluidità percepita. Al contrario: diversi interventi **riducono deliberatamente** il numero di frame disegnati, accettando una grafica meno fluida in cambio di determinismo temporale. I due assi sono indipendenti, ed è stato verificato sperimentalmente.

### 1.2 Il risultato

| Configurazione | Ritardo massimo |
|---|---|
| Baseline (`lvgl-hmi`, uscita SDL) | **158 µs** |
| \+ uscita DRM diretta | 113 µs |
| \+ coalescing del pan a 33 ms | 103 µs |
| **\+ throttling e resto (configurazione finale)** | **98 µs** |

**Test di validazione finale:** 1 451 000 attivazioni consecutive (≈ 1 h 37 min) sotto interazione continua e deliberatamente aggressiva.

- ritardo massimo: **98 µs**
- attivazioni sopra i 100 µs: **0**
- 99,895 % delle attivazioni sotto i 60 µs

**Riduzione del caso peggiore: −38 %.** Requisito soddisfatto e verificato su un campione statisticamente significativo.

---

## 2. Metodo di lavoro

La parte metodologica è, a mio avviso, più significativa dell'elenco delle modifiche. Il lavoro è consistito in cicli ripetuti di:

```
misura  →  ipotesi sul meccanismo  →  intervento  →  rimisura  →  conferma o ritiro dell'ipotesi
```

con la regola di **non considerare valida un'ipotesi non chiusa da una misura**.

### 2.1 Strumentazione sviluppata

Per poter misurare è stato necessario costruire gli strumenti.

**`PerfMonitor`** (`SqCom/PerfMonitor.cpp`, `.h` — file nuovi). Legge i **contatori hardware del processore** (PMU) tramite `perf_event_open()`, **per ogni singola iterazione** del ciclo real-time su CPU3:

- `cpu_cycles`, `instructions` → CPI, cioè quanto la CPU sta stallando
- `l2d_cache`, `l2d_cache_refill` → accessi e miss della cache L2
- `bus_access`, `bus_cycles` → traffico verso la memoria

Il monitor traccia la **peggiore iterazione** per ritardo di risveglio, conservandone tutti i contatori, ed esporta l'intera distribuzione in CSV.

Un dettaglio non banale: è stata introdotta una **finestra di warm-up** (15 000 iterazioni, configurabile via `PERF_WARMUP_ITER`) perché le prime migliaia di iterazioni contengono i transitori di avvio e dominavano sistematicamente il massimo riportato, mascherando il comportamento a regime.

**Tracing con `ftrace`/`function_graph`.** Per osservare il percorso kernel attorno a uno spike è stata usata una tecnica di **cattura condizionata**: il tracing viene avviato all'inizio del test e **fermato solo quando il ritardo supera una soglia**, con un marker scritto in `trace_marker`. Il buffer circolare contiene così proprio l'evento raro di interesse. Documentato in `function_graph/trace_cmd_instrumentation.md`.

### 2.2 Un vincolo metodologico appreso a caro prezzo

Gli eventi sopra i 90 µs valgono circa **5 per milione**. Un test da 10 minuti contiene 150 000 attivazioni, quindi **statisticamente meno di un evento**. Un massimo di 60 µs misurato su dieci minuti **non dimostra nulla**.

Questo ha invalidato alcune conclusioni intermedie e ha portato ad adottare test da almeno un'ora per ogni configurazione.

---

## 3. Le modifiche al progetto

Sette interventi, in tre repository. Per ciascuno: cosa c'era, cosa c'è, perché, e la resa misurata.

### 3.1 Uscita video: da SDL a DRM diretto

**Repo:** `pegenstein` — `PegLib/PegLib.pro` (flag `EMBEDDED_HMI_RT_DRM_DIRECT`), `PegLib/pegdrmoutput.cpp/.h`

**Prima.** Il framebuffer di PegLib veniva copiato in una texture SDL, che SDL portava sullo schermo tramite la GPU. Il codice di `flushPresent()` era:

```cpp
SDL_RenderClear(m_renderer);
SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);   // nullptr,nullptr = TUTTO lo schermo
SDL_RenderPresent(m_renderer);                             // attesa BLOCCANTE sul vsync
```

**Dopo.** Scrittura diretta sui *dumb buffer* DRM, double buffering con `drmModePageFlip` **non bloccante** (`DRM_MODE_PAGE_FLIP_EVENT`), copia del solo rettangolo modificato (`blitDirtyRegion` / `copyRectFromSource`) con logica di *catch-up*, e nessun coinvolgimento della GPU.

#### Il conteggio effettivo, per frame presentato

| | SDL | DRM |
|---|---|---|
| Copia del rettangolo dirty | `SDL_UpdateTexture` | `blitDirtyRegion` |
| Pulizia a schermo intero | **1,2 MB scritti** | — |
| Blit a schermo intero | **1,2 MB letti + 1,2 MB scritti** (via GPU) | — |
| Snapshot a schermo intero | — | **1,2 MB letti + 1,2 MB scritti** (`syncBackFromPeg`) |
| **Traffico totale a schermo intero** | **3,6 MB** | **2,4 MB** |
| Conversione di formato | possibile RGB565 → ARGB8888 | nessuna |
| Present | **bloccante** sul vsync | non bloccante |
| GPU coinvolta | sì | **no** |

**−33 % di traffico**, che diventa **−60 %** se la conversione a ARGB8888 avveniva davvero (il codice contiene una diagnostica dedicata a rilevarla).

#### Il numero di buffer in memoria

| | SDL | DRM |
|---|---|---|
| Framebuffer PEG | 1,2 MB | 1,2 MB |
| Texture SDL (stadio intermedio) | 1,2-2,4 MB | **eliminata** |
| Buffer di scanout | 2-3 × 1,2 MB | 2 × 1,2 MB |
| **Totale** | **4-5 buffer, ~5-6 MB** | **3 buffer, 3,6 MB** |

I due *dumb buffer* sono verificabili sulla board: `grep dri /proc/<pid>/maps` mostra due mappature da `0x12C000` = 1 228 800 byte ciascuna.

#### Perché il ritardo è migliorato, in ordine di peso

1. **L'attesa bloccante sul vsync è sparita.** `SDL_RenderPresent` con vsync **blocca il thread** nel kernel fino alla scansione successiva: la GUI si addormentava e si risvegliava 60 volte al secondo, con context switch e attività di scheduler a ogni giro, e tutto il lavoro concentrato in una raffica dopo ogni vsync. Con `drmModePageFlip` + evento di completamento il thread **non si blocca mai**.
2. **La GPU è uscita di scena.** `SDL_RenderCopy` su kmsdrm/GLES2 comporta sottomissione di comandi, binding della texture, un quad texturizzato a schermo pieno, fence e interrupt: lavoro kernel aggiuntivo sui core 0-2 e traffico DDR generato dalla GPU. Con PegGL che è comunque un rasterizzatore **software**, la GPU serviva solo a fare una copia che la CPU poteva fare da sé.
3. **Un terzo di byte in meno attraverso la L2 condivisa**, cioè meno sfratti del working set del thread RT.
4. **Nessuna conversione di formato** e **un buffer in meno** in memoria.

**Resa:** sforamenti ridotti del **98 %**, massimo da **158 a 113 µs**.

> ⚠️ **Rettifica.** Una prima stesura di questa sezione affermava "due copie complete del framebuffer per frame → una". Non è esatto: **anche il percorso DRM esegue una copia a schermo intero** a ogni present (`syncBackFromPeg`). Il guadagno reale è −33 %, non −50 %. Vedi il §7.1 per la conseguenza.

### 3.2 PegGL: eliminata una riallocazione per frame

**Repo:** `pegenstein` — `PegGL/egl.cpp` (commit `4e93bb5`)

**Prima.** `peglSwapBuffers()` liberava e riallocava il bitmap nativo **a ogni frame**, vanificando la logica di riuso già presente in `renderToNative()`.

**Dopo.** Il buffer viene riutilizzato. Due righe rimosse.

Rilevante perché il visualizzatore 3D di questa HMI usa **PegGL, un'implementazione OpenGL ES interamente software**: la GPU non è mai coinvolta, quindi ogni operazione è lavoro di CPU e traffico di memoria — esattamente le risorse contese col thread real-time.

### 3.3 Coalescing del ridisegno durante il pan: 16 → 33 ms

**Repo:** `pressbrakepeg` — `sim2d/Sim2DView.cpp`, `cad2d/Ppgviews.cpp` (commit `abbffb7`)

**Prima.** Durante il trascinamento, `Invalidate + Draw` al massimo ogni 16 ms (~60 fps).

**Dopo.** Al massimo ogni 33 ms (~30 fps). L'origine del pan continua ad aggiornarsi a ogni evento di tocco: cambia solo la frequenza di **ridisegno**, non la reattività al gesto.

**Perché.** Ogni frame di pan rasterizza in software sette poligoni pieni (matrice, punzone, superiore, inferiore, pezzo, riscontro, collisioni) più lo sfondo. Dimezzare i frame dimezza quel lavoro.

**Resa sul grafico 2D**, che era il peggior punto critico: sforamenti da **47,1 a 4,4 per milione** di iterazioni, massimo da **115 a 103 µs**.

**Compromesso consapevole:** fluidità grafica contro determinismo. Reversibile rimettendo 16.

### 3.4 Differimento del lavoro pesante al cambio pagina (defer CH0)

**Repo:** `pressbrakepeg` — `editorprogrammi/MDINum.cpp`, `editorprogrammi/PpgView.cpp`

**Prima.** Ogni pressione di un pulsante di cambio pagina scatenava immediatamente la ricostruzione completa delle strutture CH0. Premendo rapidamente più pulsanti in sequenza, la ricostruzione veniva eseguita **per intero a ogni pressione**.

**Dopo.** Il lavoro pesante viene **schedulato** dopo `CH0_DEFER_DELAY_MS = 300`. Pressioni successive **riprogrammano** il timer invece di accodare lavoro: N pressioni rapide producono **una sola** ricostruzione, quella finale.

Macchina a stati: `m_nPendingCH0State`, `m_nLastCompletedCH0State`, `m_bCH0WorkPending`, `m_bCH0Completing`. Esteso agli stati IMP, MAN, CORR, AUTO, SAUTO.

> **Vincolo di sicurezza rispettato.** La configurazione dei pulsanti fisici Start / Stop / Plus / Minus (`ConfigButtonsStartStopPlusMinus`, invocata dentro `AttivaMenu` per AUTO e SAUTO) resta **sempre immediata** e non viene mai differita. Differirla avrebbe introdotto una finestra in cui i comandi fisici della macchina non sarebbero stati correttamente mappati — inaccettabile su una pressa.

### 3.5 Tracce diagnostiche compilate fuori dal binario

**Repo:** `pressbrakepeg` — `liste/liste_diag.h` (commit `c74153c`)

**Prima.** `ListeDiag()` — invocata da **30 punti** del codice — scriveva su `stderr` a ogni operazione sulle liste, anche in esercizio normale.

**Dopo.** Corpo racchiuso in una guardia `LISTE_DIAG_ENABLED`, non definita per default: le chiamate si riducono a nulla e il compilatore le elimina. Riattivabile definendo la macro.

### 3.6 Limitazione della banda di CPU della GUI (cgroup v2)

**Configurazione a runtime**, script `peg_cgroup_throttle.sh` — non risiede nel codice sorgente

```
/sys/fs/cgroup/peg_gui_rt
    cpu.max      = 1000 6666    # quota 1000 µs su periodo 6666 µs = 15 %
    cpuset.cpus  = 0-2          # la GUI non viene mai schedulata su CPU3
```

Il processo dell'HMI viene confinato in un cgroup con quota di CPU limitata al **15 %** e vincolato ai core 0-2.

**Dettaglio tecnico:** il kernel **rifiuta** quote o periodi inferiori a 1 ms (`write error: Invalid argument`) — lo scheduler CFS impone quel minimo. La configurazione iniziale `600 4000`, che avrebbe dato una granularità più fine, non è realizzabile: da qui `1000 6666`, che mantiene lo stesso rapporto del 15 % con valori ammessi.

**Osservazione dai dati finali** (`cpu.stat`): su 1 599 726 periodi, **391 766 sono stati troncati (24,5 %)**, mentre il consumo medio è stato del **10 % di un core** contro un tetto del 15 %.

La GUI non è pesante *in media*: è **a raffiche**. Sta ferma a lungo, poi chiede tutto quello che può in pochi millisecondi. Il throttling è efficace proprio perché **tronca le raffiche**, non perché riduce il lavoro complessivo.

### 3.7 PerfMonitor sul thread real-time

**Repo:** `SqCom_Library` — `SqCom/PerfMonitor.cpp`, `SqCom/PerfMonitor.h` (nuovi), `SqCom/RTCHndlr.cpp`, `SqCom/SqCom.pro`
**Repo:** `PlcEsa` — `Lnk/main.cpp`

Strumento di misura descritto in §2.1. Alla terminazione (`SIGTERM`) il processo stampa le statistiche finali e del caso peggiore ed esporta `/tmp/perf_rt.csv` e `/tmp/perf_rt_worst.csv`.

> **Punto aperto onesto:** `PerfMonitor_Init()` è attualmente incondizionata, quindi anche una build di produzione apre i contatori e verifica `PerfMonitor_IsEnabled()` a ogni iterazione real-time. Il costo è piccolo ma non nullo, e cade proprio nel percorso che si vuole proteggere. Andrebbe messa dietro un flag di compilazione.

---

## 4. Il percorso di indagine

La parte più lunga del lavoro non è stata scrivere le modifiche, ma capire **da dove venisse** il jitter residuo. Nove ipotesi sono state formulate e **chiuse ciascuna da una misura**.

| # | Ipotesi | Misura | Esito |
|---|---|---|---|
| 1 | Saturazione della banda DDR | `perf stat -I 10`, riposo vs carico | Esclusa: picchi 7,8 % vs 9,1 % |
| 2 | Contesa di latenza sul bus | analisi su 10 000 iterazioni | **Ritirata**: fondata su 2 campioni e su una lettura errata di `bus_cycles` |
| 3 | Processi utente concorrenti su CPU3 | `function_graph` attorno a uno spike | Esclusa: nella finestra critica solo i thread RT e il kernel |
| 4 | Stati di idle profondi | `cpuidle` latency e usage | Esclusa: solo `WFI` (uscita 1 µs); `cpu-pd-wait` usato 573 volte su 9,7 M |
| 5 | Costo del ciclo idle/wakeup | PM QoS `/dev/cpu_dma_latency` = 0 | Esclusa: iterazioni > 25 µs da 2 092 a 2 040 (−2,5 %) |
| 6 | Scaling di frequenza (DVFS) | `scaling_governor` | Esclusa: già `performance` |
| 7 | Throttling termico | trip point | Esclusa: 50 °C contro soglia a 95 °C |
| 8 | Bilanciamento del carico | `/sys/devices/system/cpu/isolated` | Esclusa: `isolcpus=3` già attivo |
| 9 | Tick residuo dello scheduler | `/proc/interrupts`, `arch_timer` | Parziale: ~420 tick/s su CPU3, ma non spiega i dati |

### 4.1 La misura decisiva

La chiave è stata confrontare due iterazioni **a parità di istruzioni eseguite**. Due campioni della stessa esecuzione:

| | #2 (62 µs) | #3 (92 µs) | Δ |
|---|---|---|---|
| **istruzioni** | 567 692 | 567 603 | **−89 → −0,016 %** |
| accessi L2 | 16 806 | 16 718 | −0,52 % |
| **refill L2** | 4 517 | 5 202 | **+15,2 %** |
| **cicli CPU** | 1 129 283 | 1 274 788 | **+12,9 %** |
| **ritardo** | 62 µs | 92 µs | **+48 %** |

Ottantanove istruzioni di differenza su 567 692: **lo stesso identico percorso di codice**. Cambia solo quante volte quel codice ha mancato la cache.

**Stesse istruzioni + più cicli = cicli di stallo.** E il costo per miss:

$$\frac{145\,505\ \text{cicli}}{685\ \text{refill}} = 212\ \text{cicli/miss}$$

Sulle distribuzioni complete da 10 000 iterazioni, lo stesso calcolo dà **110-120 cicli per miss su tre dataset indipendenti** — a 1,6 GHz sono **69-75 ns**, cioè la latenza di un accesso alla DRAM. I cicli in più **sono** la penalità delle miss, e il conto lo dimostra numericamente.

### 4.2 Il meccanismo individuato

Il fatto strutturale che era sfuggito:

> **`isolcpus=3` isola lo scheduler, non la cache.**

I quattro Cortex-A53 dell'i.MX8M Plus stanno in un **unico cluster con L2 condivisa**. I core 0-2 che disegnano pixel scrivono nella stessa L2 in cui vive il working set del thread RT su CPU3. Nessun parametro del kernel può separarli: è hardware, e l'A53 non dispone di MPAM né di partizionamento della cache.

**L'aritmetica**, dalle mappature DRM del processo HMI: due buffer da `0x12C000` = 1 228 800 byte, cioè **1024 × 600 a 16 bit = 1,2 MB per buffer**.

| Grandezza | Valore | Rapporto con la L2 (512 KB) |
|---|---|---|
| Un buffer a schermo intero | 1,2 MB | **2,4×** |
| Una copia completa (letto + scritto) | 2,4 MB | **~5×** |

**Dopo una copia a schermo intero, in L2 non sopravvive nulla di quanto c'era prima.**

### 4.3 La verifica quantitativa

Con il coalescing a 33 ms si hanno ~**30 ridisegni al secondo**; il thread RT si sveglia **250 volte al secondo**:

$$\frac{30}{250} = 12\ \%$$

Circa **un risveglio su otto** cade subito dopo una cancellazione della cache. Confronto con la misura:

| | Iterazioni > 25 µs |
|---|---|
| A riposo | 12,9 % |
| Sotto carico GUI | 20,9 % |
| **Aumento** | **+8,0 punti** |

**8 punti misurati contro 12 previsti** da un conto che usa soltanto risoluzione dello schermo, dimensione della cache e periodi dei due processi. Nessun parametro adattato ai dati. La differenza va nella direzione attesa, perché non tutti i frame sono a schermo pieno.

È una **predizione quantitativa indipendente verificata**, ed è l'argomento più solido a sostegno del meccanismo.

### 4.4 Perché le misure di banda scagionavano la memoria — e avevano ragione

$$30\ \text{blit/s} \times 2{,}4\ \text{MB} \approx 72\ \text{MB/s}$$

Su DDR4 sono briciole. **La banda davvero non c'entra.** È traffico irrisorio che ha un effetto devastante sulla cache, perché ogni singola passata è più grande della cache stessa.

Questa distinzione — **volume di traffico** contro **effetto sulla cache** — è la ragione per cui l'indagine ha impiegato tanto a convergere: si stava misurando la grandezza sbagliata.

---

## 5. Errori commessi e corretti

Li riporto perché fanno parte del metodo, e perché una conclusione ritirata a fronte di dati migliori vale più di una conclusione difesa.

| Errore | Come è emerso |
|---|---|
| Attribuito il jitter a un `PM_HIDE` auto-indotto | Aggiunta la protezione, sintomo invariato → ipotesi sbagliata |
| Letto `BUS_CYCLES` come costo per transazione (**"+57 %"**) | È un contatore di **clock**, non di occupazione: `bus_cycles/cpu_cycles` vale 0,5013-0,5026 su **quattro** campioni indipendenti, cioè è un orologio a metà frequenza. Metrica inutilizzabile, conclusione ritirata |
| Dichiarato "meccanismo risolto" da 2 campioni | L'analisi su 10 000 iterazioni ha dato r ≤ 0,42, r² ≤ 17 %: sezione rititolata "indagato, NON risolto" |
| Confrontati CPI fra campioni con istruzioni diverse | Il CPI è un rapporto: con denominatori che differiscono di 2,9× è privo di significato. Regola adottata: **confrontare sempre a istruzioni costanti** |
| **Escluso l'interferenza di memoria** | L'esclusione si basava su misure di banda (grandezza sbagliata), su una fascia con n = 41 e su due campioni singoli. Il conteggio delle **istruzioni**, che avevo sotto gli occhi, la smentiva |

L'ultimo è il più significativo: l'ipotesi corretta era stata scartata per un'analisi statistica applicata alla variabile sbagliata.

---

## 6. Risultato finale in dettaglio

**1 451 000 attivazioni, ≈ 1 h 37 min, interazione continua.**

| Fascia | Occorrenze | Per milione |
|---|---|---|
| 60-70 µs | 1 336 | 920,7 |
| 71-80 µs | 152 | 104,8 |
| 81-90 µs | 22 | 15,2 |
| 91-99 µs | 7 | 4,8 |
| **≥ 100 µs** | **0** | **0** |

Massimo 98 µs, minimo 11 µs. La coda decade rapidamente: ogni fascia da 10 µs vale circa un settimo della precedente.

**Cosa è dimostrato:** la combinazione di interventi porta il caso peggiore sotto i 100 µs e ce lo mantiene per 1,45 milioni di attivazioni consecutive sotto interazione reale, in modo riproducibile e con modifiche tutte documentate e reversibili.

**Cosa non è dimostrato:** che 98 µs sia un limite *garantito*. È il massimo osservato su ~1,6 ore. Per un requisito hard real-time servirebbe una campagna molto più lunga e un'analisi del caso peggiore teorico, non solo statistica.

---

## 7. Lavoro aperto

In ordine di rapporto fra beneficio atteso e sforzo:

### 7.1 🥇 Rendere condizionale lo snapshot a schermo intero prima del page flip

**È l'intervento con il rapporto beneficio/sforzo più alto rimasto, e colpisce esattamente il meccanismo individuato.**

**Il problema.** Il double buffering fa sì che ogni buffer, mentre è a video, "perda" gli aggiornamenti applicati all'altro. Esistono due modi di rimediare, e **il codice li applica entrambi**:

| Approccio | Dove | Costo |
|---|---|---|
| Tenere traccia della zona persa e recuperare solo quella | `blitDirtyRegion` (`m_staleDamage[]`, `m_bufferNeedsFullCopy[]`) | proporzionale al danno reale |
| Ricopiare l'intero schermo prima del flip | `syncBackFromPeg` in `flushPresent()` | **sempre 2,4 MB** |

Il secondo viene eseguito **dopo** il primo e lo **sovrascrive integralmente**: tutto il lavoro di tracciamento del danno viene buttato via.

**L'ordine di grandezza.** Muovendo uno slider che cambia 50×20 pixel:

| | Byte copiati |
|---|---|
| `blitDirtyRegion` — utile | 50 × 20 × 2 = **2 000** |
| `syncBackFromPeg` — ridondante | 1024 × 600 × 2 = **1 228 800** |

**Circa 600 volte più del necessario.** E il conto quadra con la misura: 2,4 MB × 30 present/s = **72 MB/s**, esattamente il valore del §4.4 — che quindi proviene **tutto** da `syncBackFromPeg`, non dai rettangoli dirty.

**La correzione è di due righe**, e la funzione che serve **esiste già ma non è mai chiamata da nessuna parte** (`PegDrmOutput::needsFullSyncBeforeFlip()`, `pegdrmoutput.cpp:537` — codice morto):

```cpp
// peglvglwindow.cpp, flushPresent()
if (m_framebuffer && m_drmOutput->needsFullSyncBeforeFlip())
{
    PegFrameBufferLock lock;
    (void)m_drmOutput->syncBackFromPeg(m_framebuffer, framePitchBytes());
}
```

⚠️ **Non è automaticamente sicura.** La copia incondizionata è una rete di sicurezza: se la logica di *catch-up* avesse un difetto, la ricopiatura totale lo nasconde. Il commento nel codice lo dice esplicitamente (*"evita artefatti da back buffer parziale"*). La sequenza corretta è: attivare la condizione → **verificare visivamente** durante scroll, cambi pagina e visualizzatore 3D → solo se non compaiono artefatti, misurare il guadagno RT con una sessione da un'ora.

### 7.2 Gli altri interventi

1. **Verificare `kFullSyncDamageRatioPercent`** — contare quanto spesso il blit ricade sul ramo a schermo intero invece che sul rettangolo modificato. Se durante lo scroll del grafico 2D la soglia viene superata sistematicamente, alzarla è **una riga di codice**. Costo nullo, beneficio potenzialmente alto. *(Diventa rilevante solo dopo il §7.1: finché lo snapshot integrale è incondizionato, questa soglia non cambia nulla.)*

2. **Eliminare le 3 copie ridondanti nel percorso 3D** — ognuna vale 1,2 MB, cioè quanto l'intero passaggio da SDL a DRM.

3. **Togliere `Posiziona` dal percorso di disegno** della pagina Manual Sequence: misurato a **832 µs di CPU, il 49,8 %** del costo di un ridisegno, per un'operazione che non disegna.

4. **Prova causale definitiva** — programma `stress_mem` (già scritto, in `tracing_Imx8plus/stress_mem/`): genera traffico di memoria controllato su CPU0-2 **senza alcuna GUI**, in tre modalità che separano *banda* da *sfratto della cache*. Se riproduce il jitter, la catena causale è chiusa in modo incontrovertibile.

5. **Blitter 2D hardware (G2D)** — intervento strutturale: porta i pixel in DRAM via DMA senza passare dalle cache dei core.

**Vie da non riprovare**, ciascuna con la misura che le esclude: partizionamento della cache (impossibile su A53), `nohz_full` (guadagno limitato e non è il meccanismo), PM QoS (testato, nessun effetto), riduzione ulteriore della quota cgroup (migliora la media, non il massimo), forzatura della frequenza (già a `performance`), raffreddamento (45 °C di margine).

---

## 8. Riferimenti

| Documento | Contenuto |
|---|---|
| `registro_test_rt.md` | Registro completo della campagna: ogni test, ogni misura, ogni ipotesi con esito |
| `function_graph/trace_cmd_instrumentation.md` | Metodo di tracing a cattura condizionata |
| `stress_mem/PROCEDURA.md` | Esperimento di verifica causale, con matrice di interpretazione dei risultati |

**Tracciabilità dei branch e dei commit** corrispondenti al test finale: sezione *Test finale di tirocinio → Tracciabilità* del registro.
