# carico_gui — carico GUI riproducibile per le campagne RT

Registra i tuoi gesti dal touchscreen reale e li ripete identici, per ore, su un
touchscreen virtuale creato via `uinput`.

**Perché serve:** le campagne RT confrontano due configurazioni e richiedono che
il carico sia identico nei due bracci. Con il carico manuale questo non è mai
stato vero, e ha già invalidato tre misure:

- A/B su `PEG_DRM_FULLWIDTH_PCT`: inconcludente, la variabilità fra sessioni a
  parità di configurazione (616 → 809 µs per blit) superava l'effetto cercato
- test `COND_SYNC` da 30 minuti: l'interfaccia era in gran parte ferma
- confronti da 3 minuti: carico "uguale" solo per dichiarazione

---

## ⚠️ Sicurezza — leggere prima

Questa è l'HMI di una **pressa piegatrice**. Le icone della toolbar cambiano
stato macchina e, per AUTO e SAUTO, passano da `AttivaMenu` →
`ConfigButtonsStartStopPlusMinus`, cioè il cablaggio dei tasti fisici
**Start / Stop / Plus / Minus**. È il vincolo non negoziabile del `CLAUDE.md`.

- **La registrazione è il momento in cui decidi.** Tocchi tu, vedi cosa succede,
  e ciò che registri è esattamente ciò che verrà ripetuto centinaia di volte.
  Non registrare gesti che non vuoi veder ripetuti per un'ora.
- **Mai lasciare una riproduzione in esecuzione su una macchina in pressione.**
  Banco di prova, o macchina non alimentata.

---

## Compilazione

Cross da WSL, senza bisogno di compilatore sul target:

```bash
bash -c '. /opt/avnet/imx8mp/environment-setup-armv8a-poky-linux; $CC -O2 -Wall -o carico_gui carico_gui.c'
```

Oppure nativa, se il target ha `gcc`:

```bash
gcc -O2 -o carico_gui carico_gui.c
```

Copia sul target — **non** in `/opt/Squeeze`, che è l'applicazione deployata:

```bash
scp carico_gui root@avn8mp:/root/
```

Serve il modulo `uinput` e i permessi di root:

```bash
modprobe uinput && ls -l /dev/uinput
```

---

## 1. Registrare

Con l'HMI in esecuzione, così vedi cosa stai facendo:

```bash
./carico_gui --registra sessione.rec
```

Trova da solo il touchscreen reale e lo stampa (sul target è l'ILITEK,
`/dev/input/event2`). Se non lo trovasse:

```bash
./carico_gui --registra sessione.rec --da /dev/input/event2
```

Da quel momento **tocca l'interfaccia come vuoi che venga ripetuta**: cambi
pagina, scroll liste, trascinamenti del grafico 2D. Il contatore eventi avanza a
video. `Ctrl+C` per chiudere.

La registrazione **non crea nessun device virtuale**: legge soltanto, quindi non
interferisce con l'HMI.

Registra **almeno 30-60 secondi** di interazione varia: verrà ripetuta in ciclo,
e una registrazione troppo corta produce un carico innaturalmente periodico.

---

## 2. Riprodurre

```bash
./carico_gui --riproduci sessione.rec --minuti 60
```

Gli eventi vengono riemessi **con le tempistiche originali**, ricostruite dai
timestamp registrati: le pause fra un gesto e l'altro sono le tue. Una pausa più
lunga di **2 secondi** viene però troncata a 2, così un'esitazione durante la
registrazione non diventa tempo morto moltiplicato per centinaia di cicli.

Al primo ciclo stampa quanti eventi emette e quanti ne scarta:

```
carico_gui: 4312 eventi emessi, 118 scartati per ciclo
```

