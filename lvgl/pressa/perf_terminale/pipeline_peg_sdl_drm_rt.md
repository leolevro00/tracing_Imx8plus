# Pipeline grafica PEG → SDL → DRM/KMS e impatto real-time

## Scopo del documento

Questo documento descrive in modo completo la catena grafica osservata nel porting dell’interfaccia HMI/CNC basata su PEG/Pegenstein e SDL.

L’obiettivo è chiarire:

- cosa succede quando l’utente tocca lo schermo;
- quali thread sono coinvolti;
- qual è il ruolo di PEG, SDL, LVGL e DRM/KMS;
- dove avvengono le copie di memoria;
- perché `uploadDirtyRegion()`, `SDL_UpdateTexture()`, `SDL_RenderCopy()` e `SDL_RenderPresent()` sono punti importanti;
- quali parti sono librerie esterne e quali sono codice di integrazione/porting;
- perché questa pipeline può interferire con task real-time.

La descrizione è pensata per essere usata come base tecnica per relazione, tesi o documentazione interna.

---

# 1. Idea generale

La pipeline reale può essere riassunta così:

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

In modo ancora più sintetico:

```text
PEG = gestisce e disegna la GUI
SDL = riceve input grezzo e porta i pixel sul display
DRM/KMS = sottosistema Linux che mostra fisicamente il buffer sul pannello
```

Nel sistema analizzato, la GUI non sembra essere una GUI LVGL nativa.

La catena effettiva è:

```text
PEG / Pegenstein
    ↓
framebuffer software in RAM
    ↓
SDL_Texture
    ↓
SDL renderer / present
    ↓
DRM/KMS
    ↓
display fisico
```

LVGL risulta presente come libreria inizializzata, ma non sembra partecipare realmente alla gestione della GUI, dell’input o del display.

---

# 2. Differenza fondamentale tra PEG e SDL

## 2.1 PEG

PEG è il motore grafico/widget toolkit che conosce la logica dell’interfaccia.

PEG sa cosa sono:

- bottoni;
- finestre;
- popup;
- tabelle;
- liste;
- grafici;
- label/testi;
- widget premuti, selezionati o disabilitati;
- eventi GUI come click, movimento e rilascio.

Quando l’utente preme un bottone, PEG è il componente che capisce:

```text
Alle coordinate x,y c’è il bottone Insert.
Il bottone è stato premuto.
Devo cambiare stato grafico.
Devo eventualmente aprire un popup.
Devo ridisegnare alcune aree.
```

PEG quindi lavora a livello di **logica GUI** e **disegno dei widget**.

---

## 2.2 SDL

SDL, cioè *Simple DirectMedia Layer*, è una libreria esterna usata come layer di input/output grafico.

SDL non sa cosa sia un bottone PEG.

SDL non sa cosa significhi “Insert”.

SDL non sa cosa sia il popup “General data are not complete”.

SDL lavora a un livello più basso:

- riceve eventi di mouse/touch/tastiera;
- crea una finestra o superficie di output;
- crea texture;
- aggiorna texture;
- disegna texture nel render target;
- presenta il frame sullo schermo.

Nel codice tipicamente compaiono chiamate come:

```cpp
SDL_Init(...);
SDL_CreateWindow(...);
SDL_CreateRenderer(...);
SDL_CreateTexture(...);
SDL_UpdateTexture(...);
SDL_RenderCopy(...);
SDL_RenderPresent(...);
```

Quindi SDL è il ponte tra:

```text
pixel prodotti dalla GUI
    ↓
display fisico
```

---

## 2.3 Analogia semplice

Si può pensare così:

```text
PEG = il pittore
SDL = il sistema che prende il quadro finito e lo appende sullo schermo
DRM/KMS = il muro/impianto fisico che permette al quadro di essere visibile sul display
```

PEG decide **cosa disegnare**.

SDL decide **come portare i pixel disegnati verso lo schermo**.

---

# 3. Quali parti sono librerie esterne e quali sono codice di porting

La catena è mista: alcune parti sono librerie esterne, altre sono codice di integrazione.

