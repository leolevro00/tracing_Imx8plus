# Analisi delle metriche grafiche e memoria della GUI PEG/SDL/LVGL

## 1. Scopo del documento

Questo documento raccoglie e spiega in modo ordinato le metriche usate per analizzare l’impatto dell’interfaccia grafica sul sistema real-time.

L’obiettivo non è soltanto sapere “quanta CPU usa la GUI”, ma capire **quanti dati vengono mossi dalla pipeline grafica**, quanto tempo viene speso negli aggiornamenti della texture e se l’interazione touch provoca traffico memoria/cache/bus sufficiente a interferire con i task real-time.

La pipeline osservata è, in forma semplificata:

```text
Touch / mouse
    ↓
Eventi SDL
    ↓
PEG aggiorna lo stato dei widget
    ↓
PEG ridisegna nel framebuffer software
    ↓
dirty region
    ↓
uploadDirtyRegion()
    ↓
SDL_UpdateTexture()
    ↓
SDL_RenderCopy() / SDL_RenderPresent()
    ↓
DRM/KMS / display
```

Il punto misurato più importante è `uploadDirtyRegion()`, perché lì viene copiata una porzione del framebuffer software verso la texture SDL.

---

## 2. Concetti base: framebuffer, texture e dirty region

### 2.1 Framebuffer software

Il framebuffer software è una zona di RAM in cui PEG disegna l’immagine della schermata.

Concettualmente è una matrice di pixel:

```text
larghezza × altezza
```

Nel caso analizzato, la risoluzione ricavata dai dati è molto probabilmente:

```text
960 × 640 = 614400 pixel
```

Se il formato è RGB565, cioè 16 bit per pixel:

```text
16 bit/pixel = 2 byte/pixel
```

quindi un frame completo pesa:

```text
960 × 640 × 2 = 1,228,800 byte ≈ 1.17 MiB
```

Quando viene aggiornato tutto lo schermo, quindi, si sta copiando circa 1.17 MiB per singolo upload.

---

### 2.2 Texture SDL

La texture SDL è l’oggetto grafico usato da SDL per portare i pixel verso il renderer e poi verso il display.

Il framebuffer software contiene i pixel prodotti da PEG. La texture SDL è invece la rappresentazione usata dalla pipeline grafica SDL/driver.

Per aggiornare la texture viene chiamata una funzione del tipo:

```cpp
SDL_UpdateTexture(m_texture, &rect, src, pitch);
```

Questa chiamata copia i pixel dal framebuffer software alla texture SDL.

---

### 2.3 Dirty region

Una dirty region è una zona dello schermo che è cambiata e che quindi deve essere aggiornata.

Se cambia solo un bottone, idealmente si dovrebbe aggiornare solo il rettangolo del bottone. Se invece il sistema invalida una zona troppo grande, si finisce per copiare molti più pixel del necessario.

La dimensione in pixel della dirty region è:

```text
dirty_area_px = rect.w × rect.h
```

La dimensione in byte dell’upload è:

```text
upload_bytes = rect.w × rect.h × bytesPerPixel
```

Con RGB565:

```text
upload_bytes = rect.w × rect.h × 2
```

---

## 3. Significato delle metriche di `uploadDirtyRegion()`

Nei log hai righe di questo tipo:

```text
[RT] uploadDirtyRegion: calls=20 req=8.05MB reqMBps=7.71 updateMs=97.835 effMBps=82.3 maxRectPx=614400
```

Ogni riga rappresenta una finestra temporale di circa un secondo.

---

## 4. `calls`

### Significato

`calls` indica quante volte, nella finestra di misura, è stata chiamata `uploadDirtyRegion()`.

Esempio:

```text
calls=20
```

significa:

```text
in circa un secondo sono stati eseguiti 20 upload verso la texture SDL
```

### Interpretazione

`calls` misura la frequenza degli aggiornamenti grafici.

Valori tipici osservati:

```text
riposo:       7–9 calls/s
interazione: 14–20 calls/s
```

Quindi, quando premi o trascini sull’interfaccia, PEG/SDL produce più upload.

### Cosa NON dice da sola

`calls` non dice quanto grandi siano gli upload.

Due casi molto diversi possono avere lo stesso numero di chiamate:

```text
20 upload piccoli  → poco traffico dati, ma molto overhead
20 upload enormi   → molto traffico dati e possibile pressione su DDR/bus
```

Per questo `calls` va sempre letta insieme a:

```text
reqMBps
updateMs
maxRectPx
```

---

## 5. `req`

### Significato

`req` indica il volume totale di dati richiesto nella finestra di misura.

Esempio:

