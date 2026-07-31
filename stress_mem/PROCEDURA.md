# Esperimento decisivo — la L2 condivisa è il canale del jitter?

**Obiettivo:** dimostrare causalmente che il jitter del thread RT su CPU3 è prodotto
dall'interferenza sulla **cache L2 condivisa** del cluster Cortex-A53, e non dalla
banda DDR né dallo scheduling.

Tutte le misure fatte finora sono **correlazionali**: la GUI gira e il jitter aumenta.
Questo esperimento rimuove completamente la GUI e lascia solo il traffico di memoria.
Se il jitter si riproduce, la catena causale è chiusa.

---

## Perché serve

`isolcpus=3` isola lo **scheduler**, non la **cache**. Sull'i.MX8M Plus i quattro
Cortex-A53 stanno in un unico cluster e condividono la L2. Il working set del thread
RT su CPU3 può quindi essere sfrattato da qualunque cosa giri su CPU0-2 — inclusi
i megabyte di pixel che la GUI copia a ogni frame.

Le misure PMU mostrano già la firma: a **istruzioni identiche** (567 692 contro
567 603, differenza dello 0,016 %), un'iterazione con **+685 refill L2** ha speso
**+145 505 cicli** e ha accumulato **+30 µs di ritardo**. Stesso codice, più miss,
più stallo. Manca solo la prova che sia la GUI a causare quelle miss.

---

## Le tre modalità e cosa distinguono

| Modalità | Buffer | Banda DDR | Sfratto L2 | Ruolo |
|---|---|---|---|---|
| `l2fit` | 64 KB | ~nulla | ~nullo | **controllo negativo** |
| `stream` | 8 MB | alta | alto | **trattamento** |
| `thrash` | 8 MB, passo 64 B, con pausa | **bassa** | alto | **discriminante** |

`thrash` è la modalità che vale la tesi: sfratta l'intera L2 ripetutamente
**muovendo pochissimi byte**. Se produce jitter, la banda è scagionata e resta
solo lo sfratto della cache.

---

## Compilazione

Sulla board:

```bash
gcc -O2 -o stress_mem stress_mem.c -lpthread
```

In cross con l'SDK Yocto:

```bash
source /opt/fsl-imx-.../environment-setup-aarch64-poky-linux
$CC -O2 -o stress_mem stress_mem.c -lpthread
scp stress_mem root@<board>:/tmp/
```

---

## Prima di iniziare — verifiche

```bash
# 1. Confermare che la L2 è condivisa fra i 4 core
cat /sys/devices/system/cpu/cpu3/cache/index2/level          # atteso: 2
cat /sys/devices/system/cpu/cpu3/cache/index2/shared_cpu_list # atteso: 0-3  <-- il punto
cat /sys/devices/system/cpu/cpu3/cache/index2/size            # dimensione L2

# 2. Confermare l'isolamento dello scheduler
cat /sys/devices/system/cpu/isolated                          # atteso: 3

# 3. Nessuna GUI in esecuzione
ps aux | grep -i -E "pegmain|weston|Xorg"
```

Se `shared_cpu_list` è `0-3`, il meccanismo ipotizzato è **fisicamente possibile**
e l'esperimento ha senso. Se fosse `3` soltanto, l'ipotesi cade subito e si risparmia
una giornata.

---

## Procedura

Per ogni fase: avviare `Lnk` **senza HMI**, lasciarlo girare almeno **5 minuti**
(oltre le 75 000 iterazioni a 4 ms, ben oltre il warm-up di 15 000), poi `SIGTERM`
per far stampare le statistiche PerfMonitor e salvare i CSV.

### Fase 0 — baseline

```bash
./Lnk &                       # nessuna GUI, nessuno stress
sleep 300
kill -TERM %1
cp /tmp/perf_rt.csv /tmp/perf_rt_worst.csv ./fase0_baseline/
```

### Fase 1 — controllo negativo (`l2fit`)

```bash
./Lnk &
/tmp/stress_mem l2fit &       # 3 thread su CPU0-2, buffer 64 KB
sleep 300
kill -TERM %2 ; kill -TERM %1
```

**Atteso:** distribuzione **identica** alla baseline. Le CPU0-2 sono al 100 % di
carico ma non toccano la DRAM e non sfrattano quasi nulla.
Se qui il jitter aumenta, la causa non è la memoria ma il carico di CPU in sé
(alimentazione, frequenza, interrupt) e va indagata diversamente.

### Fase 2 — trattamento (`stream`)

```bash
./Lnk &
/tmp/stress_mem stream &
sleep 300
kill -TERM %2 ; kill -TERM %1
```

**Atteso:** iterazioni `> 25 µs` in aumento nell'ordine del **+60 %**, come sotto
carico GUI. Questa è la riproduzione del fenomeno senza una sola riga di codice
grafico.

### Fase 3 — discriminante (`thrash`)

```bash
./Lnk &
/tmp/stress_mem thrash &      # pausa 200 us: banda bassa, sfratto alto
sleep 300
kill -TERM %2 ; kill -TERM %1
```

Ripetere con `thrash 8 3 1000` (pausa 1 ms) per abbassare ancora la banda.
Annotare la banda media stampata da `stress_mem` a fine corsa.

**Atteso se l'ipotesi è giusta:** jitter presente **anche a banda molto bassa**.

---

## Come leggere il risultato

| `l2fit` | `stream` | `thrash` | Conclusione |
|---|---|---|---|
| no | **sì** | **sì** | **Sfratto della L2 condivisa** — ipotesi confermata |
| no | sì | no | Saturazione della banda DDR |
| sì | sì | sì | Non è la memoria: è il carico di CPU in sé |
| no | no | no | Ipotesi da rifare: la GUI agisce per un'altra via |

Il confronto che conta davvero è **`stream` contro `thrash`**: stessa quantità di
sfratto, banda molto diversa. Se il jitter è simile, la banda non c'entra.

---

## Curva dose-risposta (opzionale, ma vale molto in tesi)

Ripetere `stream` variando la dimensione del buffer attorno alla dimensione della L2:

```bash
for MB in 1 2 4 8 16; do
    ./stress_mem stream $MB 3 &
    # ... 5 minuti di Lnk, salvare i CSV ...
done
```

Se il jitter mostra un **ginocchio** quando il buffer supera la dimensione della L2,
la relazione causale è dimostrata in modo difficilmente contestabile: è la firma
tipica del cache thrashing e non la spiega nessun'altra ipotesi.

---

## Cosa registrare per ogni fase

Dai CSV di PerfMonitor:

- numero di iterazioni `> 25 µs`, `> 55 µs`, massimo assoluto
- `cpu_cycles`, `istruzioni`, `l2d_cache_refill` medi per fascia di ritardo
- la banda media stampata da `stress_mem`

Il confronto va fatto **a istruzioni costanti**, come per il confronto #2/#3:
è quello che rende il dato inattaccabile.

---

## Nota metodologica

`bus_cycles` **non va usato** come indicatore di occupazione del bus. Su tre campioni
indipendenti il rapporto `bus_cycles / cpu_cycles` vale 0,5026 / 0,5019 / 0,5017:
è semplicemente un clock a metà della frequenza CPU, cioè un contatore di tempo.
Non contiene informazione sull'attività del bus, e ogni metrica derivata da esso
(`bus_cycles/bus_access`) misura solo la densità degli accessi, non la contesa.

Allo stesso modo, il **CPI non va confrontato fra campioni con conteggi di istruzioni
diversi**: è un rapporto, e un denominatore che cambia di 3× lo rende privo di
significato. Confrontare sempre a istruzioni costanti.
