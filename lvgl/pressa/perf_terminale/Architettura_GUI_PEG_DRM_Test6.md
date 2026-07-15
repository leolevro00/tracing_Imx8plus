# Architettura GUI PEG/DRM — Analisi completa del Test 6

## Documento di riferimento tecnico

**Target:** i.MX8M Plus  
**Sistema operativo:** Linux PREEMPT_RT / Yocto  
**Processi principali:** `PegExec`, `Lnk`  
**GUI:** PEG / Pegenstein  
**Output video attuale:** DRM/KMS diretto con dumb buffer RGB565  
**Input touch attuale:** evdev diretto tramite `PegDrmEvdev`  
**Data di riferimento:** luglio 2026

---

# Indice

1. [Panoramica generale del sistema](#1-panoramica-generale-del-sistema)
2. [I processi principali: PegExec e Lnk](#2-i-processi-principali-pegexec-e-lnk)
3. [Le librerie condivise](#3-le-librerie-condivise)
4. [Il framebuffer software g_pyBitmap](#4-il-framebuffer-software-g_pybitmap)
5. [Differenza tra XRes/YRes e XView/YView](#5-differenza-tra-xresyres-e-xviewyview)
6. [Il bug del buffer allocato con dimensione errata](#6-il-bug-del-buffer-allocato-con-dimensione-errata)
7. [La pipeline video precedente: SDL + OpenGL ES + GPU](#7-la-pipeline-video-precedente-sdl--opengl-es--gpu)
8. [Che cos’è una texture SDL](#8-che-cosè-una-texture-sdl)
9. [Cosa facevano SDL_UpdateTexture, SDL_RenderCopy e SDL_RenderPresent](#9-cosa-facevano-sdl_updatetexture-sdl_rendercopy-e-sdl_renderpresent)
10. [Perché la GPU non portava il vantaggio atteso](#10-perché-la-gpu-non-portava-il-vantaggio-atteso)
11. [La nuova pipeline DRM diretta](#11-la-nuova-pipeline-drm-diretta)
12. [Che cos’è un DRM dumb buffer](#12-che-cosè-un-drm-dumb-buffer)
13. [Front buffer, back buffer e page flip](#13-front-buffer-back-buffer-e-page-flip)
14. [Dirty regions e damage tracking](#14-dirty-regions-e-damage-tracking)
15. [Perché il damage tracking è delicato con due buffer](#15-perché-il-damage-tracking-è-delicato-con-due-buffer)
16. [Drain dirty e sincronizzazione pre-flip](#16-drain-dirty-e-sincronizzazione-pre-flip)
17. [Il lock PEG e le race condition](#17-il-lock-peg-e-le-race-condition)
18. [La nuova gestione del touch con evdev](#18-la-nuova-gestione-del-touch-con-evdev)
19. [Modifiche architetturali, fix di stabilità e fix CAD](#19-modifiche-architetturali-fix-di-stabilità-e-fix-cad)
20. [Flusso completo attuale e conclusioni](#20-flusso-completo-attuale-e-conclusioni)

Appendici:

- [Appendice A — Tabella comparativa Test 0 vs Test 6](#appendice-a--tabella-comparativa-test-0-vs-test-6)
- [Appendice B — Perché RGB565 riduce il traffico DDR](#appendice-b--perché-rgb565-riduce-il-traffico-ddr)
- [Appendice C — Vocabolario tecnico essenziale](#appendice-c--vocabolario-tecnico-essenziale)
- [Appendice D — Frase tecnica riassuntiva](#appendice-d--frase-tecnica-riassuntiva)

---

# 1. Panoramica generale del sistema

Sul pannello girano principalmente due processi:

| Processo | Ruolo |
|---|---|
| `PegExec` | Interfaccia operatore: pagine, bottoni, tabelle, popup, CAD pezzo, grafici |
| `Lnk` | Task real-time CNC: assi, I/O, PLC, ciclo macchina |

Il concetto fondamentale è:

> **Il Test 6 non ha cambiato il modo in cui l’applicazione decide cosa mostrare. Ha cambiato soprattutto il percorso usato per portare i pixel dal framebuffer software PEG al pannello fisico.**

La GUI continua a essere costruita così:

```text
Applicazione
    ↓
widget PEG
    ↓
disegno software in RAM
    ↓
presentazione sul display
```

La parte cambiata è soprattutto l’ultimo passaggio.

Prima:

```text
PEG → SDL → OpenGL ES → GPU → KMSDRM → display
```

Ora:

```text
PEG → memcpy → DRM dumb buffer → page flip → display
```

---

# 2. I processi principali: PegExec e Lnk

## 2.1 PegExec

`PegExec` è il processo della HMI.

Dentro `PegExec` trovi, in modo semplificato:

```text
Logica applicativa
    ↓
widget PEG
    ↓
disegno sul framebuffer software
    ↓
invio del risultato al display
```

Esempi di elementi gestiti:

- bottoni;
- tabelle;
- campi di testo;
- popup;
- grafici;
- editor;
- CAD del pezzo;
- schermate operative.

La CPU esegue ancora gran parte del disegno PEG.

La GPU, nella vecchia architettura, non disegnava direttamente i widget PEG. Riceveva un’immagine già composta e la trasferiva verso il buffer finale.

---

## 2.2 Lnk

`Lnk` è il processo real-time della macchina CNC.

Gestisce attività come:

- controllo assi;
- I/O;
- ciclo macchina;
- logica PLC;
- task RTC;
- sincronizzazioni periodiche;
- handler real-time.

Il requisito principale è che il carico della GUI non interferisca in modo significativo con `Lnk`.

In particolare:

```text
Lnk deve restare isolato su CPU3
```

Il Test 6 non modifica la logica CNC.

L’obiettivo è indiretto:

> Ridurre il traffico di memoria, le sincronizzazioni grafiche, il carico GPU e i burst della GUI che possono provocare jitter sul task real-time.

---

# 3. Le librerie condivise

`PegExec` carica varie librerie dinamiche `.so`.

| Libreria | Ruolo |
|---|---|
| `libPegLib.so` | Motore GUI PEG, refresh, input, output video, infrastruttura grafica |
| `libcad2d.so` | Logica CAD del pezzo, form, viste, grafico, gestione mouse |
| Altre `.so` | Database, editor programmi, simulazione, componenti applicativi |

## 3.1 libPegLib.so

`libPegLib.so` contiene l’infrastruttura grafica principale:

- motore PEG;
- widget;
- refresh;
- gestione eventi;
- pipeline video;
- gestione dell’input;
- thread collegati alla GUI.

È qui che è stata modificata soprattutto la pipeline video del Test 6.

---

## 3.2 libcad2d.so

`libcad2d.so` contiene la logica del CAD pezzo.

Esempi:

- `CPezzoView`;
- form classica;
- form L/alpha;
- gestione linee;
- disegno del profilo;
- eventi mouse;
- funzioni come `OnLButtonDown`, `OnLButtonUp`, `OnPrev`, `InizioSequenza`.

`libcad2d.so` non decide direttamente come i pixel arrivano all’LVDS.

Dice a PEG cosa disegnare.

Esempio concettuale:

```text
libcad2d:
    "disegna questa linea"

PEG:
    modifica i pixel nel framebuffer software
```

---

# 4. Il framebuffer software g_pyBitmap

`g_pyBitmap` è il framebuffer software usato da PEG.

È una zona di memoria RAM che contiene i pixel della GUI.

La configurazione è:

```ini
XRes=1024
YRes=768
Bpp=16
```

Quindi la dimensione teorica è:

```text
1024 × 768 × 2 byte
= 1.572.864 byte
≈ 1,5 MiB
```

Perché 2 byte?

Perché il formato è RGB565:

```text
Rosso:   5 bit
Verde:   6 bit
Blu:     5 bit
Totale: 16 bit = 2 byte
```

Ogni posizione del buffer rappresenta un pixel.

Esempio concettuale:

```cpp
g_pyBitmap[y * stride + x] = pixel_rgb565;
```

Quando PEG ridisegna un bottone, una linea o una tabella, modifica determinati pixel dentro `g_pyBitmap`.

## Concetto da ricordare

> `g_pyBitmap` è il foglio sul quale PEG disegna la GUI.

Non è necessariamente lo stesso buffer che il controller display sta leggendo in quel momento.

---

# 5. Differenza tra XRes/YRes e XView/YView

La configurazione contiene:

```ini
XRes=1024
YRes=768

XView=1024
YView=600
```

Il significato è:

```text
Dimensione logica del framebuffer PEG: 1024 × 768
Area fisicamente visibile:             1024 × 600
```

Rappresentazione:

```text
g_pyBitmap: 1024 × 768

┌──────────────────────────────┐
│                              │
│       area 1024 × 600        │ ← visibile sul pannello
│                              │
├──────────────────────────────┤
│       area 1024 × 168        │ ← non visibile
└──────────────────────────────┘
```

## 5.1 Che cos’è il pitch o stride

Il pitch, chiamato anche stride, è il numero di byte necessario per passare dall’inizio di una riga all’inizio della riga successiva.

In RGB565:

```text
1024 pixel × 2 byte = 2048 byte per riga
```

Quindi:

```text
pitch = 2048 byte
```

Nota importante:

- `1024 × 768` è la dimensione del framebuffer;
- `2048 byte` è il pitch teorico di una riga;
- il pitch reale può essere maggiore se il buffer richiede allineamenti.

Quando si dice informalmente “pitch 1024×768” si sta spesso confondendo la dimensione del buffer con lo stride.

---

# 6. Il bug del buffer allocato con dimensione errata

Prima il buffer PEG era stato allocato come:

```text
1024 × 600 × 2 byte
```

Ma PEG continuava a usarlo come se fosse:

```text
1024 × 768 × 2 byte
```

Quindi PEG poteva scrivere anche nelle righe da 600 a 767.

Il problema:

```text
Memoria realmente allocata:
riga 0 ... riga 599

Memoria che PEG pensa di possedere:
riga 0 ... riga 767
```

Le scritture oltre la riga 599 finiscono fuori dal buffer.

Questo provoca **heap corruption**.

## 6.1 Conseguenze possibili

Una heap corruption può causare:

- segfault apparentemente casuali;
- crash in funzioni non collegate al punto originale;
- puntatori corrotti;
- oggetti C++ danneggiati;
- rettangoli grafici anomali;
- crash durante il CAD;
- comportamento differente tra build;
- errori che si manifestano molto dopo la scrittura sbagliata.

Il fix in `peg_run.cpp` è stato:

```text
Prima:
allocazione basata su XView × YView

Dopo:
allocazione basata su XRes × YRes
```

Quindi:

```text
Prima: 1024 × 600
Dopo:  1024 × 768
```

Il pannello continua a mostrare solo 600 righe, ma PEG possiede finalmente tutta la memoria che si aspetta.

---

# 7. La pipeline video precedente: SDL + OpenGL ES + GPU

La pipeline precedente era:

```text
PEG
 ↓
g_pyBitmap RGB565
 ↓
uploadDirtyRegion()
 ↓
SDL_UpdateTexture()
 ↓
texture SDL RGB565
 ↓
SDL_RenderCopy()
 ↓
renderer OpenGL ES 2
 ↓
GPU Vivante
 ↓
render target / framebuffer finale
 ↓
KMSDRM
 ↓
LCDIF / LVDS
 ↓
pannello
```

## 7.1 Cosa faceva PEG

PEG disegnava già tutta la GUI in CPU:

```text
CPU → g_pyBitmap RGB565
```

Quindi:

- PEG disegnava bottoni;
- PEG disegnava testo;
- PEG disegnava il CAD;
- PEG disegnava tabelle;
- PEG produceva l’immagine finale.

La GPU non riceveva una lista di widget da disegnare.

Riceveva un’immagine già pronta.

---

# 8. Che cos’è una texture SDL

Una texture è un’immagine gestita dal renderer.

Nel tuo caso:

```text
g_pyBitmap              texture SDL
RAM della CPU      →    memoria gestita dal renderer
```

La texture contiene i pixel che poi il renderer deve disegnare sul render target finale.

La texture può trovarsi:

- in memoria CPU;
- in memoria condivisa;
- in memoria accessibile alla GPU;
- in una rappresentazione interna differente;
- in un formato ottimizzato per il driver.

La chiamata `SDL_UpdateTexture()` trasferisce i pixel dal buffer software alla texture.

Questa operazione non è necessariamente un semplice `memcpy()`.

Può includere:

- controllo del pitch;
- conversione di formato;
- copia verso memoria compatibile con GPU;
- riallineamento;
- sincronizzazione CPU/GPU;
- gestione del driver grafico.

---

# 9. Cosa facevano SDL_UpdateTexture, SDL_RenderCopy e SDL_RenderPresent

## 9.1 SDL_UpdateTexture()

Concettualmente:

```text
g_pyBitmap → texture SDL
```

Serve a caricare nella texture i pixel nuovi.

Nel tuo caso il buffer PEG era RGB565.

Tuttavia il renderer OpenGL ES 2 e il formato finale potevano preferire o utilizzare un formato diverso, per esempio ARGB8888.

Questo poteva provocare:

- conversione nascosta;
- copie aggiuntive;
- operazioni CPU;
- sincronizzazioni con il driver.

---

## 9.2 SDL_RenderCopy()

Concettualmente:

```text
texture → render target
```

SDL chiede al renderer di disegnare la texture sul framebuffer finale.

La GPU esegue un’operazione simile a:

```text
prendi questa immagine
e copiala sul buffer di destinazione
```

Anche se PEG aveva già preparato l’intera GUI.

---

## 9.3 SDL_RenderPresent()

Concettualmente:

```text
rendi visibile il render target
```

SDL gestisce la presentazione del frame:

- sincronizzazione;
- VSync;
- cambio buffer;
- integrazione con KMSDRM;
- presentazione sul display.

La pipeline completa quindi richiedeva vari passaggi:

```text
g_pyBitmap
    ↓ copia / conversione
texture
    ↓ rendering GPU
render target
    ↓ presentazione
scanout
```

---

# 10. Perché la GPU non portava il vantaggio atteso

La GPU è utile quando deve eseguire:

- compositing;
- trasparenze;
- scaling;
- rotazioni;
- shader;
- molte primitive;
- effetti;
- rendering 3D;
- accelerazione 2D progettata per la GPU.

Nel tuo caso PEG aveva già generato l’immagine finale in CPU.

Quindi la GPU non sostituiva il lavoro di PEG.

Il flusso reale era:

```text
CPU disegna tutto
    ↓
CPU/GPU trasferiscono l’immagine
    ↓
GPU copia l’immagine sul buffer finale
```

Il collo di bottiglia misurato era soprattutto l’upload della texture.

Valori osservati:

```text
effMBps ≈ 90–115 MB/s
```

Questo era molto inferiore alla banda teorica della DDR.

Il problema non era la capacità massima della RAM, ma il costo effettivo della pipeline:

- copie;
- conversioni;
- sincronizzazioni;
- cache;
- driver;
- accessi non perfettamente lineari;
- lock;
- attese GPU;
- dimensioni delle dirty region.

## Concetto da ricordare

> La GPU era usata soprattutto per presentare un’immagine software già completa, non per accelerare davvero il disegno dei widget PEG.

---

# 11. La nuova pipeline DRM diretta

Con il Test 6 la pipeline diventa:

```text
PEG
 ↓
g_pyBitmap RGB565
 ↓
uploadDirtyRegion()
 ↓
memcpy delle zone dirty
 ↓
DRM dumb buffer RGB565
 ↓
drmModePageFlip()
 ↓
LCDIF / LVDS
 ↓
pannello
```

Sono stati rimossi dal percorso video:

```text
SDL_UpdateTexture()
SDL_RenderCopy()
SDL_RenderPresent()
renderer OpenGL ES
GPU Vivante
```

La GUI continua a essere disegnata dalla CPU.

La differenza è che i pixel vengono copiati direttamente nel buffer usato dal display.

## 11.1 Cosa resta invariato

Restano invariati:

- widget PEG;
- pagine;
- bottoni;
- tabelle;
- popup;
- CAD;
- logica applicativa;
- risoluzione percepita;
- processo `Lnk`;
- compiti CNC.

## 11.2 Cosa cambia

Cambia:

- backend video;
- formato finale di scanout;
- gestione dei buffer;
- presentazione;
- gestione touch;
- sincronizzazione;
- damage tracking.

---

# 12. Che cos’è un DRM dumb buffer

Un dumb buffer è un buffer grafico semplice allocato tramite DRM.

“Dumb” significa che non offre funzioni avanzate da GPU.

È sostanzialmente memoria che può essere:

- mappata dalla CPU;
- scritta dalla CPU;
- usata dal controller display come framebuffer.

Nel tuo caso:

```text
1024 × 600 × 2 byte
= 1.228.800 byte
≈ 1,17 MiB
```

Il dumb buffer contiene pixel RGB565.

Rappresentazione:

```text
DRM dumb buffer

┌──────────────────────────────┐
│                              │
│  pixel RGB565 1024 × 600     │
│                              │
└──────────────────────────────┘
```

La copia può essere concettualmente:

```cpp
memcpy(drm_buffer + dst_offset,
       g_pyBitmap + src_offset,
       bytes);
```

Il controller display legge il dumb buffer e genera il segnale video.

Il percorso diventa:

```text
CPU scrive → display legge
```

senza necessità di passare dalla GPU.

---

# 13. Front buffer, back buffer e page flip

Normalmente si usano almeno due buffer:

```text
Front buffer
Back buffer
```

## 13.1 Front buffer

È il buffer attualmente mostrato dal display.

```text
Display → Front
```

Il controller LCDIF lo legge continuamente per generare il segnale LVDS.

---

## 13.2 Back buffer

È il buffer preparato dalla CPU per il frame successivo.

```text
CPU → Back
```

La CPU può scriverci senza modificare direttamente l’immagine attualmente mostrata.

---

## 13.3 Page flip

Una volta che il back buffer è pronto:

```cpp
drmModePageFlip(...);
```

Il sistema chiede al DRM di mostrare il back buffer.

Prima del flip:

```text
Display → Buffer A
CPU     → Buffer B
```

Dopo il flip:

```text
Display → Buffer B
CPU     → Buffer A
```

Il page flip non copia i pixel.

Cambia soltanto quale buffer viene letto dal display.

## Concetto fondamentale

> Il page flip scambia i ruoli dei buffer, ma non sincronizza automaticamente il loro contenuto.

---

# 14. Dirty regions e damage tracking

Una dirty region è una zona dello schermo che è cambiata.

Esempio:

```text
┌──────────────────────────┐
│                          │
│       ┌──────────┐       │
│       │ bottone  │       │
│       └──────────┘       │
│                          │
└──────────────────────────┘
```

Se cambia solo il bottone, non serve copiare tutto lo schermo.

Invece di:

```text
1024 × 600 × 2 byte
≈ 1,17 MiB
```

si copia solo il rettangolo dirty.

Questo riduce:

- traffico DDR;
- tempo di `memcpy`;
- durata del lock;
- carico CPU;
- interferenza con il real-time.

## 14.1 Damage tracking

Il damage tracking è il sistema che tiene traccia delle regioni cambiate.

Esempio:

```text
Dirty 1: bottone
Dirty 2: grafico
Dirty 3: barra laterale
```

Queste regioni devono essere trasferite nel buffer DRM corretto.

Il damage tracking è semplice con un solo buffer, ma diventa più delicato con due buffer alternati.

---

# 15. Perché il damage tracking è delicato con due buffer

Questa è una delle parti più importanti.

Supponiamo:

```text
Buffer A = frame 10
Buffer B = frame 9

Display mostra A
```

Situazione:

```text
A: frame 10 ← visibile
B: frame 9  ← vecchio
```

PEG genera il frame 11.

Fra frame 10 e frame 11 cambia solo un bottone.

La dirty region descrive:

```text
frame 10 → frame 11
```

Quindi dice:

```text
è cambiato solo il bottone
```

Se copi solo il bottone dentro B, ottieni:

```text
Buffer B:
- bottone del frame 11
- resto dello schermo del frame 9
```

Quindi B non contiene il frame 11 completo.

Contiene un miscuglio:

```text
frame 9 + bottone frame 11
```

Se fai page flip:

```text
Display → B
```

il display mostra un frame incoerente.

## 15.1 Perché B è fermo al frame 9

Perché i buffer si alternano.

Sequenza:

```text
Frame 9:
Display mostra B

Frame 10:
CPU prepara A
page flip
Display mostra A
B resta frame 9

Frame 11:
CPU deve riutilizzare B
```

Quando B viene riutilizzato, non viene automaticamente aggiornato al frame 10.

Il page flip non copia A dentro B.

---

## 15.2 Esempio con una finestra

Frame 9:

```text
┌──────────────────────────┐
│ sfondo                   │
│                          │
│                          │
└──────────────────────────┘
```

Frame 10:

```text
┌──────────────────────────┐
│ sfondo                   │
│   ┌───────────────┐      │
│   │ finestra      │      │
│   └───────────────┘      │
└──────────────────────────┘
```

Ora:

```text
A = frame 10
B = frame 9
```

Nel frame 11 cambia solo una scritta dentro la finestra.

La dirty region contiene solo la scritta.

Se la copi su B:

```text
B = frame 9 + scritta frame 11
```

Ma in B la finestra non esisteva.

Dopo il flip potresti vedere:

- la finestra sparita;
- una scritta isolata;
- un rettangolo bianco;
- una zona vecchia;
- linee;
- residui del frame precedente.

---

## 15.3 Frase da ricordare

> Una dirty region descrive cosa è cambiato rispetto al frame precedente, ma il back buffer potrebbe contenere il frame di due aggiornamenti prima.

---

## 15.4 Caso con un solo buffer

Con un solo buffer:

```text
Buffer unico = frame 10
```

Quando cambia il bottone:

```text
frame 10 + dirty bottone = frame 11
```

Il resto era già corretto.

Il problema del singolo buffer è un altro: il display può leggere mentre la CPU sta scrivendo, causando tearing.

Con due buffer si riduce il tearing, ma serve gestire la coerenza fra i buffer.

---

## 15.5 Soluzione semplice: copia completa

Una soluzione corretta è:

```text
1. A contiene frame 10
2. B contiene frame 9
3. copia A → B
4. applica la dirty del frame 11 su B
5. page flip
```

Dopo la copia:

```text
A = frame 10
B = frame 10
```

Dopo la dirty:

```text
B = frame 11
```

Questa soluzione è semplice ma costosa, perché copia tutto il frame.

---

## 15.6 Soluzione alternativa: copia completa da g_pyBitmap

Dato che `g_pyBitmap` contiene il frame completo corrente:

```text
g_pyBitmap → back buffer completo
```

Poi si fa il page flip.

Anche questa soluzione è corretta, ma riduce il vantaggio delle dirty regions.

---

## 15.7 Soluzione efficiente: dirty accumulate per buffer

Supponiamo:

```text
B = frame 9
```

Per portare B al frame 11, devi applicare:

```text
dirty frame 9 → 10
+
dirty frame 10 → 11
```

Esempio:

```text
Frame 9 → 10:
cambia rettangolo X

Frame 10 → 11:
cambia rettangolo Y
```

Quando riusi B:

```text
B = frame 9
applica X
applica Y
→ B = frame 11
```

Questo è un damage tracking più efficiente, ma richiede di ricordare quali regioni mancano a ogni buffer.

---

# 16. Drain dirty e sincronizzazione pre-flip

Questi due concetti sono collegati ma distinti.

## 16.1 Drain dirty

Supponiamo che PEG produca rapidamente:

```text
Dirty 1: bottone
Dirty 2: grafico
Dirty 3: barra
```

Se il sistema applica solo Dirty 1 e fa subito flip, Dirty 2 e Dirty 3 non sono ancora nel frame mostrato.

Fare il drain significa:

```text
svuotare la coda delle dirty region pendenti
prima di eseguire il page flip
```

Pseudocodice:

```cpp
while (!dirty_queue.empty()) {
    Rect r = dirty_queue.pop();
    copy_rect_to_back_buffer(r);
}

page_flip();
```

Quindi:

```text
[Dirty 1] [Dirty 2] [Dirty 3]
     ↓         ↓         ↓
         back buffer
              ↓
          page flip
```

---

## 16.2 Sync pre-flip

La sincronizzazione pre-flip serve a verificare che il back buffer sia coerente prima di mostrarlo.

Può essere implementata in vari modi.

### Strategia A

```text
front buffer → back buffer
poi applica le nuove dirty
```

### Strategia B

```text
g_pyBitmap → zone mancanti del back buffer
```

### Strategia C

```text
applica tutte le dirty accumulate
dall’ultima volta che quel buffer è stato aggiornato
```

Il significato concettuale è sempre:

> Prima del page flip, il back buffer deve rappresentare un frame completo e coerente.

---

## 16.3 Differenza fra drain e sync

| Operazione | Problema risolto |
|---|---|
| `drain dirty` | Evita che alcune dirty region pendenti restino fuori dal frame |
| `sync pre-flip` | Evita che il back buffer contenga zone vecchie provenienti da frame precedenti |

---

# 17. Il lock PEG e le race condition

Nel sistema esistono almeno due attività concorrenti:

```text
PegRefreshDaemon:
scrive e ridisegna g_pyBitmap

Thread/output DRM:
legge g_pyBitmap e lo copia nel dumb buffer
```

Senza sincronizzazione può accadere:

```text
1. Il thread DRM inizia a copiare una riga
2. PegRefreshDaemon modifica quella stessa riga
3. Il thread DRM termina la copia
```

Il risultato può contenere:

```text
metà riga vecchia + metà riga nuova
```

Esempio:

```text
Prima:
AAAAAAAAAAAAAAAA

PEG modifica:
BBBBBBBBAAAAAAAA

DRM copia nel mezzo:
BBBBAAAAAAAAAAAA
```

Il buffer DRM riceve dati incoerenti.

## 17.1 LOCK_PEG

Il lock serve a evitare accessi concorrenti incompatibili.

Concettualmente:

```cpp
LOCK_PEG();

memcpy(...);

UNLOCK_PEG();
```

Durante la copia:

- PEG non deve modificare la stessa regione;
- il thread output legge un’immagine stabile;
- si evitano frame parziali.

## 17.2 Costo del lock

Il lock non deve essere tenuto troppo a lungo.

Se si copia tutto il frame:

```text
lock lungo
```

Se si copiano solo regioni dirty piccole:

```text
lock più corto
```

Quindi il damage tracking aiuta anche a ridurre la durata della sezione critica.

---

# 18. La nuova gestione del touch con evdev

## 18.1 Prima

Il percorso era:

```text
dito
 ↓
driver ILITEK
 ↓
kernel input
 ↓
SDL KMSDRM
 ↓
conversione finger → mouse
 ↓
PEG
```

SDL riceveva gli eventi touch e li convertiva in eventi utilizzabili da PEG.

---

## 18.2 Ora

Il percorso è:

```text
dito
 ↓
driver ILITEK
 ↓
/dev/input/event2
 ↓
PegDrmEvdev
 ↓
coordinate PEG
 ↓
evento mouse PEG
```

`PegDrmEvdev` legge direttamente eventi Linux come:

```text
EV_ABS / ABS_X
EV_ABS / ABS_Y
EV_KEY / BTN_TOUCH
EV_SYN / SYN_REPORT
```

Li converte in eventi equivalenti a:

```text
mouse down
mouse move
mouse up
```

PEG continua a ragionare in termini di mouse.

Non deve conoscere il dettaglio hardware del touch.

---

## 18.3 Percorso verso il CAD

Quando il tocco cade nella zona CAD:

```text
PegDrmEvdev
 ↓
evento mouse PEG
 ↓
PEG identifica CPezzoView
 ↓
OnLButtonDown / OnLButtonUp / movimento
 ↓
libcad2d.so
```

Quindi:

- è cambiata la sorgente dell’input;
- non è cambiata la logica CAD;
- il CAD continua a ricevere eventi mouse PEG.

---

## 18.4 Perché SDL è ancora presente

SDL viene ancora inizializzato per:

```cpp
SDL_INIT_EVENTS | SDL_INIT_TIMER
```

Quindi SDL resta utile per:

- timer;
- code eventi;
- eventi applicativi;
- infrastruttura preesistente.

Ma non gestisce più:

- finestra video;
- texture;
- renderer;
- OpenGL ES;
- GPU;
- presentazione KMSDRM.

Distinzione:

```text
SDL come libreria di supporto: sì
SDL come pipeline video: no
```

---

# 19. Modifiche architetturali, fix di stabilità e fix CAD

È importante separare i vari tipi di modifica.

## 19.1 Modifiche architetturali del Test 6

Queste cambiano la pipeline:

```text
SDL video → DRM diretto
OpenGL ES → nessun renderer GPU
texture SDL → dumb buffer
SDL_RenderPresent → drmModePageFlip
scanout 32 bpp → RGB565 16 bpp
touch SDL → evdev diretto
```

---

## 19.2 Fix necessari alla nuova pipeline

### LOCK_PEG

Serve a evitare race tra refresh e copia.

### Drain dirty

Serve a processare tutte le dirty region pendenti prima del flip.

### Sync pre-flip

Serve a evitare che il back buffer contenga parti vecchie.

Questi fix sono legati alla stabilità del doppio buffering e del damage tracking.

---

## 19.3 Fix della dimensione del framebuffer PEG

File:

```text
peg_run.cpp
```

Problema:

```text
buffer allocato 1024×600
PEG usa 1024×768
```

Conseguenza:

```text
heap corruption
```

Fix:

```text
allocazione XRes × YRes
```

Questo è un bug strutturale di memoria.

---

## 19.4 Diagnostica crash

File:

```text
peg_crashdiag.cpp
```

Funzione:

- intercetta segfault;
- stampa backtrace;
- scrive diagnostica su stderr;
- facilita l’identificazione della libreria e della funzione coinvolta.

`peg_crashdiag` non corregge il bug.

Serve a localizzarlo.

---

## 19.5 Fix CAD separati

File e aree coinvolte:

```text
PezzoForm.cpp
Pezzoview.cpp
PezzoFrame.cpp
libcad2d.so
```

Fix osservati:

- form doppia LAlpha;
- crash EditDraw;
- `InizioSequenza`;
- `OnPrev`;
- aggiunta linee nel grafico;
- gestione form classica + LAlpha.

Questi fix riguardano la logica applicativa CAD.

Non sono, concettualmente, parte del passaggio da SDL a DRM.

---

## 19.6 Tabella delle modifiche

| Modifica | Dove | Perché |
|---|---|---|
| Buffer `XRes × YRes` | `peg_run.cpp` | Evitare scritture fuori buffer |
| DRM diretto | `libPegLib.so` / backend video | Rimuovere texture, GPU e conversioni |
| Dumb buffer RGB565 | backend DRM | Scanout diretto a 16 bpp |
| `LOCK_PEG` | `peglvglwindow.cpp` o backend collegato | Evitare race tra disegno e copia |
| Drain dirty | backend DRM | Processare tutte le regioni pendenti |
| Sync pre-flip | backend DRM | Evitare back buffer obsoleto |
| evdev diretto | `PegDrmEvdev` | Gestire touch senza SDL video |
| `peg_crashdiag` | `peg_crashdiag.cpp` | Ottenere backtrace |
| Fix LAlpha | `PezzoForm.cpp` | Evitare crash form |
| Fix `InizioSequenza` / `OnPrev` | `Pezzoview.cpp`, `PezzoFrame.cpp` | Evitare crash nel CAD |

---

# 20. Flusso completo attuale e conclusioni

Quando l’utente tocca il grafico CAD:

```text
1. Il dito tocca il pannello.

2. Il controller ILITEK genera eventi input Linux.

3. Gli eventi arrivano su /dev/input/event2.

4. PegDrmEvdev legge coordinate e stato del tocco.

5. PegDrmEvdev converte il touch in eventi mouse PEG.

6. PEG determina quale widget è stato toccato.

7. Se il punto appartiene al CAD:
   entra in CPezzoView / libcad2d.so.

8. Il CAD aggiorna il proprio stato.

9. PEG invalida una o più regioni.

10. PegRefreshDaemon ridisegna le regioni in g_pyBitmap.

11. Le regioni modificate vengono inserite nella coda dirty.

12. Il backend DRM prende il lock PEG quando necessario.

13. Le dirty region vengono trasferite nel back buffer DRM.

14. Il sistema garantisce la coerenza del back buffer.

15. Viene eseguito drmModePageFlip().

16. Il back buffer diventa il nuovo front buffer.

17. LCDIF legge il buffer RGB565.

18. Il segnale passa al collegamento LVDS.

19. Il pannello mostra il nuovo frame.

20. Il vecchio front buffer diventa disponibile come nuovo back buffer.
```

---

## 20.1 Prima e ora in una frase

### Prima

```text
PEG disegnava in RAM a 16 bpp,
SDL caricava una texture,
OpenGL ES/GPU la copiava,
il display leggeva un buffer finale a circa 32 bpp.
```

### Ora

```text
PEG disegna ancora in RAM a 16 bpp,
la CPU copia direttamente le regioni modificate
in un dumb buffer DRM RGB565,
poi DRM esegue il page flip.
```

---

## 20.2 Cosa non è cambiato

Non sono cambiati:

- il motore PEG;
- i widget;
- la logica delle pagine;
- il CAD;
- la risoluzione visibile;
- il processo `Lnk`;
- il ciclo CNC;
- la logica HMI;
- il ruolo di `libcad2d.so`.

---

## 20.3 Cosa è migliorato

Sono stati ridotti o eliminati:

- upload texture SDL;
- conversioni nascoste;
- rendering OpenGL ES;
- uso GPU per una semplice copia;
- scanout a 32 bpp;
- passaggi intermedi;
- dipendenza dal video SDL;
- alcuni artefatti di scroll;
- parte dell’interferenza verso il real-time.

---

## 20.4 Perché PegExec può restare vicino al 30% durante lo scroll

PEG continua a dover:

- calcolare i widget;
- ridisegnare testo;
- ridisegnare linee;
- ridisegnare grafici;
- aggiornare `g_pyBitmap`;
- gestire dirty region;
- copiare pixel.

Quindi il costo del disegno software resta.

Il miglioramento riguarda soprattutto il percorso successivo:

```text
Prima:
PEG + texture + conversione + GPU + present

Ora:
PEG + memcpy + page flip
```

La percentuale CPU media non racconta da sola l’interferenza real-time.

Contano anche:

- durata dei burst;
- cache miss;
- traffico DDR;
- lock;
- IRQ;
- sincronizzazioni;
- comportamento worst-case;
- CPU affinity;
- condivisione della memoria.

---

## 20.5 Risultati osservati

Valori riportati:

```text
PegExec idle:
~7% → ~4%

PegExec scroll:
~30% → ~30%

rtc_handler_us worst:
~122 µs → ~105 µs

Campioni sopra 100 µs:
2 su circa 432.000
```

Interpretazione:

- il costo PEG di ridisegno resta importante;
- il percorso video è più semplice;
- la GPU non è più coinvolta;
- lo scanout usa meno byte;
- il worst-case RTC è migliorato;
- gli eventi sopra 100 µs sono rari;
- la stabilità grafica sotto scroll è migliorata.

---

# Appendice A — Tabella comparativa Test 0 vs Test 6

| Aspetto | Test 0 | Test 6 |
|---|---|---|
| Motore GUI | PEG | PEG |
| Logica CAD | `libcad2d.so` | `libcad2d.so` |
| Framebuffer PEG | RGB565 software | RGB565 software |
| Dimensione PEG | inizialmente errata 1024×600 | corretta 1024×768 |
| Output video | SDL + KMSDRM | DRM/KMS diretto |
| Renderer | OpenGL ES 2 | nessuno |
| GPU Vivante | usata | non usata per GUI |
| Texture | SDL RGB565 | nessuna texture |
| Presentazione | `SDL_RenderPresent()` | `drmModePageFlip()` |
| Buffer finale | circa ARGB8888 | RGB565 |
| Byte per pixel scanout | 4 | 2 |
| Copia principale | PEG → texture → render target | PEG → dumb buffer |
| Touch | SDL finger-to-mouse | evdev diretto |
| Lock copia | parziale / non sufficiente | `LOCK_PEG` |
| Damage tracking | problematico | drain + sync |
| Artefatti scroll | presenti | risolti |
| Crash CAD | presenti in alcuni flussi | fix specifici |
| GPU memory | usata | non necessaria per la GUI |

---

# Appendice B — Perché RGB565 riduce il traffico DDR

Risoluzione visibile:

```text
1024 × 600
```

Refresh ipotizzato:

```text
60 Hz
```

## RGB565, 16 bpp

```text
1024 × 600 × 2 × 60
= 73.728.000 byte/s
≈ 73,7 MB/s
```

## ARGB8888, 32 bpp

```text
1024 × 600 × 4 × 60
= 147.456.000 byte/s
≈ 147,5 MB/s
```

Quindi lo scanout a 16 bpp richiede circa metà dei byte.

Questo calcolo riguarda soltanto la lettura del controller display.

Non include:

- copie CPU;
- texture upload;
- letture GPU;
- scritture GPU;
- buffer multipli;
- overhead driver;
- cache;
- allineamenti.

---

# Appendice C — Vocabolario tecnico essenziale

## Framebuffer

Memoria contenente i pixel di un’immagine.

---

## Framebuffer software

Buffer in RAM gestito e disegnato dalla CPU.

Nel tuo caso:

```text
g_pyBitmap
```

---

## Scanout buffer

Buffer che il controller display legge continuamente per generare il segnale video.

---

## Texture

Immagine gestita dal renderer, spesso caricata verso la GPU.

---

## Renderer

Componente che prende texture o primitive e produce un’immagine finale.

Nel vecchio sistema:

```text
SDL renderer OpenGL ES 2
```

---

## Render target

Buffer sul quale il renderer disegna il risultato.

---

## DRM

Direct Rendering Manager.

Sottosistema Linux che gestisce risorse grafiche, buffer, display e presentazione.

---

## KMS

Kernel Mode Setting.

Parte del sottosistema grafico Linux che configura modalità video, risoluzione, connettori, CRTC e plane.

---

## Dumb buffer

Buffer DRM semplice, scrivibile dalla CPU e utilizzabile per lo scanout.

---

## Page flip

Cambio del buffer mostrato dal display.

Non copia i pixel.

---

## Front buffer

Buffer attualmente mostrato.

---

## Back buffer

Buffer preparato per il frame successivo.

---

## Dirty region

Rettangolo dello schermo che è cambiato.

---

## Damage tracking

Sistema che tiene traccia delle regioni modificate.

---

## Drain dirty

Elaborazione di tutte le dirty region pendenti prima della presentazione.

---

## Sync pre-flip

Sincronizzazione del back buffer prima del page flip, per evitare zone vecchie.

---

## Tearing

Effetto visivo causato dal display che legge un buffer mentre viene modificato.

---

## Race condition

Problema dovuto a due thread che accedono contemporaneamente agli stessi dati senza sincronizzazione corretta.

---

## Pitch / stride

Numero di byte fra l’inizio di una riga e l’inizio della riga successiva.

---

## RGB565

Formato colore a 16 bit:

```text
5 bit rosso
6 bit verde
5 bit blu
```

---

## ARGB8888

Formato colore a 32 bit:

```text
8 bit alpha
8 bit rosso
8 bit verde
8 bit blu
```

---

## evdev

Interfaccia Linux per leggere eventi di input da `/dev/input/eventX`.

---

## LVDS

Collegamento fisico usato per trasportare il segnale video verso il pannello.

---

## LCDIF

Controller display che legge il framebuffer e genera il flusso video.

---

# Appendice D — Frase tecnica riassuntiva

> PEG continua a renderizzare la GUI in un framebuffer software RGB565. Il Test 6 ha sostituito il backend di presentazione SDL/OpenGL ES con un backend DRM/KMS diretto, che copia le regioni modificate in dumb buffer RGB565 e le presenta tramite page flip. La stabilità è stata migliorata correggendo la dimensione del framebuffer PEG, aggiungendo sincronizzazione fra refresh e copia, drenando le dirty region e mantenendo coerente il back buffer prima del flip.

---

# Schema finale compatto

```text
INPUT

Dito
 ↓
ILITEK
 ↓
/dev/input/event2
 ↓
PegDrmEvdev
 ↓
eventi mouse PEG
 ↓
widget / CAD / libcad2d.so


DISEGNO

PEG / PegRefreshDaemon
 ↓
g_pyBitmap RGB565
1024 × 768
 ↓
dirty regions


OUTPUT

LOCK_PEG
 ↓
uploadDirtyRegion()
 ↓
memcpy
 ↓
DRM back buffer RGB565
1024 × 600
 ↓
drain dirty
 ↓
sync pre-flip
 ↓
drmModePageFlip()
 ↓
LCDIF
 ↓
LVDS
 ↓
pannello 1024 × 600


REAL-TIME

Lnk
 ↓
CPU3
 ↓
task CNC / PLC / RTC

Obiettivo:
ridurre l’interferenza prodotta dalla GUI
senza modificare la logica CNC.
```