| Componente | Cos’è | Scritto internamente? |
|---|---|---|
| PEG | Toolkit GUI embedded legacy | Origine esterna/legacy, ma probabilmente adattato |
| Pegenstein | Layer/progetto di integrazione PEG nel sistema attuale | Probabilmente sì, almeno in parte |
| SDL | Libreria esterna per input/render/display | No |
| LVGL | Libreria grafica embedded esterna | No |
| `uploadDirtyRegion()` | Funzione ponte tra framebuffer PEG e texture SDL | Sì / codice di porting |
| `SDL_UpdateTexture()` | Funzione SDL | No |
| `SDL_RenderCopy()` | Funzione SDL | No |
| `SDL_RenderPresent()` | Funzione SDL | No |
| DRM/KMS | Sottosistema grafico del kernel Linux | No |
| `m_framebuffer` / `g_pyBitmap` | Buffer software dove PEG disegna | Gestito dal porting |
| dirty region | Area modificata della GUI | Concetto PEG/GUI, gestito dal wrapper |

Quindi non bisogna pensare che “la GUI è SDL”.

Più correttamente:

```text
la GUI è PEG;
SDL è il backend usato per input e visualizzazione.
```

---

# 4. Modello a thread

Nel sistema descritto compaiono concettualmente due thread principali legati alla GUI.

## 4.1 Thread main

Il thread main contiene il loop principale, ad esempio dentro una funzione tipo `PEG_internalWinMain`.

Il loop può essere schematizzato così:

```text
Sleep(10)
    ↓
processEvents()
    ↓
processPendingUpdates()
    ↓
flushPresent()
    ↓
pumpLvgl()
```

Il thread main si occupa di:

- leggere gli eventi SDL;
- convertirli verso il sistema PEG/eventi interni;
- verificare se esistono dirty region pendenti;
- chiamare `uploadDirtyRegion()`;
- eseguire il present verso schermo;
- chiamare `lv_timer_handler()` tramite `pumpLvgl()`.

Questo thread è importante perché contiene la parte misurata con le statistiche di upload.

---

## 4.2 Thread GUI PEG / `EtsGUIThread`

Il thread GUI PEG esegue la logica vera della GUI.

In modo semplificato:

```text
loop infinito:
    Execute()
        ↓
    processa messaggi PEG
        ↓
    aggiorna widget
        ↓
    Draw()
        ↓
    scrive nel framebuffer software
```

Questo thread si occupa di:

- prelevare messaggi dalla coda;
- fare hit-test;
- aggiornare lo stato dei widget;
- invocare le funzioni di disegno PEG;
- scrivere i pixel nel framebuffer software;
- generare o concludere dirty region.

La separazione tra thread main e thread PEG è importante perché:

- un thread riceve input e aggiorna SDL;
- l’altro thread aggiorna la logica/disegno PEG;
- tra i due serve sincronizzazione, tipicamente tramite flag, mutex e dirty region.

---

# 5. Catena completa quando tocchi lo schermo

## 5.1 Touch / mouse → kernel Linux

Quando l’utente tocca lo schermo, il controller touch genera eventi hardware.

A livello Linux, questi eventi passano attraverso il sottosistema input, spesso tramite driver tipo `evdev`.

La catena iniziale è:

```text
touchscreen
    ↓
driver input Linux
    ↓
eventi evdev
    ↓
SDL
```

---

## 5.2 Eventi SDL

SDL legge gli eventi input e li rappresenta con eventi propri.

Esempi:

```text
SDL_FINGERDOWN      dito premuto
SDL_FINGERUP        dito rilasciato
SDL_FINGERMOTION    dito in movimento
SDL_MOUSEBUTTONDOWN mouse premuto
SDL_MOUSEBUTTONUP   mouse rilasciato
SDL_MOUSEMOTION     mouse in movimento
```

Questi eventi sono ancora “grezzi”.

SDL conosce:

```text
tipo evento
coordinate
pressione/rilascio
movimento
```

SDL non conosce:

```text
bottone Insert
popup
tabella
grafico
widget PEG
```

---

## 5.3 `processEvents()`

Il thread main chiama `SDL_PollEvent()` dentro `processEvents()`.

Qui gli eventi SDL vengono letti e convertiti in eventi comprensibili dal sistema PEG o dal bus eventi interno.

