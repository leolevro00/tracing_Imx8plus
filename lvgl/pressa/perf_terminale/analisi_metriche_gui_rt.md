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

---

## 24. Scenario C: trascinamento di un grafico tramite touch

Dopo le prime misure su riposo e interazione generica, è stato analizzato anche uno scenario più pesante: **il trascinamento tramite touch di un grafico visualizzato sullo schermo**.

Questo scenario è particolarmente importante perché un grafico in movimento non aggiorna soltanto un piccolo bottone o un cursore: spesso costringe il sistema a ridisegnare una porzione ampia della schermata, per esempio l’intera area del grafico, la griglia, le curve, gli assi o il contenitore grafico.

I log raccolti sono di questo tipo:

```text
[RT] uploadDirtyRegion: calls=44 req=1.69MB reqMBps=1.67 updateMs=130.150 effMBps=13.0 maxRectPx=291264
[RT] uploadDirtyRegion: calls=48 req=1.75MB reqMBps=1.73 updateMs=140.368 effMBps=12.5 maxRectPx=290608
[RT] uploadDirtyRegion: calls=43 req=2.22MB reqMBps=2.19 updateMs=129.348 effMBps=17.1 maxRectPx=306299
[RT] uploadDirtyRegion: calls=39 req=4.17MB reqMBps=4.05 updateMs=129.332 effMBps=32.2 maxRectPx=374625
[RT] uploadDirtyRegion: calls=33 req=24.74MB reqMBps=24.09 updateMs=216.366 effMBps=114.4 maxRectPx=485051
[RT] uploadDirtyRegion: calls=33 req=24.37MB reqMBps=23.75 updateMs=207.857 effMBps=117.3 maxRectPx=485051
[RT] uploadDirtyRegion: calls=33 req=24.37MB reqMBps=23.77 updateMs=208.219 effMBps=117.1 maxRectPx=485051
[RT] uploadDirtyRegion: calls=33 req=24.58MB reqMBps=23.95 updateMs=212.522 effMBps=115.7 maxRectPx=485051
[RT] uploadDirtyRegion: calls=23 req=15.88MB reqMBps=14.78 updateMs=140.663 effMBps=112.9 maxRectPx=485051
```

---

### 24.1 Lettura generale dello scenario

Il trascinamento del grafico produce un regime più pesante rispetto al semplice tap o all’interazione leggera.

Nei dati si vedono due fasi principali:

```text
fase iniziale:
  calls molto alte, circa 39–48/s
  reqMBps relativamente basso, circa 1.7–4.0 MB/s
  updateMs alto, circa 129–140 ms/s
  effMBps basso, circa 12–32 MB/s

fase stabile di drag grafico:
  calls circa 33/s
  reqMBps circa 23.7–24.1 MB/s
  updateMs circa 208–220 ms/s
  effMBps circa 111–117 MB/s
  maxRectPx fisso a 485051 px
```

Quindi il sistema passa da un regime con **molte chiamate piccole/medie e molto overhead** a un regime con **upload grandi, continui e costosi**.

---

### 24.2 Prime righe: molte chiamate, pochi MB, tanto overhead

Esempi:

```text
calls=44 req=1.69MB reqMBps=1.67 updateMs=130.150 effMBps=13.0 maxRectPx=291264
calls=48 req=1.75MB reqMBps=1.73 updateMs=140.368 effMBps=12.5 maxRectPx=290608
```

In queste righe il dato più evidente è:

```text
calls molto alte
reqMBps non altissimo
updateMs molto alto
effMBps basso
```

Questo significa che il sistema sta facendo **molti upload piccoli o medi**. Il volume totale di dati non è ancora enorme, ma il tempo speso dentro `SDL_UpdateTexture()` è già alto.

La spiegazione probabile è che ogni chiamata abbia un costo fisso:

```text
- ingresso in SDL_UpdateTexture()
- gestione del rettangolo
- controlli interni SDL
- possibile gestione/lock della texture
- interazione con il backend grafico
- copia effettiva dei pixel
```

Quando si fanno tante chiamate con pochi dati per chiamata, il costo fisso pesa molto. Per questo `effMBps` risulta basso.

Esempio:

```text
req=1.69 MB
updateMs=130.150 ms
```

Formula:

```text
effMBps = 1.69 / 0.130150 ≈ 13.0 MB/s
```

Questa non è una situazione di banda enorme, ma è comunque negativa per il real-time perché si spendono già circa:

```text
130 ms / 1000 ms = 13% di un core equivalente
```

solo dentro l’upload della texture.

---

### 24.3 Fase stabile: il grafico viene aggiornato quasi continuamente

Dalla quinta riga in poi compare un regime molto regolare:

```text
calls=33 req≈24.4MB reqMBps≈23.8MB/s updateMs≈208–220ms effMBps≈112–117MB/s maxRectPx=485051
```

Questo significa che, durante il trascinamento del grafico, il sistema aggiorna la texture circa:

```text
33 volte al secondo
```

Questo valore è vicino a un refresh grafico di circa 30 Hz. Quindi durante il drag il grafico si comporta come una vera animazione continua: a ogni intervallo viene ridisegnata e caricata una porzione ampia dello schermo.

Il dato più pesante è `updateMs`:

```text
updateMs ≈ 210 ms/s
```

cioè:

```text
210 ms / 1000 ms ≈ 21% di un core equivalente
```

speso soltanto dentro `SDL_UpdateTexture()`.

Per un sistema real-time questo è un carico importante, perché non pesa solo sulla CPU. Durante questi upload vengono coinvolti anche:

```text
- cache L1/L2
- write-back delle cache line modificate
- bus/interconnect interno al SoC
- memoria DDR
- driver grafico / pipeline SDL
```

---

### 24.4 Significato di `maxRectPx=485051`

Assumendo una risoluzione di:

```text
960 × 640 = 614400 pixel
```

il valore:

```text
maxRectPx = 485051
```

corrisponde a:

```text
485051 / 614400 ≈ 0.789
```

quindi circa:

```text
79% dello schermo
```

Questo significa che durante il trascinamento del grafico almeno una dirty region per secondo arriva a coprire quasi l’80% dello schermo.

A 16 bpp, quindi 2 byte per pixel:

```text
485051 × 2 = 970102 byte ≈ 0.93 MiB
```

Quindi una singola dirty region grande può richiedere quasi 1 MiB di upload.

Il fatto che `maxRectPx` resti fisso a 485051 per molti secondi suggerisce che probabilmente PEG stia invalidando sempre la stessa grande area, cioè verosimilmente l’area del grafico o del pannello che lo contiene.

---

### 24.5 Perché `effMBps` è alto nella fase pesante

Durante la fase stabile del trascinamento si osservano valori come:

```text
effMBps = 114–117 MB/s
```

Questo può sembrare positivo, ma non deve essere interpretato come “il sistema sta andando meglio”.

Significa solo che, quando il sistema copia blocchi grandi, la copia è più efficiente per byte.

Confronto:

```text
molti upload piccoli:
  reqMBps basso
  updateMs alto
  effMBps basso
  overhead dominante

upload grandi e continui:
  reqMBps alto
  updateMs molto alto
  effMBps alto
  banda e tempo totale dominanti
```

Quindi `effMBps` alto indica che la copia è efficiente mentre avviene, ma il sistema sta comunque copiando molti più dati e sta spendendo molto più tempo complessivo.

Per il real-time la metrica più critica resta:

```text
updateMs
```

seguita da:

```text
reqMBps
maxRectPx
calls
```

---

### 24.6 Confronto tra riposo, touch generico e drag del grafico

| Scenario | calls/s | reqMBps | updateMs/s | maxRectPx | Interpretazione |
|---|---:|---:|---:|---:|---|
| Riposo / attività leggera | 7–9 | ~0.75–1 MB/s | ~22–30 ms | ~66k px | redraw periodico leggero |
| Pressione / touch generico | 14–20 | ~5–8 MB/s | ~60–97 ms | fino a 614400 px | interazione pesante con dirty region anche full-screen |
| Inizio drag grafico | 39–48 | ~1.7–4 MB/s | ~129–140 ms | ~290k–375k px | moltissime chiamate, overhead alto |
| Drag grafico stabile | ~33 | ~23.7–24.1 MB/s | ~208–220 ms | 485051 px | scenario critico: upload grandi e continui |