```text
req=8.05MB
```

significa:

```text
in quell’intervallo sono stati copiati complessivamente circa 8.05 MB di pixel
```

La formula è:

```text
req = somma_di_tutti_gli_upload_bytes_nella_finestra
```

dove:

```text
upload_bytes = rect.w × rect.h × bytesPerPixel
```

### Esempio a riposo

```text
calls=7 req=0.75MB
```

significa:

```text
7 upload nel secondo
0.75 MB totali copiati
```

### Esempio durante touch

```text
calls=20 req=8.05MB
```

significa:

```text
20 upload nel secondo
8.05 MB totali copiati
```

Quindi durante l’interazione vengono mossi molti più dati.

---

## 6. `reqMBps`

### Significato

`reqMBps` significa:

```text
requested megabytes per second
```

cioè:

```text
quanti MB al secondo vengono richiesti alla pipeline di upload
```

Formula:

```text
reqMBps = req_MB / elapsed_seconds
```

Se la finestra dura esattamente 1 secondo, `req` e `reqMBps` sono quasi uguali.

Se la finestra dura più di 1 secondo, possono differire.

---

### Perché `req=8.05MB` e `reqMBps=7.71` non sono uguali?

Esempio reale:

```text
req=8.05MB
reqMBps=7.71
```

Questo succede perché la finestra di misura non dura esattamente 1.000 secondi.

Se la finestra reale dura circa 1.044 secondi:

```text
reqMBps = 8.05 / 1.044 ≈ 7.71 MB/s
```

Quindi:

```text
req      = totale copiato nella finestra
reqMBps = totale copiato normalizzato al secondo
```

Analogia:

```text
req      = in questo viaggio ho trasportato 8.05 kg
reqMBps = in media ho trasportato 7.71 kg al secondo
```

---

### Perché è importante per il real-time?

`reqMBps` è una delle metriche più utili per capire quanta banda memoria viene richiesta dalla GUI.

Valori osservati:

```text
riposo:       circa 0.75–1 MB/s
interazione: circa 5–8 MB/s
```

Quindi durante il touch la GUI può richiedere anche 5–10 volte più traffico grafico rispetto al riposo.

Questo è coerente con l’aumento osservato nei contatori:

```text
l2d_cache_wb
bus_access
mem_access
imx8_ddr0/write-accesses
```

---

## 7. `updateMs`

### Significato

`updateMs` indica il tempo totale passato dentro `SDL_UpdateTexture()` durante la finestra di misura.

Esempio:

```text
updateMs=97.835
```

significa:

```text
in circa un secondo, 97.835 ms sono stati spesi dentro SDL_UpdateTexture()
```

In termini percentuali:

```text
97.835 ms / 1000 ms ≈ 9.8%
```

Quindi quasi il 10% del tempo di un core equivalente è stato occupato dall’upload texture.

---

### Perché è importante?

Per un sistema real-time, `updateMs` è fondamentale perché misura un costo temporale concreto.

Se la GUI passa molto tempo dentro `SDL_UpdateTexture()`, può interferire in vari modi:

```text
- consuma tempo CPU
- genera pressione sulla cache
- genera traffico verso bus/DDR
- può attivare driver grafici
- può aumentare la latenza percepita da altri task se condividono core, cache, DDR o interconnect
```

Valori osservati:

```text
riposo:       circa 22–30 ms/s
interazione: circa 60–97 ms/s
```

Quindi durante il touch il tempo speso negli upload cresce molto.

---

## 8. `effMBps`

### Significato

`effMBps` significa:

```text
effective megabytes per second
```

cioè:

```text
a che velocità media SDL_UpdateTexture() ha copiato i dati mentre era effettivamente in esecuzione
```

Formula:

```text
effMBps = req_MB / (updateMs / 1000)
```

Esempio:

```text
req=8.05MB
updateMs=97.835
```

quindi:

```text
effMBps = 8.05 / 0.097835 ≈ 82.3 MB/s
```

---

### Differenza tra `reqMBps` ed `effMBps`

Questa è la distinzione più importante.

`reqMBps` guarda tutto il secondo:

```text
quanti MB/s sto chiedendo alla pipeline grafica?
```

`effMBps` guarda solo il tempo passato dentro `SDL_UpdateTexture()`:

```text
quando SDL_UpdateTexture lavora, a che velocità copia?
```

Esempio:

```text
calls=20 req=8.05MB reqMBps=7.71 updateMs=97.835 effMBps=82.3
```

Significa:

```text
nel secondo reale ho chiesto circa 7.71 MB/s
ma SDL_UpdateTexture ha lavorato solo per circa 98 ms
durante quei 98 ms ha copiato a circa 82.3 MB/s
```

