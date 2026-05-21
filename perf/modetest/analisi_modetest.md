# Analisi dei test `modetest`: 1280x720 vs 640x480

Questo documento analizza due prove eseguite con `modetest` a risoluzioni differenti:

- `modetest_1280x720`
- `modetest_640x480`

L'obiettivo è capire se il semplice **scanout di un framebuffer statico** ad alta risoluzione sia sufficiente a generare jitter significativo sulla `clock_nanosleep()`, oppure se il problema osservato nei test con `kmscube` sia più legato al **rendering continuo**.

---

## 1. Contesto del test

`modetest` viene usato per impostare una modalità video tramite DRM/KMS e mantenere a schermo un'immagine/framebuffer statico.

Questo è diverso da `kmscube`.

Con `modetest`:

```text
DRM/KMS imposta una modalità video
  -> viene mostrato un framebuffer statico
  -> il display controller continua a fare scanout
  -> non c'è rendering 3D continuo
  -> non c'è cubo che ruota
  -> non c'è aggiornamento continuo dei buffer grafici
```

Con `kmscube`, invece:

```text
OpenGL ES/EGL/GBM
  -> GPU renderizza continuamente nuovi frame
  -> vengono aggiornati buffer grafici
  -> avvengono swap/present dei frame
  -> aumenta il traffico su memoria/bus/interconnect
```

Per questo motivo il confronto tra `modetest` e `kmscube` è molto utile: permette di separare il costo del **display statico** dal costo del **rendering grafico continuo**.

---

## 2. Risultati sintetici

| Metrica | modetest 1280x720 | modetest 640x480 | Delta 640-1280 | Delta % |
|---|---:|---:|---:|---:|
| Iterazioni valide | 10001 | 10001 | 0 | 0 % |
| Nanosleep media [us] | 15.2627 | 15.2083 | -0.0544 | -0.36 % |
| Nanosleep max [us] | 25 | 21 | -4 | -16 % |
| Nanosleep min [us] | 15 | 15 | 0 | 0 % |
| Media cpu_cycles totale | 1045542 | 1047917 | 2375 | 0.23 % |
| Media istruzioni totale | 640604 | 638683 | -1921 | -0.30 % |
| Media IPC totale | 0.6130 | 0.6100 | -0.0031 | -0.50 % |
| Media CPI totale | 1.6320 | 1.6404 | 0.0084 | 0.52 % |
| Media L2 miss [%] | 0.2713 | 0.2814 | 0.0101 | 3.72 % |
| Media bus_access/bus_cycles | 0.001045 | 0.001059 | 0.000014 | 1.34 % |
| Media bus_cycles/bus_access | 1443.7640 | 1522.0383 | 78.2743 | 5.42 % |

---

## 3. Confronto del caso peggiore della `nanosleep`

| Metrica | modetest 1280x720 | modetest 640x480 | Delta 640-1280 | Delta % |
|---|---:|---:|---:|---:|
| Iterazione max sleep | 9429 | 0 | -9429 | -100 % |
| Ritardo max sleep [us] | 25 | 21 | -4 | -16 % |
| cpu_cycles | 3406368 | 1378428 | -2027940 | -59.53 % |
| istruzioni | 1557644 | 758687 | -798957 | -51.29 % |
| IPC | 0.4573 | 0.5504 | 0.0931 | 20.37 % |
| CPI | 2.1869 | 1.8169 | -0.3700 | -16.92 % |
| L2 miss [%] | 9.2932 | 3.4044 | -5.8888 | -63.37 % |
| bus_access | 34183 | 6258 | -27925 | -81.69 % |
| bus_cycles | 1714423 | 704303 | -1010120 | -58.92 % |
| bus_access/bus_cycles | 0.019938 | 0.008885 | -0.011053 | -55.44 % |
| bus_cycles/bus_access | 50.1543 | 112.5444 | 62.3901 | 124.40 % |

---

## 4. Lettura dei risultati

I due test con `modetest` producono risultati molto simili.

La media della `nanosleep` è praticamente uguale:

```text
1280x720: 15.2627 us
640x480 : 15.2083 us
```

Anche il worst case rimane basso in entrambi i casi:

```text
1280x720: 25 us
640x480 : 21 us
```

La differenza è piccola, soprattutto se confrontata con i risultati ottenuti con `kmscube`, dove il worst case saliva fino a centinaia di microsecondi.

Questo indica che il semplice cambio di risoluzione del framebuffer statico non produce un peggioramento importante del comportamento real-time.

---

## 5. Interpretazione tecnica

Il risultato più importante è che `modetest` a `1280x720` e `modetest` a `640x480` hanno latenze molto vicine.

Questo suggerisce che il solo scanout video, cioè il display controller che legge periodicamente il framebuffer e lo manda verso l'uscita video, non è il principale responsabile del jitter elevato osservato con `kmscube`.

In altre parole:

```text
display statico a 1280x720
  ≈ comportamento simile a
display statico a 640x480
```

mentre nei test con `kmscube`:

```text
rendering continuo ad alta risoluzione
  -> peggioramento molto più evidente
```

Questa differenza rafforza l'ipotesi che il problema sia legato soprattutto a:

- rendering continuo;
- GPU attiva;
- aggiornamento dei buffer grafici;
- swap/present dei frame;
- traffico memoria generato da OpenGL ES/EGL/GBM;
- pressione su DDR, bus e interconnect.

---

## 6. Perché questo risultato è importante

Se il problema fosse causato principalmente dal semplice fatto che il display controller legge un framebuffer più grande, allora ci aspetteremmo una differenza molto più marcata tra:

```text
1280x720
```

e:

```text
640x480
```

Invece i risultati sono molto vicini.

Quindi la spiegazione più coerente è:

> Il display statico, anche cambiando risoluzione, non è sufficiente a produrre il forte jitter osservato nei test grafici pesanti. Il peggioramento sembra emergere quando il sistema deve renderizzare e aggiornare continuamente i buffer grafici, come avviene con `kmscube`.

---

## 7. Collegamento con i test `kmscube`

I test `kmscube` avevano mostrato una forte dipendenza dalla risoluzione.

In quel caso, abbassando la risoluzione, il worst case della `nanosleep` calava sensibilmente.

Questo comportamento è coerente con un carico legato al rendering:

```text
più pixel da renderizzare
  -> più traffico GPU/memoria
  -> più bus_cycles/cpu_cycles/CPI
  -> maggiore latenza nel path real-time
```

Con `modetest`, invece, non c'è rendering continuo. Il framebuffer viene mostrato staticamente.

Per questo motivo i due test `modetest` risultano simili anche a risoluzioni differenti.

---

## 8. Conclusione

I risultati dei test con `modetest` rafforzano l'ipotesi che il problema non sia il semplice HDMI acceso o il semplice scanout di un framebuffer statico.

La conclusione principale è:

> Il jitter elevato osservato nei test con carico grafico non sembra dipendere dal solo display controller che legge un framebuffer statico. La causa più probabile è il rendering continuo e il traffico di memoria generato dalla pipeline grafica attiva, come nel caso di `kmscube`.

Questa osservazione è importante perché orienta le possibili soluzioni.

Invece di concentrarsi solo su:

```text
spegnere HDMI
ridurre genericamente la risoluzione del display statico
```

ha più senso indagare e ottimizzare:

```text
riduzione del rendering continuo
limitazione FPS
riduzione repaint HMI
aggiornamenti grafici solo on-change
riduzione animazioni
uso più controllato della GPU
riduzione del traffico sui buffer grafici
```

---

## 9. Frase sintetica da usare nel report

> Il confronto tra `modetest` a 1280x720 e 640x480 mostra risultati molto simili, con latenze della `nanosleep` basse e stabili. Questo indica che il semplice scanout di un framebuffer statico non è sufficiente a generare il jitter elevato osservato con `kmscube`. La differenza tra `modetest` e `kmscube` suggerisce quindi che il problema sia legato principalmente al rendering continuo e all'aggiornamento dei buffer grafici, più che alla sola presenza del display attivo.

---

## 10. File analizzati

| File | Descrizione |
|---|---|
| `modetest_1280x720` | Test con `modetest` a risoluzione 1280x720. |
| `modetest_640x480` | Test con `modetest` a risoluzione 640x480. |
