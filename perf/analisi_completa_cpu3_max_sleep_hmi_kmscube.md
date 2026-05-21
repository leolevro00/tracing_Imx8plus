# Osservazione aggiuntiva su CPU3 - MAX SLEEP: HMI idle vs HMI con interazione

Questo documento aggiunge una precisazione importante all'analisi precedente sul caso:

```text
CPU3 - MAX SLEEP
```

L'idea principale è che, nel caso HMI ESA, il problema non va visto come una sola causa isolata.  
Ci sono almeno due contributi che possono sommarsi:

1. accessi a memoria dovuti a cache miss elevate, probabilmente legate a shared memory/dati condivisi;
2. maggiore difficoltà ad accedere alla memoria quando, oltre alla shared memory, la parte grafica sta renderizzando in modo più intenso.

---

## 1. HMI attiva senza interazione

Nel caso HMI attiva ma senza premere pulsanti, si osserva indicativamente:

```text
L2 cache miss ≈ 20%
ritardo max sleep ≈ 150 us
```

Questo significa che, anche senza interagire con la GUI, la CPU3 ha già una percentuale di miss L2 abbastanza alta.

Un valore intorno al 20% può essere interpretato in modo semplice così:

```text
circa 1 accesso su 5 alla L2 non trova il dato in cache
```

Quando questo accade, la CPU3 deve recuperare il dato da livelli più lontani della gerarchia memoria, tipicamente andando verso memoria/bus/interconnect.

La spiegazione più plausibile è che la HMI, anche se apparentemente ferma, stia comunque aggiornando alcuni valori, ad esempio:

- quote degli assi;
- stati macchina;
- variabili visualizzate;
- diagnostica;
- elementi grafici periodici;
- polling di dati condivisi.

Quindi il sistema non è veramente statico.

La HMI può continuare ad accedere a dati condivisi e a far aggiornare alcune parti dell'interfaccia.

La lettura del caso idle è quindi:

```text
HMI attiva senza pulsanti
  -> accessi periodici a dati condivisi
  -> cache miss L2 già significative, circa 20%
  -> la CPU3 deve andare spesso verso memoria
  -> il bus è ancora abbastanza libero
  -> il ritardo sale, ma resta intorno a 150 us
```

In questo scenario il costo principale sembra essere legato alla shared memory/cache, ma il sottosistema memoria non appare ancora pesantemente congestionato.

---

## 2. HMI attiva con interazione sui pulsanti

Nel caso HMI attiva con pressione dei pulsanti, si osserva indicativamente:

```text
L2 cache miss ≈ 30%
ritardo max sleep ≈ 400 us
```

Qui la situazione peggiora in modo evidente.

La percentuale di L2 miss aumenta ulteriormente: da circa 20% a circa 30%.

Questo significa che la CPU3 è obbligata ancora più spesso a uscire dalla cache e ad andare verso memoria.

Però il punto importante è che, mentre questo accade, la pressione sui pulsanti può attivare più lavoro grafico:

- repaint di bottoni;
- aggiornamento di stati grafici;
- cambio colore/forma/stato del widget;
- possibile aggiornamento di finestre o aree della GUI;
- rendering più intenso rispetto alla sola visualizzazione dei valori assi.

Quindi la CPU3 non solo deve andare più spesso in memoria, ma può trovarsi a farlo mentre la pipeline grafica sta usando più intensamente il bus/memoria.

La lettura del caso con interazione è:

```text
HMI attiva con pulsanti
  -> più accessi a dati condivisi
  -> L2 miss molto alte, circa 30%
  -> la CPU3 deve andare spesso verso memoria
  -> contemporaneamente la GUI sta renderizzando di più
  -> GPU/driver/display/buffer grafici aumentano il traffico su memoria/bus
  -> gli accessi memoria della CPU3 diventano più costosi
  -> il ritardo può salire fino a circa 400 us
```

Quindi il peggioramento non è dovuto soltanto al fatto che aumentano le cache miss.