---

### Analogia semplice

Immagina un operaio che in un’ora lavora solo 10 minuti.

Durante quei 10 minuti sposta 100 kg.

Media sull’ora intera:

```text
100 kg / 1 ora = 100 kg/ora
```

Velocità mentre lavora davvero:

```text
100 kg / 10 minuti = 600 kg/ora
```

Nel nostro caso:

```text
reqMBps = media sul tempo totale
effMBps = velocità solo mentre SDL_UpdateTexture lavora
```

---

### Perché `effMBps` può salire quando il sistema peggiora?

Nei log si vede che a riposo:

```text
reqMBps ≈ 0.75–1 MB/s
effMBps ≈ 33–36 MB/s
```

Durante il touch forte:

```text
reqMBps ≈ 5–8 MB/s
effMBps ≈ 80–96 MB/s
```

Questo non significa che il sistema stia meglio.

Significa che durante il touch vengono copiati rettangoli più grandi.

Le copie grandi sono spesso più efficienti per byte, perché il costo fisso della chiamata viene distribuito su più dati.

Però, anche se la copia è più efficiente, il sistema peggiora perché:

```text
- vengono copiati molti più MB totali
- updateMs aumenta molto
- maxRectPx arriva spesso al full-screen
- aumenta la pressione su cache, bus e DDR
```

Quindi `effMBps` alto non è necessariamente positivo.

---

### Quando `effMBps` basso è un problema?

Esempio reale:

```text
calls=17 req=0.98MB updateMs=53.587 effMBps=18.4 maxRectPx=71504
```

Qui hai molte chiamate ma pochi dati totali.

Questo suggerisce:

```text
tanti upload piccoli
```

In questo caso la copia non è efficiente perché ogni upload ha overhead:

```text
- chiamata SDL
- controllo parametri
- gestione texture
- eventuali lock interni
- driver
- calcolo del rettangolo
```

Quindi `effMBps` basso può indicare frammentazione o overhead alto.

---

## 9. `maxRectPx`

### Significato

`maxRectPx` è l’area in pixel della dirty region più grande vista in quella finestra di misura.

Formula:

```text
maxRectPx = max(rect.w × rect.h)
```

Esempio:

```text
maxRectPx=614400
```

Se la risoluzione è 960×640:

```text
960 × 640 = 614400 pixel
```

quindi:

```text
maxRectPx=614400 = full screen
```

---

### Percentuale dello schermo

Assumendo risoluzione 960×640:

| maxRectPx | Percentuale schermo | Interpretazione |
|---:|---:|---|
| 66,912 | circa 10.9% | zona medio-piccola |
| 71,504 | circa 11.6% | zona medio-piccola |
| 74,784 | circa 12.2% | zona medio-piccola |
| 76,096 | circa 12.4% | zona medio-piccola |
| 94,962 | circa 15.5% | zona media |
| 332,304 | circa 54.1% | più di metà schermo |
| 370,688 | circa 60.3% | zona molto grande |
| 614,400 | 100% | schermo intero |

---

### Perché è il segnale più importante?

Quando `maxRectPx` arriva a 614400, significa che almeno una volta in quel secondo è stata caricata una dirty region grande quanto tutto lo schermo.

A 16 bpp:

```text
614400 × 2 = 1,228,800 byte ≈ 1.17 MiB
```

Quindi un singolo upload full-screen costa circa 1.17 MiB.

Se questo succede molte volte al secondo, il traffico cresce rapidamente.

---

### Attenzione: `maxRectPx` è massimo, non media

Una riga come:

```text
calls=20 req=8.05MB maxRectPx=614400
```

non significa che tutte le 20 chiamate siano full-screen.

Significa:

```text
almeno una delle 20 chiamate è stata full-screen
```

Se tutte e 20 fossero full-screen, il totale sarebbe circa:

```text
20 × 1.17 MiB ≈ 23.4 MiB
```

Nel log invece il totale è circa 8.05 MB, quindi probabilmente ci sono:

```text
- alcuni upload molto grandi
- altri upload più piccoli
```

---

## 10. Lettura dei due regimi nei log

Dai dati emergono due regimi molto chiari.

---

### 10.1 Regime A: schermo quasi fermo / attività leggera

Esempi:

```text
calls=7  reqMBps=0.75  updateMs=22   maxRectPx=66912
calls=9  reqMBps=0.98  updateMs=28   maxRectPx=66912
```

Interpretazione:

```text
- 7–9 upload al secondo anche a riposo
- circa 0.75–1 MB/s di dati grafici copiati
- circa 22–30 ms/s spesi in SDL_UpdateTexture
- dirty region massima circa 10–15% dello schermo
```

Conclusione:

```text
anche a riposo la GUI non è completamente ferma
```

Probabili cause:

```text
- timer grafici
- cursori
- lampeggi
- barra di stato
- aggiornamenti periodici
- componenti PEG che ridisegnano periodicamente
```

---

### 10.2 Regime B: pressione / trascinamento / interazione forte

Esempi:

```text
calls=19  reqMBps=7.80  updateMs=90   maxRectPx=614400
calls=20  reqMBps=8.05  updateMs=97   maxRectPx=614400
```

Interpretazione:

```text
- 14–20 upload al secondo
- circa 5–8 MB/s di dati grafici copiati
- fino a 60–97 ms/s dentro SDL_UpdateTexture
- dirty region massima full-screen
```

Conclusione:

```text
durante il touch la GUI produce più upload e soprattutto upload molto più grandi
```

Questa è la combinazione più critica:

```text
calls alte + reqMBps alto + updateMs alto + maxRectPx full-screen
```

---

## 11. Perché quando premi aumentano le `calls`

La catena tipica è:

```text
touch down / touch motion / touch up
    ↓
evento SDL
    ↓
messaggio PEG
    ↓
widget cambia stato
    ↓
Invalidate()
    ↓
Draw()
    ↓
EndDraw()
    ↓
request_update(dirty rect)
    ↓
processPendingUpdates()
    ↓
uploadDirtyRegion()
    ↓
SDL_UpdateTexture()
```

Se trascini o tieni premuto, arrivano molti eventi di movimento:

```text
SDL_FINGERMOTION
SDL_MOUSEMOTION
```

Ogni evento può generare redraw.

Per questo è normale vedere:

```text
riposo:       7–9 calls/s
interazione: 14–20 calls/s
```

Il problema però non è soltanto il numero di chiamate.

Il problema è:

```text
più chiamate + rettangoli molto più grandi
```

---

## 12. Bounding box unico e dirty region che si gonfia

Un possibile problema è il merge delle dirty region.

Immagina che cambino due zone piccole, lontane tra loro:

```text
+--------------------------------+
| A                              |
|                                |
|                                |
|                              B |
+--------------------------------+
```

Se il sistema mantiene due dirty region separate, copia solo A e B.

Se invece le fonde in un unico bounding box, copia tutto il rettangolo che contiene A e B:

```text
+--------------------------------+
| A..............................|
|................................|
|................................|
|..............................B |
+--------------------------------+
```

In questo modo una modifica piccola può trasformarsi in un upload enorme.

Questo spiegherebbe perché durante l’interazione compare spesso:

```text
maxRectPx=614400
```

cioè quasi tutto lo schermo.

---

## 13. Collegamento con i contatori `perf`

Le metriche di `uploadDirtyRegion()` spiegano molto bene i risultati osservati con `perf`.

Durante il touch:

```text
reqMBps aumenta
updateMs aumenta
maxRectPx arriva a full-screen
```

È quindi naturale osservare anche:

```text
l2d_cache      aumenta
l2d_cache_refill aumenta
l2d_cache_wb  aumenta
bus_access    aumenta
mem_access    aumenta
DDR write-accesses globali aumentano
```

---

## 14. `mem_access`

### Significato

`mem_access` è un evento PMU del core CPU.

Indica, in modo semplificato:

```text
quante operazioni di accesso ai dati in memoria vengono generate dal core CPU
```

Quindi riguarda il punto di vista della CPU.

Non indica direttamente:

```text
- RAM DDR usata dal sistema
- memoria GPU
- RES di top
- byte effettivamente letti/scritti in DDR
```

Indica invece operazioni tipo:

```cpp
x = array[i];      // load
array[i] = value;  // store
```

---

### A quale memoria fa riferimento?

Alla gerarchia di memoria vista dalla CPU:

```text
CPU
 ↓
L1 data cache
 ↓
L2 cache
 ↓
bus / interconnect
 ↓
DDR
```

Un `mem_access` può essere soddisfatto in L1, in L2 o arrivare fino alla DDR.

Quindi:

```text
mem_access alto
```

significa:

```text
il processo sta facendo molte letture/scritture dati
```

ma non significa automaticamente:

```text
tutte quelle letture/scritture arrivano alla DDR
```

---

### Nel caso della GUI

Durante il touch, `mem_access` aumenta perché PEG/SDL eseguono molte più operazioni memoria:

```text
- lettura strutture dati GUI
- lettura immagini/font
- scrittura framebuffer
- copia verso texture
- gestione eventi
```

