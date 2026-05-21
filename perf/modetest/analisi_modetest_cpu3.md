# Analisi dei test `modetest` su CPU3: 1280x720 vs 640x480

Questo documento confronta due prove eseguite con `modetest` a risoluzioni differenti:

- `modetest_1280x720`
- `modetest_640x480`

Il confronto dei contatori viene fatto **solo sulla CPU3**, perché nel test real-time la CPU3 è la CPU più interessante: è quella associata alla parte critica del wakeup/attivazione real-time.

L'obiettivo è capire se il semplice **scanout di un framebuffer statico** a risoluzioni diverse sia sufficiente a generare differenze importanti sulla CPU3, oppure se il problema osservato con `kmscube` sia più legato al **rendering continuo**.

---

## 1. Contesto del test

`modetest` imposta una modalità video tramite DRM/KMS e mantiene a schermo un framebuffer statico.

Con `modetest`:

```text
DRM/KMS imposta una modalità video
  -> viene mostrato un framebuffer statico
  -> il display controller continua a fare scanout
  -> non c'è rendering 3D continuo
  -> non c'è aggiornamento continuo dei buffer grafici
```

Questo è diverso da `kmscube`, dove invece la GPU renderizza continuamente nuovi frame.

Quindi `modetest` serve a isolare il costo del **display statico** dal costo del **rendering continuo**.

---

## 2. Nota sui valori globali della nanosleep

Il valore della `nanosleep` è un dato globale dell'iterazione, non un contatore specifico della CPU3.

Per completezza, nei due test i valori globali principali risultano:

| Metrica globale | modetest 1280x720 | modetest 640x480 |
|---|---:|---:|
| Nanosleep media [us] | 15.2627 | 15.2083 |
| Nanosleep max [us] | 25 | 21 |
| Nanosleep min [us] | 15 | 15 |

Questi valori servono solo come riferimento temporale.  
Le tabelle successive confrontano invece i **performance counter della CPU3**.

---

## 3. Medie dei contatori sulla CPU3

| Metrica | modetest 1280x720 | modetest 640x480 | Delta 640-1280 | Delta % |
|---|---:|---:|---:|---:|
| Iterazioni valide CPU3 | 10001 | 10001 | 0 | 0 % |
| Media cpu_cycles CPU3 | 155698 | 151515 | -4183 | -2.69 % |
| Media istruzioni CPU3 | 101358 | 98643 | -2715 | -2.68 % |
| Media IPC CPU3 | 0.6511 | 0.6512 | 0.0001 | 0.01 % |
| Media CPI CPU3 | 1.5361 | 1.5360 | -0.0001 | -0.01 % |
| Media L2 miss CPU3 [%] | 0.2385 | 0.2245 | -0.0140 | -5.87 % |
| Media bus_access/bus_cycles CPU3 | 0.000520 | 0.000479 | -0.000041 | -7.88 % |
| Media bus_cycles/bus_access CPU3 | 3618.7267 | 4107.9543 | 489.2276 | 13.52 % |

---

## 4. Confronto CPU3 nell'iterazione con `MAX SLEEP`

| Metrica | modetest 1280x720 | modetest 640x480 | Delta 640-1280 | Delta % |
|---|---:|---:|---:|---:|
| Iterazione max sleep | 9429 | 0 | -9429 | -100 % |
| Ritardo max sleep [us] | 25 | 21 | -4 | -16 % |
| cpu_cycles CPU3 | 212168 | 173581 | -38587 | -18.19 % |
| istruzioni CPU3 | 101358 | 87734 | -13624 | -13.44 % |
| IPC CPU3 | 0.4777 | 0.5054 | 0.0277 | 5.80 % |
| CPI CPU3 | 2.0933 | 1.9785 | -0.1148 | -5.48 % |
| L2 miss CPU3 [%] | 10.0473 | 9.6809 | -0.3664 | -3.65 % |
| bus_access CPU3 | 1700 | 1436 | -264 | -15.53 % |
| bus_cycles CPU3 | 108289 | 89076 | -19213 | -17.74 % |
| bus_access/bus_cycles CPU3 | 0.015699 | 0.016121 | 0.000422 | 2.69 % |
| bus_cycles/bus_access CPU3 | 63.6994 | 62.0306 | -1.6688 | -2.62 % |

---

## 5. Confronto CPU3 nell'iterazione con `MIN SLEEP`