Il trascinamento del grafico è quindi lo scenario più pesante tra quelli misurati finora.

Anche se `maxRectPx=485051` è inferiore al full-screen `614400`, l’aggiornamento è molto più continuo e sostenuto. Per questo `reqMBps` arriva a circa 24 MB/s e `updateMs` supera i 200 ms/s.

---

### 24.7 Interpretazione tecnica del drag grafico

Il comportamento osservato è coerente con questa catena:

```text
touch drag sul grafico
    ↓
tanti eventi motion
    ↓
PEG aggiorna posizione/scala/offset del grafico
    ↓
il widget grafico viene invalidato
    ↓
PEG ridisegna una grande area
    ↓
uploadDirtyRegion() copia il rettangolo dirty verso la texture SDL
    ↓
SDL_UpdateTexture() consuma tempo e muove dati
    ↓
aumentano traffico cache/bus/DDR e potenziale interferenza RT
```

In altre parole, il problema non è necessariamente un bug. È un caso d’uso graficamente pesante: muovere un grafico tramite touch richiede aggiornamenti continui e ampi.

Tuttavia, dal punto di vista real-time è uno scenario critico perché concentra tre fattori:

```text
1. frequenza alta di upload
2. dirty region molto grande
3. tempo elevato dentro SDL_UpdateTexture()
```

---

### 24.8 Come valutare questi dati

La valutazione sintetica è:

```text
scenario: drag/movimento grafico tramite touch
carico: pesante ma coerente
criticità RT: alta
causa probabile: ridisegno continuo di una grande area grafica
metrica più grave: updateMs ≈ 210 ms/s
metrica di banda: reqMBps ≈ 24 MB/s
area aggiornata: fino a circa 79% dello schermo
```

Quindi questi dati non indicano semplicemente “troppe calls”. Indicano:

```text
calls abbastanza alte + rettangoli grandi + tempo di upload molto alto
```

Questa combinazione può spiegare bene gli aumenti osservati con `perf` sui contatori:

```text
mem_access
l2d_cache_wb
bus_access
imx8_ddr0/write-accesses
```

---

### 24.9 Implicazioni per le ottimizzazioni

Per questo scenario, le ottimizzazioni più promettenti sono:

#### 1. Limitare il refresh del grafico durante il drag

Non è detto che ogni evento touch debba produrre un redraw completo.

Si può valutare un limite, ad esempio:

```text
massimo 20–30 redraw/s durante il drag
```

Se arrivano più eventi touch, si può usare solo l’ultimo evento disponibile prima del prossimo frame.

---

#### 2. Coalescing degli eventi motion

Durante un drag possono arrivare molti eventi `SDL_FINGERMOTION` o `SDL_MOUSEMOTION`.

Invece di processarli tutti graficamente, si può fare:

```text
accumula eventi motion
prima del redraw usa solo l’ultimo
```

Questo può ridurre:

```text
calls
redraw PEG
uploadDirtyRegion
updateMs
```

---

#### 3. Separare parte statica e parte dinamica del grafico

Se il grafico contiene assi, griglia, label e sfondo, non sempre ha senso ridisegnare tutto a ogni movimento.

Idealmente:

```text
parte statica:
  assi
  griglia
  sfondo
  label fisse

parte dinamica:
  curva
  cursore
  finestra visibile
  punto selezionato
```

Se si riesce a mantenere cache della parte statica, il drag potrebbe richiedere meno lavoro.

---

#### 4. Verificare se il widget grafico invalida tutta la sua area

Il valore `maxRectPx=485051` stabile suggerisce che l’intero widget grafico o quasi tutto il suo contenitore venga invalidato a ogni aggiornamento.

Per confermarlo, conviene loggare anche:

```text
rect.x
rect.y
rect.w
rect.h
```

soprattutto quando:

```text
area > 300000 px
```

Così si può capire se il rettangolo coincide sempre con la stessa zona del grafico.

---

#### 5. Valutare una modalità RT-safe durante il drag

Se l’interazione con il grafico non è critica quanto il ciclo real-time, si può valutare una modalità in cui, mentre i task RT sono attivi, il grafico viene aggiornato a frequenza ridotta.