Qualche scarto è normale (codici che il pannello emette e il device virtuale non
dichiara, e che l'app comunque ignora). **Se sono scartati quasi tutti**, il
pannello usa codici diversi da quelli previsti: va esteso `codice_supportato()`
nel sorgente.

---

## 3. Collegare l'HMI al device virtuale

Con `carico_gui` **già in esecuzione** (l'ordine conta: `pegdrm_evdev` apre il
device all'inizializzazione), trova il numero:

```bash
grep -A5 -i carico-gui /proc/bus/input/devices | grep -o "event[0-9]*"
```

⚠️ In `/proc/bus/input/devices` il nome (`N:`) e gli handler (`H:`) stanno su
righe **diverse** dello stesso blocco: cercare solo il nome non restituisce il
numero. Serve il `-A5`.

Verifica che gli eventi escano davvero, così distingui "il generatore non emette"
da "l'app non legge":

```bash
timeout 3 cat /dev/input/eventN > /tmp/ev.bin; ls -l /tmp/ev.bin
```

Qualche kilobyte = tutto a posto. File vuoto = problema nel generatore.

Poi avvia l'HMI forzando quel device, così non c'è ambiguità con il touchscreen
reale — usando il numero **vero**, non il segnaposto:

```bash
PEGDRM_TOUCH_DEV=/dev/input/event5 PEG_DRM_SYNC_STATS=0 ./PegExec
```

---

## 4. Campagna A/B completa

**Braccio A:**

```bash
./carico_gui --riproduci sessione.rec --minuti 60      # shell 1
PEGDRM_TOUCH_DEV=/dev/input/eventN PEG_DRM_SYNC_STATS=0 ./PegExec   # shell 2
./Lnk                                                  # shell 3
```

A fine ora, termina `Lnk` con **SIGTERM** — `Ctrl+C` non è gestito e fa perdere
il dump finale e i CSV:

```bash
kill -TERM $(pidof Lnk)
```

**Braccio B:** cambia **una sola** variabile (es. commenta
`EMBEDDED_HMI_RT_DRM_DIRECT` in `PegLib.pro:12`), ricompila **con il `touch`**
sui due sorgenti che usano le macro, rideploya, e ripeti con **la stessa
registrazione**.

```bash
touch pegenstein/PegLib/peglvglwindow.cpp pegenstein/PegLib/peg_run.cpp
```

Elenco completo degli interruttori: sezione *Interruttori: variabili d'ambiente
e define di build* del `registro_test_rt.md`.

---

## Modalità di ripiego: sequenza sintetica

Se non puoi registrare, il sorgente contiene una sequenza scritta a mano
(`g_sequenza[]`), fatta di soli trascinamenti nell'area di contenuto:

```bash
./carico_gui --lista                  # la stampa senza eseguirla
./carico_gui --sintetica --minuti 60
```

Per capire cosa c'è in un punto prima di aggiungere un tap, **uno alla volta**:

```bash
./carico_gui --probe 300 320
```

Coordinate in **pixel di schermo** (1024 × 600): la conversione al range del
pannello (0..16384 × 0..9600) è un fattore 16 esatto, gestita dal programma.

Riferimento utile: l'area **(0,117)-(621,521)** è il pannello di contenuto
principale — è il rettangolo che la diagnostica `[DIRTY-OUT]` ha visto
invalidato 232 volte durante il lavoro sul grafico 2D.

---

## Trappole

**Due touchscreen presenti.** Con quello reale *e* quello virtuale attivi, il
path SDL potrebbe agganciare quello sbagliato. Sul path DRM si forza con
`PEGDRM_TOUCH_DEV`; sul path SDL **va verificato**. Nel dubbio, scollegare o
disabilitare il touch reale durante la campagna.

**Ordine di avvio.** `carico_gui` prima di `PegExec`, sempre.

**I numeri non sono confrontabili con i test manuali precedenti.** È una serie
nuova. Il vantaggio è che d'ora in poi è ripetibile a distanza di mesi.

**Verifica che il carico sia arrivato.** Se le fasce basse dell'istogramma sono
quasi vuote, l'HMI non stava ricevendo eventi: device sbagliato, ordine di
avvio, o `INPUT_PROP_DIRECT` non riconosciuto.

**Conserva il file `.rec` insieme ai risultati.** È parte della configurazione
del test tanto quanto le variabili d'ambiente: senza, la campagna non è
ripetibile.

---

## Controllo di validità, dai dati stessi

Anche col carico automatico, vale la pena mantenere il controllo che ha reso
solida la tabella B del 2026-09-17: **se le fasce sotto i 100 µs coincidono fra
i due bracci, il carico era equivalente**.

A mano avevano coinciso entro il 2%. Con `carico_gui` dovrebbero coincidere
molto meglio — e se non lo fanno, è cambiato qualcosa oltre alla variabile in
esame.