Esempio logico:

```text
SDL_FINGERDOWN a coordinate x,y
    ↓
conversione coordinate
    ↓
PegMouseMapping(...)
    ↓
messaggio PEG equivalente a pressione mouse/touch
```

Il risultato è che il sistema PEG riceve eventi del tipo:

```text
PM_LBUTTONDOWN
PM_POINTER_MOVE
PM_LBUTTONUP
```

---

## 5.4 PEG aggiorna lo stato dei widget

Il thread GUI PEG preleva i messaggi dalla propria coda.

A questo punto viene eseguito l’hit-test:

```text
alle coordinate x,y quale widget c’è?
```

Esempio:

```text
coordinate x,y
    ↓
sotto il dito c’è il bottone Insert
    ↓
PEG invia il messaggio al bottone
    ↓
PegButton::Message(...)
```

Il bottone può:

- passare allo stato premuto;
- ridisegnarsi;
- inviare un segnale alla finestra padre;
- generare un’azione applicativa;
- far comparire un popup;
- cambiare pagina;
- modificare un grafico.

Importante:

```text
in questa fase si aggiorna la logica della GUI.
Non necessariamente i pixel sono già stati mostrati sul display.
```

---

## 5.5 PEG ridisegna nel framebuffer software

Quando un widget cambia aspetto, PEG deve ridisegnarlo.

Esempi:

- un bottone premuto deve apparire “affossato”;
- una finestra popup deve comparire;
- un grafico deve spostarsi;
- una tabella deve aggiornarsi;
- una label deve cambiare testo.

PEG disegna in un framebuffer software, cioè un blocco di RAM normale che contiene i pixel della schermata.

Esempio concettuale:

```text
g_pyBitmap / m_framebuffer = array di pixel in RAM
```

Se la risoluzione è 960×640 e il formato è RGB565 a 16 bpp:

```text
960 × 640 × 2 byte = 1,228,800 byte ≈ 1.17 MiB
```

Il framebuffer software è quindi una “foto” della schermata in RAM.

PEG scrive direttamente lì:

```text
pixel bottone
pixel popup
pixel testo
pixel grafico
pixel sfondo
```

---

# 6. Dirty region

## 6.1 Cosa significa dirty region

Una dirty region è una zona dello schermo che è stata modificata e quindi deve essere aggiornata anche nella texture/display.

“Dirty” significa “sporca”, cioè:

```text
questa zona del framebuffer non coincide più con ciò che è stato caricato nella texture/display
```

Esempio:

```text
compare un piccolo popup
    ↓
dirty region ideale = rettangolo che contiene solo il popup
```

Altro esempio:

```text
si muove un grafico grande
    ↓
dirty region = area occupata dal grafico
```

---

## 6.2 `Invalidate()`

Quando un widget sa di dover essere ridisegnato, può invalidare un rettangolo:

```text
Invalidate(rect)
```

Questo significa:

```text
questa zona dovrà essere ridisegnata e poi aggiornata a schermo
```

---

## 6.3 `Draw()`

La funzione `Draw()` ridisegna effettivamente il contenuto grafico nel framebuffer software.

Quindi:

```text
Invalidate() = segna cosa è sporco
Draw()       = scrive i nuovi pixel nel framebuffer
```

---

## 6.4 `EndDraw()` e bounding box

A fine disegno, PEG o il layer video raccoglie le aree invalidate.

Nel sistema analizzato sembra che più dirty region possano essere unite in un unico rettangolo complessivo, cioè un bounding box.

Esempio:

```text
dirty region A = bottone in basso
dirty region B = popup al centro
```

Se vengono mantenute separate, vengono copiati solo i due rettangoli.

Se invece vengono fuse in un unico bounding box:

```text
rettangolo unico che contiene sia A sia B
```

allora viene copiata anche tutta l’area tra A e B, anche se non è realmente cambiata.

Questo è importante per interpretare valori come:

```text
maxRectPx = 370688
```

nel caso di un popup visivamente piccolo.

---

## 6.5 `request_update(left, top, right, bottom)`

Quando è stata calcolata la dirty region, viene chiamata una funzione del tipo:

```text
request_update(left, top, right, bottom)
```