Il punto è che le miss costano di più quando il bus/interconnect/memoria sono più occupati dal rendering.

---

## 3. Differenza tra HMI idle e HMI con pulsanti

La differenza concettuale può essere vista così:

### HMI attiva senza pulsanti

```text
L2 miss ≈ 20%
ritardo ≈ 150 us
```

Interpretazione:

```text
la CPU3 va spesso in memoria a causa della shared memory,
ma il bus sembra ancora relativamente disponibile.
```

### HMI attiva con pulsanti

```text
L2 miss ≈ 30%
ritardo ≈ 400 us
```

Interpretazione:

```text
la CPU3 va ancora più spesso in memoria
e in più trova il sottosistema memoria/bus più occupato,
perché l'interazione grafica genera più rendering.
```

Quindi il salto da 150 us a 400 us può essere spiegato come combinazione di:

```text
più miss L2
+
costo maggiore di ogni accesso fuori cache
```

---

## 4. Perché il rendering peggiora il costo delle miss

Una miss L2 non ha sempre lo stesso costo.

Se il sistema è poco carico, andare in memoria può costare relativamente meno.

Se invece GPU/display/driver grafici stanno generando traffico, allora la memoria può essere più contesa.

Quindi:

```text
miss L2 con bus poco occupato
    -> costo moderato

miss L2 con bus/interconnect occupato dal rendering
    -> costo molto più alto
```

Questo spiega perché una differenza apparentemente non enorme di miss rate, da 20% a 30%, può produrre un aumento molto più grande del ritardo.

Non conta solo quante volte si va fuori cache.

Conta anche quanto costa ogni accesso fuori cache.

---

## 5. Collegamento con `bus_access/bus_cycles`

Il rapporto:

```text
bus_access/bus_cycles
```

aiuta a capire quanto gli accessi al bus siano densi rispetto ai cicli bus osservati.

Nel caso HMI con interazione si ha un valore indicativo:

```text
bus_access/bus_cycles ≈ 0.046757
```

Questo valore va letto insieme a:

```text
L2 cache miss ≈ 30%
CPI alto
ritardo elevato
```

L'interpretazione è:

```text
la CPU3 genera molti accessi verso il bus perché ha molte miss L2;
questi accessi hanno un costo significativo perché il sistema grafico sta lavorando;
la combinazione delle due cose produce un forte aumento del ritardo.
```

Nel caso HMI senza pulsanti, invece, la CPU3 può avere comunque molte miss, circa 20%, ma il rendering è più leggero.

In quel caso gli accessi verso memoria possono essere meno penalizzanti, e il ritardo resta più basso, intorno a 150 us.

---

## 6. Osservazione chiave

La parte importante da evidenziare è questa:

> Nel caso HMI ESA, le cache miss elevate obbligano la CPU3 ad andare spesso verso memoria. Quando la HMI è attiva ma non viene usata, questo causa già un ritardo significativo, circa 150 us, perché circa una volta su cinque la CPU3 deve recuperare dati fuori dalla L2. Quando però si interagisce con la HMI, il miss rate sale verso il 30% e contemporaneamente aumenta il rendering grafico. In quel momento la CPU3 non solo va più spesso in memoria, ma trova anche il bus/interconnect più occupato. Per questo il ritardo può crescere fino a circa 400 us.

---

## 7. Modello interpretativo finale

Il modello più convincente è quindi a due fattori:

```text
Ritardo finale
  =
  frequenza con cui la CPU3 deve andare fuori cache
  x
  costo di ogni accesso fuori cache
```

Nel caso HMI idle:

```text
miss L2 abbastanza alte
+
bus relativamente meno congestionato
=
ritardo medio-alto, circa 150 us
```

Nel caso HMI con pulsanti:

```text
miss L2 ancora più alte
+
bus più congestionato dal rendering
=
ritardo molto alto, circa 400 us
```

Questo modello spiega perché non basta guardare solo la percentuale di miss.

Bisogna considerare insieme:

- quante miss avvengono;
- quanto costa ogni miss;
- quanto è occupato il bus;
- quanto rendering grafico sta avvenendo;
- quanto la HMI accede a memoria condivisa.

---

---

# Estensione dell'analisi: includere anche il caso `kmscube`

Le osservazioni precedenti descrivono bene il comportamento della HMI ESA, distinguendo tra:

- HMI attiva senza interazione;
- HMI attiva con pressione dei pulsanti.

Tuttavia, per completare il ragionamento è necessario includere anche il caso `kmscube`, perché questo test permette di separare meglio l'effetto della **shared memory ESA** dall'effetto del **rendering grafico continuo**.

`kmscube` è particolarmente utile perché:

```text
non usa la shared memory applicativa ESA
non legge le strutture dati della HMI ESA
non interagisce con le variabili condivise del programma ESA
ma genera comunque rendering grafico continuo tramite GPU/DRM/KMS
```

Quindi, se anche con `kmscube` si osserva un ritardo elevato, significa che il problema non può essere spiegato solo con la shared memory ESA.

---

## 8. Caso `kmscube`: ritardo alto anche senza shared memory ESA

Nel caso `kmscube`, osservando sempre il caso:

```text
CPU3 - MAX SLEEP
```

si nota che il ritardo massimo può rimanere vicino a:

```text
circa 400 us
```

ma con caratteristiche molto diverse rispetto alla HMI ESA.

In particolare:

```text
L2 cache miss ≈ 4%
CPI ≈ 7
bus_access/bus_cycles ≈ 0.002003
```

Questo è un dato molto importante.

Nel caso `kmscube`, infatti, la percentuale di L2 cache miss è molto più bassa rispetto al caso HMI ESA.

Quindi il ritardo elevato non sembra essere causato principalmente dal fatto che la CPU3 stia facendo tantissime miss L2.

La differenza principale è invece nel rapporto:

```text
bus_access/bus_cycles
```

che nel caso `kmscube` è molto basso.

---

## 9. Interpretazione del caso `kmscube`

Il rapporto:

```text
bus_access/bus_cycles ≈ 0.002003
```

indica che, durante la finestra di worst case, gli accessi bus osservati sulla CPU3 sono molto rari rispetto al numero di cicli bus.

L'inverso è spesso più intuitivo:

```text
bus_cycles/bus_access
```

Se `bus_access/bus_cycles` è molto basso, allora `bus_cycles/bus_access` è molto alto.

Questo suggerisce che ogni accesso bus osservato corrisponde a molti cicli bus.

Non è una latenza fisica esatta del singolo accesso, ma è un indicatore relativo molto utile.

La lettura più prudente è:

```text
nel caso kmscube, la CPU3 non fa molte miss L2,
ma quando deve interagire con il sottosistema memoria/bus,
questa interazione risulta molto più costosa.
```

Questo è coerente con l'ipotesi che il rendering continuo generato da `kmscube` stia occupando il sottosistema memoria.

---

## 10. Perché `kmscube` può disturbare anche con poche miss L2

Il punto fondamentale è che il ritardo non dipende solo da:

```text
quante volte la CPU3 va fuori cache
```

ma anche da:

```text
quanto costa accedere alla memoria/bus quando serve
```

Nel caso `kmscube`, la CPU3 può avere poche miss L2, ma quelle poche interazioni con il bus/memoria possono diventare costose perché la pipeline grafica sta lavorando molto.

`kmscube` genera rendering continuo:

```text
OpenGL ES / EGL / GBM
  -> GPU renderizza nuovi frame
  -> vengono aggiornati buffer grafici
  -> avvengono sincronizzazioni grafiche
  -> avvengono page flip / present
  -> il display controller legge i buffer
```

Questo genera traffico su:

- DDR;
- bus;
- interconnect;
- memory controller;
- GPU;
- driver grafici;
- DRM/KMS;
- display controller.

Anche se la CPU3 non riceve direttamente gli interrupt grafici, può comunque essere rallentata perché le risorse fisiche sono condivise.

La CPU3 deve comunque accedere a memoria per eseguire il path critico:

```text
timer interrupt
  -> hrtimer
  -> hrtimer_wakeup
  -> try_to_wake_up
  -> ttwu_do_activate
  -> scheduler
```

Questo path tocca strutture come:

- `task_struct`;
- runqueue;
- dati per-CPU;
- strutture dello scheduler;
- code hrtimer;
- lock interni;
- strutture kernel usate nel wakeup.

Se il sottosistema memoria è sotto pressione a causa del rendering, questi accessi possono richiedere più cicli.

---

## 11. Confronto concettuale tra HMI ESA e `kmscube`

La parte più interessante è che HMI ESA e `kmscube` possono produrre ritardi simili, ma per motivi diversi.

### HMI ESA con pulsanti

```text
ritardo max sleep ≈ 400 us
L2 cache miss ≈ 30%
bus_access/bus_cycles ≈ 0.046757
CPI ≈ 10
```

Interpretazione:

```text
la CPU3 va spesso fuori dalla L2
la shared memory/dati condivisi generano pressione cache
la HMI interattiva aumenta anche il rendering
le miss diventano più costose perché il bus è più occupato
```

### `kmscube`

```text
ritardo max sleep ≈ 400 us
L2 cache miss ≈ 4%
bus_access/bus_cycles ≈ 0.002003
CPI ≈ 7
```

Interpretazione:

```text
la CPU3 non fa molte miss L2
ma il sottosistema bus/memoria è fortemente penalizzato
il rendering continuo genera traffico grafico
gli accessi bus osservati sono pochi rispetto ai cicli bus
la CPU3 avanza lentamente anche senza molte miss L2
```

---

## 12. Differenza fondamentale tra i due meccanismi

Si possono distinguere due famiglie di interferenza.

### 12.1 Interferenza cache/shared memory

È più evidente nel caso HMI ESA.

Caratteristiche:

```text
L2 miss alta
shared memory presente
dati condivisi tra HMI e parte real-time
possibile working set più grande
possibile pressione sulla L2 condivisa
possibile false sharing
possibile traffico di coerenza
```

Effetto:

```text
la CPU3 deve andare spesso fuori cache
```

### 12.2 Interferenza bus/memoria/rendering

È più evidente nel caso `kmscube`.

Caratteristiche:

```text
L2 miss bassa
CPI comunque alto
bus_access/bus_cycles molto basso
rendering continuo
GPU/display/DRM/KMS attivi
traffico su DDR/bus/interconnect
```

Effetto:

```text
la CPU3 non va necessariamente spesso fuori L2,
ma quando deve accedere al sottosistema memoria,
questo accesso può essere molto più costoso.
```

---

## 13. CPI: perché è alto in entrambi i casi

Nel caso osservato:

```text
kmscube:
    CPI ≈ 7

HMI ESA:
    CPI ≈ 10
```

Entrambi sono valori alti.

Il CPI indica:

```text
CPI = cpu_cycles / inst_retired
```

quindi misura quanti cicli CPU servono mediamente per completare una istruzione.

Un CPI alto significa che la CPU3 sta avanzando lentamente.

Però il motivo può essere diverso:

```text
HMI ESA:
    CPI alto + L2 miss alta
    -> molte attese possono essere legate a cache miss/shared memory

kmscube:
    CPI alto + L2 miss bassa
    -> il rallentamento sembra più legato a bus/interconnect/memoria esterna
```

Questa distinzione è molto importante perché impedisce di attribuire tutto semplicemente alla L2 miss.

---

## 14. Perché non basta dire "il problema è la cache"

Nel caso HMI ESA, dire che il problema è "lato cache" è ragionevole, ma va precisato.

Le miss L2 alte indicano che la CPU3 spesso non trova i dati nemmeno nella L2 e deve andare verso livelli più lontani.

Tuttavia, il ritardo finale dipende anche da quanto costa andare verso memoria.

Quindi il modello corretto non è:

```text
più cache miss = automaticamente 400 us
```

ma:

```text
ritardo finale
  =
  numero di accessi fuori cache
  x
  costo medio di ogni accesso fuori cache
```

Nel caso HMI senza pulsanti:

```text
L2 miss ≈ 20%
ritardo ≈ 150 us
```

Nel caso HMI con pulsanti:

```text
L2 miss ≈ 30%
ritardo ≈ 400 us
```

La differenza non è solo il passaggio da 20% a 30%.

La differenza è anche che, premendo pulsanti, aumenta il rendering e quindi aumenta il traffico su memoria/bus.

Quindi ogni accesso fuori cache può diventare più costoso.

---

## 15. Perché HMI senza interazione ha già miss L2 alte

Una domanda importante è:

> Perché con HMI accesa ma senza interazione si vedono già miss L2 alte?

La risposta è che "senza interazione" non significa necessariamente "HMI ferma".

Anche senza premere pulsanti, una HMI industriale può continuare a:

- leggere quote assi;
- leggere stati macchina;
- leggere variabili PLC/CNC;
- aggiornare label numeriche;
- aggiornare diagnostica;
- controllare allarmi;
- fare polling di dati condivisi;
- ridisegnare alcune zone della schermata.

Quindi la HMI può continuare a generare accessi a memoria condivisa e aggiornamenti grafici leggeri.

Il caso HMI idle è più correttamente:

```text
HMI in polling/refresh periodico
```

non:

```text
HMI completamente statica
```

---

## 16. Perché il caso `modetest` aiuta a capire

I test con `modetest` hanno mostrato risultati molto simili a risoluzioni diverse, specialmente osservando la CPU3.

Questo è importante perché `modetest` mostra un framebuffer statico.

Con `modetest`:

```text
framebuffer statico
scanout del display controller
nessun rendering continuo
```

Con `kmscube`:

```text
rendering continuo
GPU attiva
buffer grafici aggiornati
page flip
sincronizzazioni
```

Il fatto che `modetest` non produca un peggioramento simile a `kmscube` indica che il problema non è semplicemente:

```text
HDMI acceso
framebuffer statico
scanout del display controller
```

Il problema emerge soprattutto quando:

```text
i buffer grafici vengono aggiornati continuamente
```

---

## 17. Framebuffer: perché `kmscube` pesa più di `modetest` a parità di risoluzione

A parità di risoluzione, per esempio 1280x720, il framebuffer finale ha circa la stessa dimensione.

A 32 bit per pixel:

```text
1280 x 720 x 4 byte ≈ 3.7 MB
```

Quindi la domanda è:

> se il framebuffer è grande uguale, perché `kmscube` dà più fastidio di `modetest`?

La risposta è:

> perché il problema non è solo quanto è grande il framebuffer finale, ma quante volte viene scritto, aggiornato, sincronizzato e scambiato.

Con `modetest`:

```text
un framebuffer statico viene mostrato
il display controller lo legge
quasi nessuno lo riscrive continuamente
```

Con `kmscube`:

```text
la GPU renderizza continuamente nuovi frame
vengono usati front buffer/back buffer/depth buffer
ci sono page flip
ci sono sincronizzazioni
ci sono scritture e letture continue
```

Quindi il framebuffer visibile può avere la stessa dimensione, ma il traffico totale generato da `kmscube` è molto maggiore.

---

## 18. Il framebuffer sta nella L2?

Il framebuffer vive normalmente in DDR.

Può succedere che alcune cache line del framebuffer passino temporaneamente nelle cache della CPU se la CPU lo accede e se il mapping è cacheable.

Però non bisogna immaginare:

```text
il framebuffer intero sta nella L2
```

È più corretto dire:

```text
il framebuffer è in DDR;
alcune sue cache line possono entrare temporaneamente in cache CPU se la CPU le legge/scrive;
GPU e display controller accedono ai buffer tramite bus/interconnect/DDR.
```

Nel caso `kmscube`, il disturbo più probabile non è che il framebuffer "riempia la L2 della CPU3", ma che GPU/display/DRM/KMS generino traffico su DDR/bus/interconnect.