È coerente con l’aumento di `reqMBps` e `updateMs`.

---

## 15. `l2d_cache`, `l2d_cache_refill`, `l2d_cache_wb`

### 15.1 `l2d_cache`

Indica accessi alla cache L2 dati.

Quando aumenta, significa che la CPU sta consultando molto di più la L2 per dati.

---

### 15.2 `l2d_cache_refill`

Indica che un dato non è stato trovato in L2 ed è stato caricato da un livello inferiore.

Schema:

```text
DDR / interconnect
        ↓
cache L2
        ↓
CPU
```

Quindi è un indizio di traffico in ingresso verso la L2.

---

### 15.3 `l2d_cache_wb`

Indica write-back dalla L2 verso livelli inferiori.

Schema:

```text
cache L2
        ↓
bus / interconnect / DDR
```

Un write-back avviene quando una cache line modificata, cioè dirty, deve essere scritta fuori dalla cache.

Nel caso della GUI, questo è molto importante perché il framebuffer viene scritto spesso.

Esempio:

```cpp
framebuffer[pixel] = colore;
```

Questa scrittura può sporcare cache line che poi devono uscire dalla L2.

---

### 15.4 Perché refill e write-back non devono essere uguali

Sono due direzioni diverse:

```text
refill     = dati che entrano nella L2
write-back = dati modificati che escono dalla L2
```

Un programma che legge molto e scrive poco avrà molti refill e pochi write-back.

Un programma che scrive molto su buffer grandi può avere molti write-back.

Nel caso della GUI, durante il touch aumentano entrambi, perché la GUI:

```text
- legge strutture, immagini, font, stato widget
- scrive pixel nel framebuffer
- copia dati verso la texture
```

---

## 16. `bus_access` e `bus_cycles`

### 16.1 `bus_access`

Indica accessi al bus/interconnect generati dal core CPU.

È più vicino alla parte “fuori dalla cache” rispetto a `mem_access`.

Non è ancora una misura perfetta della DDR, ma è un segnale forte di pressione sul sistema di memoria.

---

### 16.2 `bus_cycles`

Indica cicli associati all’attività del bus.

Non va interpretato direttamente come MB/s, ma è utile in confronto relativo:

```text
idle vs touch
prima patch vs dopo patch
GUI leggera vs GUI pesante
```

Se durante il touch `bus_cycles` cresce molto, significa che il bus/interconnect è più coinvolto.

---

## 17. Eventi DDR: `imx8_ddr0/read-accesses/` e `imx8_ddr0/write-accesses/`

Il comando:

```bash
perf stat -a -I 1000 \
  -e imx8_ddr0/read-accesses/,imx8_ddr0/write-accesses/
```

misura eventi dal punto di vista del controller DDR.

Schema:

```text
CPU / GPU / display controller / DMA / periferiche
        ↓
interconnect
        ↓
controller DDR
        ↓
RAM esterna
```

---

### 17.1 `write-accesses`

Indica accessi di scrittura arrivati al controller DDR.

È una misura globale di sistema, perché viene usato `-a`.

Quindi può includere traffico generato da:

```text
- PegExec
- altri processi
- kernel
- driver grafico
- GPU
- display controller
- DMA
- filesystem
```

Non è attribuibile automaticamente solo a PegExec.

Però se aumenta temporalmente quando si tocca la GUI e, nello stesso momento, aumentano anche i contatori del processo PegExec, allora la correlazione è forte.

---

### 17.2 `read-accesses` a zero

Nei dati osservati, `read-accesses` rimaneva a zero.

Questo non è fisicamente credibile.

Un sistema Linux legge continuamente dalla DDR.

Quindi la conclusione corretta è:

```text
il contatore imx8_ddr0/read-accesses non sembra affidabile in questa configurazione
```

Possibili cause:

```text
- evento esposto ma non implementato correttamente
- bug o limite del driver PMU DDR
- alias perf non corretto
- contatore non collegato al registro giusto
- configurazione kernel/SoC specifica
```

Per questo, nella relazione, conviene usare `write-accesses` come indicatore relativo e trattare `read-accesses` come non affidabile.

---

## 18. GPU meminfo: `/sys/kernel/debug/gc/meminfo`

Il comando:

```bash
watch -n 0.5 "cat /sys/kernel/debug/gc/meminfo"
```

mostra informazioni sulla memoria gestita dal driver GPU Vivante/galcore.

Esempio con interfaccia spenta:

```text
POOL SYSTEM:
  Free :        268413936 B
  Used :            21520 B
  MinFree :     259782536 B
  MaxUsed :       8652920 B
  Total :       268435456 B
POOL VIRTUAL:
  Used :             0 B
  MaxUsed :          0 B
```

Esempio con interfaccia attiva:

```text
POOL SYSTEM:
  Free :        259782536 B
  Used :          8652920 B
  MinFree :     259782536 B
  MaxUsed :       8652920 B
  Total :       268435456 B
POOL VIRTUAL:
  Used :       7389184 B
  MaxUsed :    7389184 B
```

---

### 18.1 `POOL SYSTEM`

È memoria di sistema gestita/usata dal driver GPU.

Valori osservati:

```text
interfaccia spenta:
Used = 21,520 B ≈ 0.02 MiB

interfaccia attiva:
Used = 8,652,920 B ≈ 8.25 MiB
```

La pool totale è:

```text
268,435,456 B = 256 MiB
```

Quindi l’interfaccia attiva usa circa:

```text
8.25 MiB / 256 MiB ≈ 3.2%
```

Non è molto.

---

### 18.2 `POOL VIRTUAL`

Indica memoria/mapping virtuale lato GPU.

Valori osservati:

```text
interfaccia spenta:
Used = 0 B

interfaccia attiva:
Used = 7,389,184 B ≈ 7.05 MiB
```

---

### 18.3 Interpretazione

Il confronto dice:

```text
interfaccia spenta
→ memoria GPU quasi nulla

interfaccia attiva
→ allocazioni GPU presenti ma contenute

touch/interazione
→ valori stabili
```

Quindi:

```text
la GUI usa la pipeline grafica/GPU/driver
ma non sembra saturare la memoria GPU
e il touch non provoca nuove allocazioni GPU significative
```

Attenzione: questo non significa che la GPU non venga usata.

Significa solo:

```text
non si osserva crescita di memoria GPU durante l’interazione
```

---

## 19. Risposta alla domanda: CPU o GPU?

Dai dati raccolti la risposta più precisa è:

```text
non sembra principalmente un problema di saturazione CPU
non sembra principalmente un problema di memoria GPU
sembra soprattutto un problema di traffico memoria/cache/bus generato dalla pipeline grafica durante l’interazione
```

In forma più concreta:

```text
touch/interazione
    ↓
più eventi
    ↓
più redraw PEG
    ↓
dirty region più grandi
    ↓
più SDL_UpdateTexture
    ↓
più MB copiati
    ↓
più write-back L2
    ↓
più bus_access
    ↓
più scritture DDR globali
    ↓
possibile interferenza sui task real-time
```

---

## 20. Tabella riassuntiva delle metriche

| Metrica | Significato | Cosa indica se aumenta | Importanza RT |
|---|---|---|---|
| `calls` | numero di upload in circa 1 secondo | più redraw / più eventi / più aggiornamenti | media |
| `req` | MB totali copiati nella finestra | più pixel copiati | alta |
| `reqMBps` | MB/s richiesti alla pipeline grafica | più banda memoria usata | molto alta |
| `updateMs` | ms/s spesi in `SDL_UpdateTexture` | più tempo perso in upload | molto alta |
| `effMBps` | velocità durante `SDL_UpdateTexture` | efficienza della copia | diagnostica |
| `maxRectPx` | dirty region massima in pixel | rettangoli troppo grandi / full-screen update | molto alta |
| `mem_access` | accessi memoria lato CPU | più load/store del processo | media/alta |
| `l2d_cache_refill` | dati caricati in L2 da sotto | più miss L2 / più traffico lettura fuori cache | alta |
| `l2d_cache_wb` | dati sporchi scritti fuori da L2 | più scritture verso bus/DDR | alta |
| `bus_access` | accessi al bus/interconnect | più pressione fuori dalla CPU | alta |
| `imx8_ddr0/write-accesses` | scritture DDR globali | più traffico DDR di sistema | alta, ma globale |
| `gc/meminfo Used` | memoria GPU allocata | più allocazioni GPU | utile, ma non misura carico GPU |

---

## 21. Come leggere una riga del log

Esempio:

```text
[RT] uploadDirtyRegion: calls=20 req=8.05MB reqMBps=7.71 updateMs=97.835 effMBps=82.3 maxRectPx=614400
```

Lettura completa:

```text
In circa 1 secondo:
- uploadDirtyRegion è stata chiamata 20 volte
- sono stati copiati circa 8.05 MB di pixel
- normalizzati sul tempo reale sono circa 7.71 MB/s
- quasi 98 ms sono stati spesi dentro SDL_UpdateTexture
- durante quei 98 ms la velocità media della copia è stata circa 82.3 MB/s
- almeno una dirty region era grande quanto tutto lo schermo
```