Esempio:

```text
modalità normale:
  redraw grafico fino a 30 Hz

modalità RT-safe:
  redraw grafico limitato a 10–15 Hz
  oppure aggiornamento differito quando il sistema è meno carico
```

Questo non elimina la GUI, ma evita che un drag grafico occupi oltre 200 ms/s in upload texture.

---


## 25. Scenario D: popup di avviso piccolo dopo pressione del bottone

È stato analizzato anche un caso diverso dal cambio completo di schermata e dal trascinamento del grafico: la pressione di un bottone che fa comparire una **piccola finestra di avviso** sopra l'interfaccia esistente.

Il caso è importante perché, visivamente, la schermata cambia poco: l'interfaccia principale resta quasi uguale e compare soltanto un popup centrale con un messaggio e un pulsante `Ok`. Questo permette di distinguere tra:

```text
aggiornamento grande giustificato:
  cambio schermata completo

aggiornamento grande sospetto:
  piccola modifica visiva, ma dirty region molto ampia
```

---

### 25.1 Log osservati

Esempi di righe raccolte durante la comparsa del popup:

```text
[RT] uploadDirtyRegion: calls=7 req=2.01MB reqMBps=1.99 updateMs=29.084 effMBps=69.2 maxRectPx=370688
[RT] uploadDirtyRegion: calls=6 req=2.03MB reqMBps=2.02 updateMs=26.080 effMBps=77.7 maxRectPx=370688
[RT] uploadDirtyRegion: calls=8 req=3.19MB reqMBps=3.16 updateMs=37.039 effMBps=86.2 maxRectPx=370688
[RT] uploadDirtyRegion: calls=5 req=0.68MB reqMBps=0.67 updateMs=17.154 effMBps=39.4 maxRectPx=227632
```

Valori riassuntivi:

| Metrica | Valore tipico osservato | Interpretazione |
|---|---:|---|
| `calls` | 4–8/s | frequenza di upload contenuta |
| `reqMBps` | circa 0.67–3.16 MB/s | traffico moderato |
| `updateMs` | circa 16–37 ms/s | costo non nullo, ma molto inferiore al drag del grafico |
| `maxRectPx` | fino a 370688 px | area dirty molto ampia rispetto al popup visivo |

Quindi questo scenario non è pesante come il trascinamento del grafico, ma è più interessante dal punto di vista dell'efficienza delle dirty region.

---

### 25.2 Perché `maxRectPx=370688` è sospetto

Assumendo risoluzione:

```text
960 × 640 = 614400 pixel
```

il valore:

```text
maxRectPx = 370688 pixel
```

corrisponde a:

```text
370688 / 614400 ≈ 0.603
```

cioè circa il **60% dello schermo**.

A 16 bpp, quindi 2 byte per pixel:

```text
370688 × 2 = 741376 byte ≈ 0.71 MiB
```

Quindi una singola dirty region grande in questo scenario può richiedere circa **0.7 MiB** di upload verso la texture.

Questo è sospetto perché il popup visibile è molto più piccolo del 60% dello schermo. A occhio, la finestra di avviso occupa soltanto una porzione centrale della schermata. L'interfaccia sotto resta quasi invariata.

---

### 25.3 Differenza rispetto al cambio completo di schermata

Quando un bottone cambia completamente interfaccia o pagina, una dirty region grande è normale:

```text
pressione bottone
    ↓
cambio schermata
    ↓
gran parte dello schermo cambia
    ↓
maxRectPx grande giustificato
```

Nel caso del popup di avviso, invece, non cambia tutta l'interfaccia. Compare soltanto una piccola finestra sopra la schermata esistente.

Quindi la lettura corretta è:

```text
maxRectPx grande durante cambio pagina:
  atteso

maxRectPx grande durante popup piccolo:
  potenzialmente inefficiente
```

---

### 25.4 Spiegazione più probabile: bounding box unico troppo ampio

La spiegazione più probabile è che il sistema non stia copiando solo la zona del popup, ma stia unendo più zone modificate distanti in un unico rettangolo dirty.

Esempio possibile:

```text
1. il bottone Insert viene premuto e cambia stato
2. compare il popup al centro
3. il pulsante Ok o la barra del popup vengono disegnati
4. forse qualche widget sotto viene invalidato
5. le regioni dirty vengono unite in un solo bounding box
```

Se il bottone premuto è in basso a destra e il popup è al centro, il rettangolo che contiene entrambe le zone può diventare molto più grande delle aree realmente modificate.

Schema semplificato:

```text
+--------------------------------------------------+
|                                                  |
|                  popup                           |
|              +-----------+                       |
|              |  warning  |                       |
|              +-----------+                       |
|                                                  |
|                                                  |
|                                      Insert      |
+--------------------------------------------------+
```

Se si usa un solo bounding box, l'area copiata può includere anche molto spazio non modificato tra popup e bottone.

Quindi il problema non è necessariamente che PEG ridisegni davvero il 60% dello schermo. Potrebbe essere che la rappresentazione finale della dirty region venga **gonfiata** dall'unione di rettangoli distanti.

---

### 25.5 Interpretazione delle metriche in questo scenario

Nel caso del popup, i valori vanno letti così:

```text
calls basse/moderate:
  non ci sono troppi upload al secondo

reqMBps moderato:
  il traffico non è enorme

updateMs moderato:
  il costo è misurabile ma non critico quanto il grafico

maxRectPx alto:
  la zona copiata è probabilmente troppo grande rispetto alla modifica visiva
```

Quindi il collo di bottiglia non è la frequenza di aggiornamento, ma l'ampiezza della regione aggiornata.

In sintesi:

```text
non è un caso critico per banda continua,
ma è un caso sospetto per inefficienza della dirty region.
```

---

### 25.6 Confronto con gli altri scenari

| Scenario | `calls` | `reqMBps` | `updateMs` | `maxRectPx` | Giudizio |
|---|---:|---:|---:|---:|---|
| Riposo / attività leggera | 7–9/s | ~0.75–1 MB/s | ~22–30 ms/s | ~66912 px | baseline non nulla |
| Popup piccolo | 4–8/s | ~0.67–3.16 MB/s | ~16–37 ms/s | fino a 370688 px | non pesante, ma dirty region sospetta |
| Cambio schermata completo | variabile | può salire | può salire | può arrivare a full-screen | giustificato se cambia tutta la pagina |
| Touch generico / cambio pagina | 14–20/s | ~5–8 MB/s | ~60–97 ms/s | fino a 614400 px | pesante se ripetuto, normale se transitorio |
| Drag grafico stabile | ~33/s | ~24 MB/s | ~208–220 ms/s | 485051 px | scenario critico continuo |

Questa tabella evidenzia una distinzione importante:

```text
popup piccolo:
  carico moderato, ma area dirty sproporzionata

drag grafico:
  carico elevato e sostenuto nel tempo

cambio schermata:
  dirty region grande giustificata se l'interfaccia cambia davvero
```

---

### 25.7 Prossima misura consigliata per confermare l'ipotesi

Per capire se il problema è davvero il bounding box unico, non basta più stampare solo `maxRectPx`. Conviene loggare anche le coordinate del rettangolo massimo:

```text
maxRectX
maxRectY
maxRectW
maxRectH
maxRectPx
```

Un log utile sarebbe, per esempio:

```text
[RT] maxRect x=420 y=300 w=610 h=608 area=370688
```

oppure:

```text
[RT] maxRect x=430 y=260 w=390 h=160 area=62400
```

Nel primo caso il rettangolo è enorme e probabilmente contiene sia popup sia bottone o altre aree distanti.

Nel secondo caso il rettangolo coincide più o meno con il popup, quindi la dirty region sarebbe coerente con la modifica visiva.

Questa misura permetterebbe di distinguere tra:

```text
invalidazione realmente ampia:
  PEG ridisegna un grande contenitore

bounding box gonfiato:
  più regioni piccole e distanti vengono unite
```

---

### 25.8 Conclusione sul popup

Il popup di avviso rappresenta uno scenario intermedio. Non genera un carico sostenuto paragonabile al trascinamento del grafico, ma mostra un comportamento potenzialmente inefficiente: la dirty region massima raggiunge circa il 60% dello schermo, nonostante la modifica visiva sembri limitata a una finestra di avviso centrale.