Questa funzione non copia ancora i pixel verso SDL.

Serve a dire:

```text
questa area del framebuffer è cambiata
il thread main dovrà occuparsi di caricarla nella texture SDL
```

In pratica salva:

- coordinate della dirty region;
- flag “update pending”;
- eventualmente protegge l’accesso con mutex.

---

# 7. `uploadDirtyRegion()`

## 7.1 Ruolo

`uploadDirtyRegion()` è il ponte tra il mondo PEG e il mondo SDL.

PEG ha già disegnato i pixel nel framebuffer software.

SDL però mostra a schermo una texture.

Quindi bisogna copiare i pixel modificati dal framebuffer software alla texture SDL.

Schema:

```text
framebuffer PEG in RAM
    ↓
uploadDirtyRegion()
    ↓
SDL_Texture
```

---

## 7.2 Cosa fa

La funzione riceve coordinate:

```text
left, top, right, bottom
```

Costruisce un rettangolo SDL:

```cpp
SDL_Rect rect;
rect.x = left;
rect.y = top;
rect.w = right - left + 1;
rect.h = bottom - top + 1;
```

Calcola il pitch:

```cpp
const int pitch = m_width * bytesPerPixel();
```

Calcola il puntatore di partenza dentro il framebuffer:

```cpp
const unsigned char *src =
    m_framebuffer + (top * pitch) + (left * bytesPerPixel());
```

Poi chiama:

```cpp
SDL_UpdateTexture(m_texture, &rect, src, pitch);
```

---

## 7.3 Perché è costosa

Questa chiamata copia pixel.

A 16 bpp, ogni pixel pesa 2 byte.

Quindi una regione da 400×300 pixel costa:

```text
400 × 300 × 2 = 240,000 byte ≈ 234 KiB
```

Una regione da 960×640 pixel costa:

```text
960 × 640 × 2 = 1,228,800 byte ≈ 1.17 MiB
```

Se questa copia viene fatta molte volte al secondo, genera:

- traffico memoria;
- accessi cache;
- write-back;
- bus access;
- possibile interferenza con task real-time.

---

# 8. `SDL_UpdateTexture()`

## 8.1 Cosa fa

`SDL_UpdateTexture()` aggiorna i pixel contenuti in una texture SDL.

Nel sistema analizzato:

```text
sorgente = framebuffer software PEG
destinazione = SDL_Texture
```

Quindi:

```text
SDL_UpdateTexture() = copia area modificata dalla RAM del framebuffer alla texture
```

---

## 8.2 Cosa non fa

`SDL_UpdateTexture()` non mostra necessariamente il risultato sul display.

Dopo questa chiamata, la texture è aggiornata, ma il frame non è ancora stato necessariamente presentato.

Serve ancora:

```text
SDL_RenderCopy()
SDL_RenderPresent()
```

---

# 9. Renderer, render target, back buffer

## 9.1 Cos’è un renderer

Un renderer è l’oggetto che SDL usa per disegnare.

Nel codice è tipicamente:

```cpp
SDL_Renderer *m_renderer;
```

Il renderer è responsabile di operazioni come:

- pulire il buffer di rendering;
- copiare texture;
- disegnare primitive;
- preparare il frame da mostrare.

---

## 9.2 Cos’è un render target

Il render target è la destinazione su cui il renderer sta disegnando.

Nel caso comune, il render target è il back buffer associato alla finestra.

Importante:

```text
il render target non è necessariamente il display fisico visibile.
```

Di solito si disegna su un buffer nascosto e poi lo si presenta.

---

## 9.3 Front buffer e back buffer

Si può pensare a due buffer:

```text
front buffer = ciò che il display sta mostrando ora
back buffer = buffer nascosto dove preparo il prossimo frame
```

La GUI viene preparata nel back buffer.

Quando il frame è completo, viene presentato.

---

# 10. `SDL_RenderCopy()`

## 10.1 Cosa fa

`SDL_RenderCopy()` prende una texture e la disegna sul render target.

Nel codice:

```cpp
SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
```

Significa:

```text
prendi tutta m_texture
disegnala su tutto il render target corrente
```

Con `nullptr, nullptr`:

- il primo `nullptr` indica tutta la texture sorgente;
- il secondo `nullptr` indica tutta la destinazione.

Quindi la texture GUI viene disegnata full-screen nel back buffer.

---

## 10.2 Differenza rispetto a `SDL_UpdateTexture()`

| Funzione | Sorgente | Destinazione | Scopo |
|---|---|---|---|
| `SDL_UpdateTexture()` | framebuffer PEG | SDL_Texture | aggiornare i pixel della texture |
| `SDL_RenderCopy()` | SDL_Texture | back buffer/render target | comporre il frame da mostrare |

Quindi:

```text
UpdateTexture = aggiorna il contenuto della texture
RenderCopy = usa quella texture per costruire il frame
```

---

# 11. `SDL_RenderPresent()`

## 11.1 Cosa fa

`SDL_RenderPresent()` mostra il frame appena preparato.

Dopo `SDL_RenderCopy()`, il back buffer contiene la GUI aggiornata.

`SDL_RenderPresent()` rende visibile quel buffer sul display.

Schema:

```text
back buffer pronto
    ↓
SDL_RenderPresent()
    ↓
front buffer / display visibile
```

---

## 11.2 Perché può bloccare

Con backend come KMSDRM, `SDL_RenderPresent()` può aspettare il VSync o il pageflip.

Questo significa che la chiamata può bloccare fino al momento in cui il display può accettare il nuovo frame.

Per esempio, con display a 60 Hz:

```text
un refresh ogni circa 16.6 ms
```

Se il programma arriva troppo presto, `SDL_RenderPresent()` può attendere il refresh successivo.

---

## 11.3 Differenza tra RenderCopy e Present

| Funzione | Significato semplice |
|---|---|
| `SDL_RenderCopy()` | preparo/disegno la texture nel buffer nascosto |
| `SDL_RenderPresent()` | mostro quel buffer sul display |

Analogia:

```text
RenderCopy    = preparo la lavagna dietro il sipario
RenderPresent = apro il sipario e la mostro al pubblico
```

---

# 12. DRM/KMS e display

## 12.1 Cos’è DRM/KMS

DRM/KMS è il sottosistema grafico del kernel Linux.

DRM significa:

```text
Direct Rendering Manager
```

KMS significa:

```text
Kernel Mode Setting
```

Gestisce:

- display controller;
- risoluzione;
- scanout buffer;
- page flip;
- sincronizzazione col refresh;
- uscita HDMI/LCD;
- interazione col driver grafico.

---

## 12.2 Cosa succede alla fine della pipeline

SDL si appoggia al backend grafico disponibile.

Su sistemi embedded può usare KMSDRM.

Alla fine:

```text
SDL_RenderPresent()
    ↓
DRM/KMS
    ↓
display controller
    ↓
pannello fisico
```

Il display controller legge il buffer di scanout e lo invia al pannello.

---

# 13. Ruolo di LVGL

## 13.1 LVGL è presente, ma non sembra coinvolto nella GUI

Nel codice sono presenti chiamate come:

```cpp
lv_init();
lv_timer_handler();
```

Tuttavia, per una GUI LVGL reale servirebbero anche componenti come:

```text
lv_display_create()
driver display LVGL
lv_indev per input
lv_tick_inc()
widget LVGL come lv_btn, lv_label, lv_table
```

Se questi elementi mancano, LVGL non sta realmente gestendo la GUI.

---

## 13.2 Interpretazione della macro `PEG_USE_LVGL`

Il nome `PEG_USE_LVGL` può essere fuorviante.

Da quanto osservato, sembra indicare più una modalità di compilazione/porting che usa il nuovo backend, ma non una GUI LVGL nativa.

La catena reale non è:

```text
PEG → LVGL → SDL → schermo
```

ma piuttosto:

```text
PEG → framebuffer software → SDL texture → schermo
```

con LVGL inizializzato ma scollegato dalla pipeline principale.

---

## 13.3 Frase sintetica

Una formulazione precisa è:

> LVGL è presente come dipendenza e viene inizializzato, ma non è collegato al display né all’input. La GUI, inclusi bottoni, popup, grafici e gestione touch, è gestita da PEG, che disegna in un framebuffer software. SDL viene usato per copiare quel framebuffer in una texture e presentarlo sul display tramite DRM/KMS.