Interpretazione:

```text
questa è una situazione pesante per il real-time
```

Perché combina:

```text
calls alte
reqMBps alto
updateMs alto
maxRectPx full-screen
```

---

## 22. Regole pratiche di interpretazione

### 22.1 Se aumentano le `calls`

Probabile causa:

```text
più eventi touch/motion
più redraw
timer grafici attivi
```

Azioni possibili:

```text
coalescing degli eventi motion
limitare redraw inutili
limitare frequenza update
```

---

### 22.2 Se aumenta `reqMBps`

Probabile causa:

```text
si stanno copiando più pixel
dirty region più grandi
più upload full-screen
```

Azioni possibili:

```text
migliorare dirty region
evitare bounding box unico troppo grande
ridurre full-screen update
```

---

### 22.3 Se aumenta `updateMs`

Probabile causa:

```text
SDL_UpdateTexture costa molto tempo
copie grandi
tanti upload piccoli
overhead driver/texture
```

Azioni possibili:

```text
ridurre numero upload
ridurre dimensione rettangoli
posticipare upload al momento del present
evitare upload quando non si presenta
```

---

### 22.4 Se `effMBps` è alto

Non significa automaticamente che la situazione sia buona.

Può significare:

```text
copie grandi ed efficienti per byte
```

ma se contemporaneamente `reqMBps` e `updateMs` sono alti, il sistema sta comunque facendo molto lavoro.

---

### 22.5 Se `effMBps` è basso

Può indicare:

```text
tanti upload piccoli
molto overhead per pochi dati
frammentazione
```

In quel caso non è la banda a dominare, ma il costo fisso delle chiamate.

---

### 22.6 Se `maxRectPx=614400`

Con risoluzione 960×640 significa:

```text
full-screen upload
```

Questo è un segnale critico.

Azioni possibili:

```text
capire chi invalida tutto lo schermo
distinguere tap singolo da drag
loggare rect.w, rect.h, left, top, right, bottom
verificare merge dirty region
```

---

## 23. Priorità di ottimizzazione suggerite

Dai dati raccolti, la priorità più logica è:

### 1. Misurare meglio le dirty region

Aggiungere log opzionale di:

```text
rect.x
rect.y
rect.w
rect.h
area
```

soprattutto quando:

```text
area > 50% dello schermo
```

Obiettivo:

```text
capire perché si arriva a full-screen
```

---

### 2. Separare tap singolo e drag continuo

Fare due test:

```text
Test A: tap singolo su bottone
Test B: drag continuo su slider/lista/schermata
```

Interpretazione:

```text
se il tap singolo produce maxRectPx=614400
→ invalidazione troppo ampia anche sul click

se il full-screen compare solo durante drag
→ problema più legato agli eventi motion e al merge dirty region
```

---

### 3. Coalescing degli eventi motion

Se arrivano molti eventi motion al secondo, non è detto che vadano tutti processati.

Si può valutare una politica del tipo:

```text
tieni solo l’ultimo motion event prima del redraw
```

Questo può ridurre:

```text
calls
redraw
uploadDirtyRegion
```

---

### 4. Evitare upload quando non si presenta

Se `uploadDirtyRegion()` viene chiamata anche quando poi non si fa `SDL_RenderPresent()`, si rischia di copiare dati inutilmente.

Possibile idea:

```text
accumulare dirty region
fare upload solo quando si sta per presentare
```

---

### 5. Ridurre full-screen bounding box

Se il problema è il bounding box unico, si può valutare:

```text
gestire più dirty rect separati
oppure imporre una soglia:
se due rettangoli lontani generano un bbox enorme,
conviene fare due update separati
```

Questo va verificato sperimentalmente, perché più chiamate piccole possono avere overhead maggiore.

---

### 6. Capire il redraw periodico a riposo

A riposo si osservano comunque:

```text
7–9 calls/s
22–30 ms/s in SDL_UpdateTexture
```

Quindi conviene capire chi ridisegna periodicamente.

Possibili cause:

```text
timer PEG
lampeggi
stati grafici
cursor update
barra stato
aggiornamenti ciclici
```

Ridurre questa baseline aiuta anche senza interazione.

---

## 24. Conclusione tecnica

I dati mostrano chiaramente due comportamenti.

A riposo, l’interfaccia produce già aggiornamenti periodici della texture, con circa 7–9 upload al secondo, circa 0.75–1 MB/s di dati copiati e circa 22–30 ms/s spesi in `SDL_UpdateTexture()`.

