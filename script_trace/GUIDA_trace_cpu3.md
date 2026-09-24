# Guida — tracciare la catena RT e leggerla con `Ordina_trace_cpu3`

> Script e trace stanno in **`tracing_Imx8plus/script_trace/`**.
> Trace già raccolti: `trace.txt` (interfaccia spenta, 5 s) · `trace_interfaccia.txt` (GUI attiva, 60 s) · `trace_interfaccia_scrolling.txt` (GUI + scroll grafico 2D, 60 s)

Serve a rispondere a una domanda sola, ma decisiva:
**chi occupa la CPU fra il momento in cui un thread viene svegliato e il momento in cui gira davvero?**

È lo strumento che il 23/09 ha chiuso in un colpo un'anomalia su cui erano cadute quattro ipotesi:
i ~40 µs fra la post del semaforo e la partenza di `PLC FAST` **non erano latenza**, era il
regolatore `MainRegol` che ha priorità più alta e gira in mezzo a ogni ciclo.

---

## Parte 1 — registrare sul target

### Cosa registrare

Bastano due eventi. Non aggiungerne altri: il file cresce in fretta.

| evento | cosa dice |
|---|---|
| `sched_wakeup` | il kernel ha reso **eseguibile** un thread |
| `sched_switch` | un thread **lascia** la CPU e un altro la **prende** |

### Il comando

```bash
trace-cmd record -e sched:sched_wakeup -e sched:sched_switch sleep 60
```

Registra per 60 secondi e scrive `trace.dat` nella cartella corrente.

⚠️ **Salvare in `/root`, non in `/tmp`**: `/tmp` è tmpfs e si perde al riavvio.

### Quanto registrare

| durata | cicli PLC FAST | `trace.dat` | testo convertito |
|---|---:|---:|---:|
| 5 s | ~1 250 | ~2 MB | ~12 MB |
| **60 s** | **~15 000** | ~25 MB | ~200 MB |

⇒ **60 secondi è la durata giusta.** Dà ~15 000 cicli, che è lo stesso ordine dei campioni
della strumentazione applicativa e permette di confrontare anche i percentili alti.

⚠️ **Campagne di durata diversa non hanno massimi confrontabili.** Se confronti due condizioni
(per esempio interfaccia spenta e accesa), registrale **per lo stesso tempo**. È l'errore in cui
si cade più spesso: un trace da 5 s contro uno da 60 s sembra dire che la condizione corta è
migliore, mentre ha solo dodici volte meno estrazioni.

### Convertire in testo

`trace.dat` è binario e si legge solo con `trace-cmd`, che sta **sul target**:

```bash
trace-cmd report trace.dat | grep -E "sched_wakeup|sched_switch" > /root/trace.txt
```

Se il file resta troppo grande, restringere già qui ai thread che interessano:

```bash
trace-cmd report trace.dat | grep -E "PLC FAST|PLCScheduler|MainRegol" > /root/trace.txt
```

⚠️ Filtrando per nome si perdono i thread **non previsti** — ed è proprio quello che si sta
cercando quando un tempo cresce senza spiegazione. Filtrare solo se serve davvero.

### Portarlo sulla macchina di sviluppo

```bash
scp root@10.10.51.110:/root/trace.txt /mnt/c/Leonardo/tesi/tracing_Imx8plus/script_trace/
```

---

## Parte 2 — leggerlo con `Ordina_trace_cpu3`

```bash
cd /mnt/c/Leonardo/tesi/tracing_Imx8plus/script_trace
./Ordina_trace_cpu3 trace.txt
```

Lo script tiene **solo il core 3** (quello dei thread real-time), spezza il flusso in **cicli** —
uno per ogni risveglio di `PLC FAST` — e riscrive ogni riga con lo **scarto in µs** da quell'istante.

### Le opzioni

| opzione | cosa fa |
|---|---|
| *(nessuna)* | i primi 3 cicli, più il riepilogo |
| `-n 10` | i primi 10 cicli |
| **`--worst 5`** | ⭐ i 5 cicli con l'**attesa più lunga** — quasi sempre è questa che serve |
| `--raw` | le righe originali di `trace-cmd`, col delta davanti |
| `--cpu 2` | un core diverso |
| `--target "PLC SLOW"` | un altro thread bersaglio |

I nomi dei thread real-time su questo sistema:

```
COM RTC Handler    il tick (clock_nanosleep)
PLCScheduler       chiama PlcManager e posta i semafori dei PLC
MainRegol          regolatore dello slave        ← priorità PIU' ALTA di PLC FAST
PLC FAST           logica PLC veloce
MainSlave          calcolo slave
PLC SLOW           logica PLC lenta
```