La spiegazione più probabile è l'aggregazione di più regioni dirty distanti in un unico bounding box, oppure l'invalidazione di un contenitore più grande del necessario.

Una conclusione sintetica può essere:

> Nel caso della finestra di avviso, il numero di upload e la banda richiesta restano moderati, ma la dirty region massima raggiunge circa il 60% dello schermo. Poiché visivamente compare soltanto un piccolo popup, questo suggerisce che l'area effettivamente caricata verso la texture sia più ampia dell'area realmente modificata, probabilmente a causa del merge delle dirty region o dell'invalidazione di un contenitore più grande.

---

## 26. Aggiornamento della conclusione tecnica

I dati raccolti mostrano diversi livelli di carico della GUI.

A riposo, l’interfaccia produce comunque aggiornamenti periodici della texture, con circa 7–9 upload al secondo, circa 0.75–1 MB/s di dati copiati e circa 22–30 ms/s spesi in `SDL_UpdateTexture()`.

Nel caso del popup di avviso, il carico resta moderato in termini di frequenza e banda, con circa 4–8 upload al secondo, circa 0.67–3.16 MB/s e circa 16–37 ms/s spesi in `SDL_UpdateTexture()`. Tuttavia, la dirty region massima arriva fino a circa 370688 pixel, cioè circa il 60% dello schermo assumendo una risoluzione 960×640. Questo valore è elevato rispetto alla modifica visiva reale, che consiste principalmente nella comparsa di una piccola finestra di avviso.

Durante una normale interazione touch o un cambio di schermata, il carico può aumentare: gli upload salgono spesso a 14–20 al secondo, il volume copiato arriva a circa 5–8 MB/s e il tempo speso in `SDL_UpdateTexture()` raggiunge circa 60–97 ms/s. In caso di cambio completo dell’interfaccia, una dirty region prossima al full-screen può essere giustificata perché gran parte della schermata cambia davvero.

Il trascinamento di un grafico tramite touch rappresenta invece lo scenario più critico osservato. In fase stabile si misurano circa 33 upload al secondo, circa 24 MB/s copiati verso la texture e oltre 200 ms/s spesi in `SDL_UpdateTexture()`. Il valore `maxRectPx=485051` indica che la dirty region massima copre circa il 79% dello schermo.

Questi risultati indicano che l’interferenza non è spiegata principalmente da una saturazione della memoria GPU, che resta stabile e contenuta, né da una saturazione CPU generica. Il comportamento più rilevante è l’aumento del traffico memoria/cache/bus generato dagli aggiornamenti grafici, in particolare quando vengono ridisegnate e caricate porzioni molto ampie dello schermo. L’analisi mostra anche che una dirty region grande non è sempre da interpretare allo stesso modo: può essere giustificata in un cambio pagina, ma sospetta quando la modifica visiva è limitata, come nel caso del popup.

---

## 27. Riassunto finale operativo

Per valutare rapidamente un nuovo log, conviene leggere le metriche in questo ordine:

```text
1. updateMs
   Quanto tempo viene speso dentro SDL_UpdateTexture?

2. reqMBps
   Quanti MB/s vengono richiesti alla pipeline grafica?

3. maxRectPx
   Quanto è grande la dirty region massima?

4. calls
   Quante volte al secondo si aggiorna la texture?

5. effMBps
   La copia è efficiente o dominata dall’overhead?
```

Nel caso del popup piccolo, il problema principale è:

```text
maxRectPx troppo grande rispetto alla modifica visiva
```

Quindi la misura più utile è stampare anche le coordinate del rettangolo massimo, per capire se la dirty region contiene sia popup sia bottone o altre aree distanti.

Nel caso del grafico trascinato, il problema principale è:

```text
updateMs molto alto + reqMBps alto + dirty region molto grande
```

Quindi la direzione di ottimizzazione più promettente non è ridurre la memoria GPU allocata, ma:

```text
- ridurre la frequenza di redraw del grafico
- coalescere gli eventi motion
- ridurre l’area invalidata
- evitare di ridisegnare parti statiche del grafico
- limitare il refresh grafico in modalità RT-safe
- distinguere gli update full-screen giustificati dai bounding box gonfiati
```