| Metrica | modetest 1280x720 | modetest 640x480 | Delta 640-1280 | Delta % |
|---|---:|---:|---:|---:|
| Iterazione min sleep | 2 | 2 | 0 | 0 % |
| Ritardo min sleep [us] | 15 | 15 | 0 | 0 % |
| cpu_cycles CPU3 | 157667 | 156567 | -1100 | -0.70 % |
| istruzioni CPU3 | 101709 | 99131 | -2578 | -2.53 % |
| IPC CPU3 | 0.6451 | 0.6332 | -0.0119 | -1.85 % |
| CPI CPU3 | 1.5502 | 1.5794 | 0.0292 | 1.88 % |
| L2 miss CPU3 [%] | 0.8061 | 0.9104 | 0.1043 | 12.94 % |
| bus_access CPU3 | 140 | 152 | 12 | 8.57 % |
| bus_cycles CPU3 | 81008 | 80494 | -514 | -0.63 % |
| bus_access/bus_cycles CPU3 | 0.001728 | 0.001888 | 0.000160 | 9.26 % |
| bus_cycles/bus_access CPU3 | 578.6286 | 529.5658 | -49.0628 | -8.48 % |

---

## 6. Interpretazione del confronto su CPU3

Il confronto sulla CPU3 mostra che i due test `modetest` hanno un comportamento molto simile.

Nel caso medio, i valori di `IPC` e `CPI` sono praticamente identici:

```text
IPC medio CPU3:
  1280x720 = 0.651114
  640x480  = 0.651192

CPI medio CPU3:
  1280x720 = 1.536113
  640x480  = 1.535999
```

Anche nell'iterazione con `MAX SLEEP`, il peggioramento rimane contenuto: il worst case è basso in entrambi i casi e i contatori della CPU3 restano dello stesso ordine di grandezza.

Questo è molto diverso da quanto osservato con `kmscube`, dove la riduzione della risoluzione produceva un miglioramento molto più marcato, soprattutto su:

```text
cpu_cycles
IPC
CPI
bus_cycles
ritardo massimo della nanosleep
```

---

## 7. Cosa dimostra questo test

Il risultato suggerisce che il semplice scanout di un framebuffer statico non sia il principale responsabile del jitter elevato.

Se il problema fosse causato soprattutto dal fatto che il display controller legge un framebuffer più grande, ci si aspetterebbe una differenza più evidente tra:

```text
modetest 1280x720
```

e:

```text
modetest 640x480
```

Invece il comportamento della CPU3 rimane molto simile.

Quindi il risultato rafforza questa ipotesi:

> Il problema non è principalmente il display statico o l'HDMI acceso, ma il rendering continuo e l'aggiornamento dei buffer grafici.

---

## 8. Collegamento con `kmscube`

Con `kmscube`, la GPU renderizza continuamente.

Il flusso è circa:

```text
OpenGL ES / EGL / GBM
  -> GPU renderizza un nuovo frame
  -> vengono aggiornati buffer grafici
  -> avviene lo swap/present del frame
  -> aumenta il traffico su DDR, bus e interconnect
```

Con `modetest`, invece:

```text
framebuffer statico
  -> scanout continuo del display controller
  -> nessun rendering continuo
```

Il fatto che `modetest` sia stabile anche cambiando risoluzione indica che il traffico generato dal solo scanout non basta a spiegare il problema.

La parte critica sembra emergere quando ci sono:

- rendering continuo;
- GPU attiva;
- aggiornamento dei buffer;
- sincronizzazioni grafiche;
- page flip / present;
- traffico memoria più intenso.

---

## 9. Conclusione

Il confronto tra `modetest_1280x720` e `modetest_640x480`, limitato alla CPU3, mostra risultati molto simili.

Questo rafforza la teoria secondo cui il jitter elevato osservato con carichi grafici pesanti non è causato dal semplice framebuffer statico o dal solo scanout HDMI.

La conclusione principale è:

> Il display statico tramite `modetest`, anche a risoluzioni diverse, non produce differenze significative sulla CPU3. Il problema sembra quindi più legato al rendering continuo, all'attività GPU e all'aggiornamento dei buffer grafici, come avviene con `kmscube`.

---

## 10. Frase sintetica da usare nel report

> Nei test con `modetest`, il confronto tra 1280x720 e 640x480 mostra valori molto simili sulla CPU3. Questo indica che il semplice scanout di un framebuffer statico non è sufficiente a generare il jitter elevato osservato con `kmscube`. La differenza principale sembra quindi essere il rendering continuo, che genera traffico memoria/bus e può rallentare il path real-time.