---

## Parte 3 — come si legge l'output

```
════ ciclo 1 ═════════════════════════════════════════════
     t0 = 18216.733223   risveglio di «PLC FAST»   ·   va in esecuzione dopo 198 us
        quando   chi gira           cosa fa
     ──────────────────────────────────────────────────────
       +0 us   PLCScheduler       sveglia     ·    PLC FAST
      +14 us   PLCScheduler       sveglia     ·    PLC SLOW
      +34 us   PLCScheduler       SI BLOCCA   →    MainRegol
     +167 us   MainRegol          sveglia     ·    MainSlave
     +198 us   MainRegol          SI BLOCCA   →    PLC FAST  ⭐ PARTE
     +271 us   PLC FAST           SI BLOCCA   →    MainSlave
```

### Le due righe hanno significati diversi

| | significato |
|---|---|
| `SI BLOCCA →` | **cambio di contesto**: quello a sinistra cede la CPU, quello a destra la prende |
| `sveglia ·` | rende eseguibile un altro thread, ma **continua a girare lui** |

🔑 **Una riga `sveglia` non interrompe niente.** Fra `+34` e `+198` gira `MainRegol`, anche se
a `+167` compare una riga che nomina `MainSlave`.

### Durata di esecuzione di un thread

> **Intervallo fra lo switch che lo porta dentro e lo switch che lo porta fuori.**
> Le righe `sveglia` in mezzo non contano.

Nell'esempio:

```
MainRegol   +34 → +198   =  164 µs
PLC FAST   +198 → +271   =   73 µs   ← durata della scansione PLC
```

### Il riepilogo finale

```
ATTESA dal risveglio all'esecuzione di «PLC FAST» — 15002 cicli
   min=28  mediana=54  p95=81  p99=96  max=198 us

CHI occupa il core in quell'attesa:
   MainRegol                  41.85 us/ciclo
   (chi ha svegliato)         12.78 us/ciclo
```

⇒ **La riga «CHI occupa il core» è la più importante.** Se compare un nome che non ti aspetti,
è lì la risposta. È esattamente così che è saltato fuori `MainRegol`.

---

## Parte 4 — i limiti, da conoscere prima di trarre conclusioni

### ⚠️ 1. Il timestamp non è l'istante in cui il codice gira

`sched_switch` è emesso dentro `__schedule()`, **prima** del cambio di contesto vero. Dopo restano
lo scambio di tabelle delle pagine, quello di registri e stack, e il ritorno dalla syscall.

⇒ Fra «switch registrato» e «prima istruzione del thread» passano **alcuni microsecondi**.
La misura applicativa `HOP1` (timestamp presi nel codice dei due thread) dice che l'intero
percorso blocco → esecuzione vale **~12,8 µs**.

**Conseguenza:** va benissimo per dire *«il regolatore ha girato 164 µs»*. **Non** va bene per
misurare un cambio di contesto da 2 µs: lì l'errore è dello stesso ordine della grandezza.

### ⚠️ 2. Il trace dice CHI ha la CPU, non QUANTO VELOCE esegue

```
MainRegol:  18 µs nel caso migliore  ·  164 µs nel peggiore
```

Stesso codice, stessa CPU tutta per sé. Ftrace registra la durata ma **non può dire perché**.

⇒ Il perché è l'interferenza sulla cache L2 condivisa: il thread ha la CPU ma sta aspettando la
memoria. Per dimostrarlo servono i **contatori PMU** (`PerfMonitor`, `l2d_cache_refill`
**a istruzioni costanti**), non ftrace.

```
ftrace  →  CHI ha la CPU, e per quanto tempo
PMU     →  PERCHÉ quel tempo è lungo
```

### ⚠️ 3. Registrare perturba

Il tracciamento ha un costo. Non è grande con due soli eventi, ma esiste: non confrontare un
trace con una campagna fatta senza.

---

## Ricetta rapida

```bash
# 1. sul target
trace-cmd record -e sched:sched_wakeup -e sched:sched_switch sleep 60
trace-cmd report trace.dat | grep -E "sched_wakeup|sched_switch" > /root/trace.txt

# 2. portarlo di qua
scp root@10.10.51.110:/root/trace.txt /mnt/c/Leonardo/tesi/tracing_Imx8plus/script_trace/

# 3. guardare i casi peggiori
cd /mnt/c/Leonardo/tesi/tracing_Imx8plus/script_trace
./Ordina_trace_cpu3 trace.txt --worst 5
```

📌 Per confrontare due condizioni: **stessa durata di registrazione**, e leggere **mediana, p95 e
p99**, non i massimi.