---

# 14. Dove si genera il carico real-time

I punti più critici sono due:

```text
SDL_UpdateTexture()
SDL_RenderPresent()
```

## 14.1 `SDL_UpdateTexture()`

È critico perché copia pixel:

```text
RAM framebuffer PEG → texture SDL
```

Questa copia può generare:

- traffico DDR;
- cache refill;
- write-back;
- bus access;
- tempo CPU;
- interferenza con altri task.

È il punto che viene misurato con metriche come:

```text
calls
req
reqMBps
updateMs
effMBps
maxRectPx
```

---

## 14.2 `SDL_RenderPresent()`

È critico perché può coinvolgere:

- GPU/display controller;
- page flip;
- VSync;
- driver DRM/KMS;
- interrupt del display;
- attese/blocchi.

Anche se `SDL_UpdateTexture()` è spesso il costo più evidente nei log, il present può comunque incidere sul comportamento temporale.

---

# 15. Collegamento con le metriche misurate

## 15.1 `calls`

Numero di chiamate a `uploadDirtyRegion()` in un intervallo di circa un secondo.

Esempio:

```text
calls=33
```

significa:

```text
in quel secondo sono stati fatti circa 33 upload verso la texture
```

---

## 15.2 `req`

Megabyte totali richiesti nell’intervallo.

Esempio:

```text
req=24.37MB
```

significa:

```text
in quella finestra di misura sono stati copiati circa 24.37 MB complessivi
```

---

## 15.3 `reqMBps`

Megabyte al secondo richiesti.

Formula:

```text
reqMBps = req / durata reale della finestra
```

È la metrica più utile per capire quanta banda grafica viene richiesta.

---

## 15.4 `updateMs`

Tempo totale passato dentro `SDL_UpdateTexture()` nell’intervallo.

Esempio:

```text
updateMs=210
```

significa:

```text
circa 210 ms su 1000 ms sono stati spesi dentro SDL_UpdateTexture()
```

Questo equivale indicativamente a circa il 21% di un core equivalente.

---

## 15.5 `effMBps`

Velocità effettiva durante il tempo passato dentro `SDL_UpdateTexture()`.

Formula:

```text
effMBps = req / (updateMs / 1000)
```

Non è la banda DDR reale del sistema.

Indica solo quanto velocemente vengono smaltiti i byte durante la funzione di update.

---

## 15.6 `maxRectPx`

Area in pixel della dirty region più grande vista in quel secondo.

Esempio:

```text
maxRectPx=485051
```

Se la risoluzione è 960×640:

```text
960 × 640 = 614400 pixel
```

quindi:

```text
485051 / 614400 ≈ 79%
```

Vuol dire che almeno una dirty region in quell’intervallo copriva circa il 79% dello schermo.

---

# 16. Esempi dai casi osservati

## 16.1 Interfaccia quasi ferma

Valori tipici:

```text
calls=7-9
reqMBps≈0.75-1 MB/s
updateMs≈22-30 ms/s
maxRectPx≈66912
```

Interpretazione:

- esistono redraw periodici anche a riposo;
- la banda richiesta è bassa ma non nulla;
- il costo è moderato;
- la dirty region è parziale.

---

## 16.2 Bottone che cambia interfaccia completa

Se premendo un bottone cambia tutta la schermata, un valore alto di `maxRectPx` è atteso.

Esempio:

```text
maxRectPx=614400
```

con risoluzione 960×640 significa full-screen.

Questo non è necessariamente un problema, perché gran parte dello schermo cambia davvero.

---

## 16.3 Popup piccolo

Nel caso del popup di avviso, visivamente cambia solo una finestra piccola.

Valori osservati:

```text
calls=6-8
reqMBps≈2 MB/s
updateMs≈26-33 ms/s
maxRectPx=370688
```

Con risoluzione 960×640:

```text
370688 / 614400 ≈ 60%
```

Interpretazione:

- il costo assoluto non è enorme;
- però la dirty region sembra troppo grande rispetto alla modifica visiva;
- possibile aggregazione di popup + bottone premuto + aree distanti;
- possibile invalidazione del contenitore o bounding box unico troppo ampio.