Durante l’interazione touch, il comportamento cambia in modo netto: il numero di upload sale spesso a 14–20 al secondo, il volume copiato cresce fino a circa 5–8 MB/s, il tempo trascorso in `SDL_UpdateTexture()` arriva fino a circa 60–97 ms/s e la dirty region massima raggiunge spesso 614400 pixel, cioè l’intero schermo a 960×640.

Questo indica che il problema principale non è semplicemente “la GUI usa CPU”, né una crescita della memoria GPU allocata. La memoria GPU resta contenuta e stabile durante l’interazione. Il dato più rilevante è invece l’aumento del traffico memoria/cache/bus causato dagli aggiornamenti grafici, in particolare quando le dirty region diventano grandi quanto tutto lo schermo.

---

## 25. Frase pronta per relazione o tesi

> La strumentazione introdotta in `uploadDirtyRegion()` mostra che, durante l’interazione con il touchscreen, la GUI aumenta sia la frequenza degli aggiornamenti della texture sia la dimensione delle regioni aggiornate. In condizioni di riposo si osservano circa 7–9 upload al secondo, con un traffico inferiore a 1 MB/s e circa 22–30 ms/s spesi in `SDL_UpdateTexture()`. Durante la pressione o il trascinamento, invece, gli upload salgono fino a 14–20 al secondo, il traffico raggiunge circa 5–8 MB/s e il tempo speso in `SDL_UpdateTexture()` arriva fino a circa 60–97 ms/s. Inoltre, la dirty region massima raggiunge spesso 614400 pixel, corrispondenti all’intero schermo a 960×640. Questi risultati suggeriscono che l’interferenza osservata sia legata principalmente al traffico memoria generato dal ridisegno della GUI e dall’aggiornamento della texture, più che a una saturazione della CPU o a un incremento dell’occupazione di memoria GPU.

---

## 26. Checklist rapida da usare davanti al terminale

| Se vedi... | Significa probabilmente... | Cosa controllare |
|---|---|---|
| `calls` alte | molti redraw / molti eventi | touch motion, timer, invalidazioni |
| `reqMBps` alto | molti pixel copiati | dirty region grandi |
| `updateMs` alto | molto tempo dentro SDL_UpdateTexture | costo diretto per RT |
| `effMBps` basso | tanti upload piccoli / overhead alto | frammentazione update |
| `effMBps` alto + `reqMBps` alto | copie grandi efficienti ma pesanti | full-screen update |
| `maxRectPx=614400` | full-screen upload | chi invalida tutto |
| GPU meminfo stabile | no nuove allocazioni GPU | non esclude carico GPU |
| `l2d_cache_wb` alto | molte scritture dirty fuori da L2 | framebuffer/texture upload |
| `bus_access` alto | più pressione su bus/interconnect | correlare con touch |
| DDR write globali alti | più traffico DDR di sistema | correlare con PegExec/perf |

---

## 27. Mini-glossario

### Upload

Copia di pixel dal framebuffer software alla texture SDL.

### Dirty region

Zona dello schermo che è cambiata e deve essere aggiornata.

### Full-screen update

Aggiornamento di una regione grande quanto tutto lo schermo.

### Bounding box

Rettangolo minimo che contiene una o più dirty region. Se le regioni sono lontane, il bounding box può diventare molto grande.

### RGB565

Formato pixel a 16 bit: 5 bit rosso, 6 bit verde, 5 bit blu. Occupa 2 byte per pixel.

### PMU

Performance Monitoring Unit. Hardware interno al processore/SoC che conta eventi come cache miss, accessi memoria, cicli bus.

### L2 refill

Dati caricati nella cache L2 perché mancavano.

### L2 write-back

Dati modificati in L2 scritti verso livelli inferiori.

### DDR PMU

Contatore prestazionale del controller DDR. Misura eventi globali della memoria esterna, non di un singolo processo.

### GPU meminfo

File debug del driver GPU che mostra memoria grafica allocata. Non misura direttamente carico GPU o banda DDR.

---

## 28. Idea centrale da ricordare

La metrica più importante non è una sola.

Bisogna leggere insieme:

```text
calls      → quante volte aggiorno
reqMBps    → quanti dati muovo
updateMs   → quanto tempo perdo
maxRectPx  → quanto grande è la dirty region
```

Nel caso osservato, il problema emerge quando questi valori crescono insieme:

```text
calls alte
reqMBps alto
updateMs alto
maxRectPx full-screen
```

Questa combinazione indica che l’interazione con la GUI non sta solo generando più eventi, ma sta causando aggiornamenti grafici grandi e costosi, con effetti visibili anche sui contatori cache, bus e DDR.