---

## 19. Si può impedire al framebuffer di finire in cache?

In teoria, per gli accessi CPU, sì: una regione di memoria può essere mappata come cacheable, non-cacheable, write-combine o device memory.

Questo dipende dagli attributi delle pagine e dal driver.

Esempi concettuali nel kernel:

```text
dma_alloc_coherent
dma_alloc_wc
ioremap_wc
ioremap_nocache
pgprot_writecombine
pgprot_noncached
```

Però questo riguarda principalmente gli accessi CPU.

Nel caso `kmscube`, il problema più probabile non è che CPU3 stia cachando il framebuffer, ma che GPU e display controller generino traffico su memoria condivisa.

Quindi rendere il framebuffer non-cacheable potrebbe non risolvere il problema e, in alcuni casi, potrebbe anche peggiorare gli accessi CPU.

---

## 20. Evento perf diretto per misurare l'attesa sul bus

Sarebbe ideale avere un evento del tipo:

```text
memory_bus_wait_cycles
stall_cycles_due_to_memory
```

Tuttavia, sul Cortex-A53 non è sempre disponibile un evento PMU diretto e affidabile che dica:

```text
la CPU ha atteso N cicli per accedere alla memoria/bus
```

Per questo motivo l'analisi usa proxy indiretti:

```text
cpu_cycles
inst_retired
CPI
IPC
l2d_cache
l2d_cache_refill
bus_access
bus_cycles
mem_access
ld_retired
st_retired
```

Metriche utili:

```text
CPI = cpu_cycles / inst_retired
IPC = inst_retired / cpu_cycles

L2 miss % = l2d_cache_refill / l2d_cache * 100

bus_cycles_per_access = bus_cycles / bus_access

mem_access_per_instruction = mem_access / inst_retired
ld_retired_per_instruction = ld_retired / inst_retired
st_retired_per_instruction = st_retired / inst_retired
```

---

## 21. Modello interpretativo completo

Il modello finale può essere scritto così:

```text
ritardo finale CPU3
  =
  quante volte la CPU3 deve accedere fuori cache
  x
  costo medio di questi accessi
  +
  eventuale rallentamento generale della pipeline
```

Nel caso HMI senza pulsanti:

```text
miss L2 alte, circa 20%
rendering leggero
bus non troppo congestionato
ritardo intorno a 150 us
```

Nel caso HMI con pulsanti:

```text
miss L2 più alte, circa 30%
rendering più intenso
bus/interconnect più occupato
ritardo intorno a 400 us
```

Nel caso `kmscube`:

```text
miss L2 basse, circa 4%
rendering molto intenso
bus_access/bus_cycles molto basso
CPI alto, circa 7
ritardo intorno a 400 us
```

Quindi:

```text
HMI idle:
    prevale effetto shared memory/cache

HMI con pulsanti:
    shared memory/cache + rendering

kmscube:
    prevale effetto rendering/bus/interconnect
```

---

## 22. Conclusione generale

La conclusione più importante è che non esiste una sola causa del jitter.

I dati suggeriscono almeno tre scenari:

### HMI attiva senza interazione

```text
L2 miss alte
ritardo medio-alto
probabile effetto di shared memory / polling / aggiornamento valori
```

### HMI attiva con pulsanti

```text
L2 miss ancora più alte
rendering più intenso
ritardo molto alto
effetto combinato cache + bus occupato
```

### `kmscube`

```text
L2 miss basse
CPI alto
bus_access/bus_cycles molto basso
ritardo molto alto
probabile effetto rendering / traffico grafico / contesa DDR-bus-interconnect
```

Quindi la HMI ESA e `kmscube` possono arrivare a ritardi simili, ma attraverso meccanismi diversi.

Nel caso HMI ESA il problema sembra più vicino a:

```text
cache
shared memory
coerenza
working set
false sharing
```

Nel caso `kmscube` il problema sembra più vicino a:

```text
rendering continuo
GPU
DRM/KMS
buffer grafici
DDR
bus
interconnect
```

---