---

## 16.4 Grafico trascinato tramite touch

Valori osservati:

```text
calls≈33/s
reqMBps≈24 MB/s
updateMs≈208-220 ms/s
maxRectPx=485051
```

Con risoluzione 960×640:

```text
485051 / 614400 ≈ 79%
```

Interpretazione:

- scenario pesante ma coerente con un grafico trascinato;
- il grafico viene ridisegnato continuamente;
- gli upload sono frequenti;
- la dirty region è molto ampia;
- il tempo dentro `SDL_UpdateTexture()` arriva a oltre 20% di un core equivalente;
- scenario critico per interferenza real-time.

---

# 17. Collegamento con perf, cache e DDR

I log di `uploadDirtyRegion()` spiegano perché nei test `perf` aumentano eventi come:

```text
mem_access
bus_access
l2d_cache_refill
l2d_cache_wb
imx8_ddr0/write-accesses
```

Quando PEG ridisegna molte aree e SDL copia molti pixel:

```text
più pixel copiati
    ↓
più accessi memoria CPU
    ↓
più traffico cache
    ↓
più write-back
    ↓
più accessi bus
    ↓
più traffico DDR globale
```

Quindi la pipeline grafica può disturbare i task real-time anche se la CPU non risulta “satura” in `top`.

Il problema non è solo la percentuale CPU.

Il problema è anche:

```text
cache + bus + DDR + driver grafico + pageflip
```

---

# 18. Conclusione tecnica

La pipeline osservata è una pipeline ibrida:

```text
PEG legacy GUI
    ↓
framebuffer software in RAM
    ↓
SDL texture
    ↓
SDL renderer / present
    ↓
DRM/KMS
    ↓
display
```

PEG è responsabile della logica e del disegno della GUI.

SDL è responsabile della ricezione degli eventi input grezzi e del trasferimento dei pixel verso il display.

LVGL è presente ma non sembra realmente connesso alla GUI misurata.

Il punto più critico per il real-time è la fase:

```text
framebuffer software → SDL_Texture
```

realizzata tramite:

```cpp
SDL_UpdateTexture()
```

Quando la dirty region è ampia o viene aggiornata molte volte al secondo, il sistema muove molti pixel, consumando tempo CPU e traffico memoria. Questo è coerente con l’aumento osservato nei contatori PMU e DDR.

---

# 19. Frase pronta per relazione/tesi

> L’analisi del codice e della strumentazione mostra che l’interfaccia non è gestita direttamente da LVGL, ma da una pipeline basata su PEG/Pegenstein. PEG riceve gli eventi input, aggiorna lo stato dei widget e ridisegna le aree modificate in un framebuffer software. Successivamente, il thread main copia le dirty region dal framebuffer verso una texture SDL tramite `SDL_UpdateTexture()` e presenta il frame attraverso `SDL_RenderCopy()` e `SDL_RenderPresent()`, appoggiandosi al backend DRM/KMS del kernel Linux. Di conseguenza, l’interferenza real-time osservata non è attribuibile a LVGL come motore grafico, ma principalmente al traffico memoria generato dalla pipeline PEG → framebuffer → SDL texture, soprattutto durante interazioni che causano dirty region ampie o aggiornamenti frequenti.

---

# 20. Riassunto finale

```text
PEG:
    gestisce widget, logica GUI, popup, bottoni, grafici, disegno

SDL:
    riceve eventi input grezzi e mostra pixel tramite texture/render/present

LVGL:
    presente e inizializzato, ma non realmente coinvolto nella GUI misurata

DRM/KMS:
    sottosistema Linux che porta il frame al display fisico

uploadDirtyRegion():
    ponte critico tra framebuffer PEG e texture SDL

SDL_UpdateTexture():
    copia pixel, punto pesante per memoria/cache/bus

SDL_RenderCopy():
    disegna la texture nel back buffer/render target

SDL_RenderPresent():
    mostra il back buffer sul display tramite pageflip/present
```

In una frase:

```text
PEG decide e disegna; SDL trasporta e presenta; DRM/KMS mostra fisicamente.
```
