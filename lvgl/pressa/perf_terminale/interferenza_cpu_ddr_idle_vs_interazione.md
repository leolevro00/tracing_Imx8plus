# Interferenza GUI su task real-time: CPU/DDR ≫ GPU + confronto "idle vs interazione"

> Testo pronto per il documento di tesi. Argomentazione tecnica supportata dai dati raccolti
> (strumentazione `[RT] uploadDirtyRegion` + screenshot `top`/`htop`).

---

## 1) Perché l'interferenza è soprattutto lato CPU/DDR (non "memoria GPU piena")

### Architettura della pipeline grafica (PEG → SDL)

Nel porting attuale la GUI **non** è disegnata dalla GPU direttamente con widget "nativi". La sequenza è:

1. **PEG disegna in un framebuffer software** in RAM normale (`g_pyBitmap`, RGB565 16 bpp).
2. PEG produce una **dirty region** (rettangolo che rappresenta l'area "sporca").
3. Il thread principale **copia la dirty region verso SDL** tramite:
   - `SDL_UpdateTexture()`: trasferimento CPU→texture (copia di memoria, banda DDR, cache pollution)
4. La GPU fa il compositing/present:
   - `SDL_RenderCopy()` + `SDL_RenderPresent()` → pageflip DRM/KMS

---

### Punto chiave

> Il costo dominante **non è "tenere i buffer nella GPU"**, ma **spostare continuamente pixel dalla RAM alla texture**.

Questo spostamento:

- consuma **banda DDR** (memoria condivisa con tutto il sistema, inclusi task real-time),
- genera **cache miss e bus traffic** sui core CPU,
- aumenta la **latenza/wakeup jitter** dei thread real-time quando la GUI è attiva.

Per questo l'interferenza osservata è principalmente **"CPU/memoria"**, non "GPU memory".

---

### Evidenza sperimentale (strumentazione)

Con la strumentazione su `uploadDirtyRegion()` si misura direttamente:

| Metrica      | Significato                                              |
|--------------|----------------------------------------------------------|
| **`reqMBps`** | MB/s di pixel richiesti (volume di dati copiati)        |
| **`updateMs`** | ms/s spesi dentro `SDL_UpdateTexture()`                |

Nei test più critici (drag del grafico, risoluzione di produzione 1024×600) sono stati osservati valori dell'ordine di:

- **~24 MB/s** copiati verso la texture (`reqMBps`)
- **~208–220 ms su 1000 ms** di CPU spesi dentro `SDL_UpdateTexture()` (`updateMs/s`, cioè oltre il 20% di un core dedicato solo all'upload)

**Log rappresentativi (drag grafico, 1024×600, 33 chiamate/s):**

```text
[RT] uploadDirtyRegion: calls=33 req=24.74MB reqMBps=24.09 updateMs=216.366 effMBps=114.4 maxRectPx=485051
[RT] uploadDirtyRegion: calls=33 req=24.37MB reqMBps=23.75 updateMs=207.857 effMBps=117.3 maxRectPx=485051
```

Contro un idle con la stessa GUI, dove il carico scende di oltre un ordine di grandezza (`calls=7–9`, `reqMBps=0,75–1,0`, `updateMs=22–30`, `maxRectPx≈66 912`, ~11% schermo).

> Questi numeri indicano un carico centrato su **memcpy/trasferimenti**, cioè lato CPU/DDR.

---

### Perché non è (solo) GPU

La GPU entra soprattutto in `RenderCopy`/`Present`, ma:

- l'occupazione di **memoria GPU non risulta saturata** e tende a rimanere stabile;
- il grosso della **variabilità (e del jitter)** è coerente con traffico memoria e tempo CPU speso in upload, che impattano direttamente i core e la DDR.

---

## 2) Differenza "idle vs interazione" (CPU/MEM)

### Metodo di misura

Per confrontare la differenza si usa `top`/`htop` sul target in due condizioni:

| Condizione       | Descrizione                                      |
|------------------|--------------------------------------------------|
| **Idle**         | GUI avviata, schermo statico, nessuna interazione |
| **Interazione**  | drag touch/uso attivo (es. grafico)             |

Si osservano:

- **%CPU** del processo GUI (`PegExec`)
- **RES** (memoria residente) del processo
- opzionale: attività di thread specifici

---

### Risultato quantitativo — risoluzione di produzione (1024×600 viewport)

Misure `top`/`ps` sul target, stesso protocollo (idle = interfaccia attiva senza touch; interazione = drag/scroll sul grafico), configurazione di produzione `XRes=1024 YRes=768` `XView=1024 YView=600` `Bpp=16`:

| Condizione                       | %CPU `PegExec` | %CPU `Lnk` | RES `PegExec`           | Stato       |
|----------------------------------|---------------:|-----------:|------------------------:|:------------|
| **Idle** (nessuna interazione)   | **7,3%**       | 20,1%      | 195 744 KB (~191 MiB)   | S (sleep)   |
| **Interazione** (drag grafico)   | **30,4%**      | 21,8%      | 196 256 KB (~192 MiB)   | R (running) |

> **Δ scroll − idle:** **+23,1 punti percentuali** di CPU (7,3% → 30,4%), a fronte di una RES praticamente **invariata** (+0,26%).

`Lnk` (processo di sistema non legato alla GUI) resta stabile ~20–22% CPU sia in idle sia in interazione: confirma che la crescita di carico osservata è **specifica del processo GUI**, non un effetto di sistema generale.

**Conferma indipendente (implementazione alternativa della pipeline di output):** lo stesso pattern è stato riprodotto con una pipeline di rendering diversa (output diretto DRM/dumb-buffer, bypassando texture SDL/GPU), sempre a 1024×600:

| Condizione (pipeline DRM diretta) | %CPU `PegExec` | RES `PegExec` |
|-----------------------------------|---------------:|--------------:|
| Idle                              | 7,9%           | ~180 MiB      |
| Interazione                       | 38,4%          | ~180 MiB      |

> Δ = **+30,5 pp**. Il salto di CPU tra idle e interazione si osserva **con due implementazioni di presentazione diverse** (texture SDL vs scanout DRM diretto): è quindi un effetto della **quantità di dati copiati dal framebuffer software** (dirty region più grandi e più frequenti), non un artefatto di una specifica API grafica.

---

### Interpretazione

- in **idle**, `PegExec` mostra un carico CPU sensibilmente più basso (~7%);
- durante **interazione**, la CPU del processo cresce in modo netto — più che **4×** — coerente con più redraw, più dirty region, più byte copiati verso la texture/scanout;
- la memoria **RES resta stabile** (variazione < 1%): è coerente con un problema di **banda/tempo di copia** (attività a runtime), non con allocazioni massicce che aumentano la RAM.

---

## 3) Perché l'interazione fa crescere il costo (spiegazione causa-effetto)

Quando l'utente interagisce (touch motion, drag):

1. PEG genera **più eventi** e aggiorna più spesso lo stato dei widget
2. PEG ridisegna **porzioni più grandi** (la dirty region può arrivare a coprire gran parte dello schermo)
3. Ogni frame "sporco" richiede una **copia CPU→texture**

Quindi aumenta:

| Metrica                        | Effetto                          |
|--------------------------------|----------------------------------|
| la **frequenza di upload**       | (`calls/s`)                      |
| l'**area media del rettangolo**  | (`maxRectPx`)                    |
| il **volume copiato**           | (`reqMBps`)                      |
| il **tempo CPU in upload**      | (`updateMs`)                     |

> Questo spiega perché il carico cresce in modo marcato **solo durante l'interazione**.

---

## 4) Conclusione tecnica

L'interferenza della GUI sui task real-time è spiegata principalmente da:

- **traffico DDR** generato dalla copia del framebuffer software verso la texture (`SDL_UpdateTexture`)
- **tempo CPU** impegnato in tali copie
- **dirty region grandi** che moltiplicano il volume copiato

> La GPU e la memoria GPU **non risultano il vincolo principale**: il problema è la pipeline
> **"software framebuffer → upload → present"**, che rende il carico strettamente legato a
> memoria/cache/bus e quindi più impattante sul comportamento real-time.

---

## 5) Come presentare gli screenshot (idle vs interazione)

Sotto le due immagini si può mettere una didascalia tipo:

- **Figura X (Idle)**: `PegExec` al 7,3% CPU; sistema in stato stazionario, RES ~191 MiB.
- **Figura Y (Interazione)**: `PegExec` sale al 30,4% CPU durante drag touch sul grafico (+23,1 pp), a parità di RES; conferma che la GUI introduce carico principalmente quando genera upload/ridisegni, non allocazioni di memoria.

---

### Tabella riassuntiva finale

*(risoluzione di produzione 1024×600, texture SDL/GPU)*

| Condizione   | %CPU `PegExec` | RES `PegExec`         | Note            |
|--------------|---------------:|-----------------------|-----------------|
| Idle         | **7,3%**       | 195 744 KB (~191 MiB) | schermo statico |
| Interazione  | **30,4%**      | 196 256 KB (~192 MiB) | drag grafico    |
| **Δ**        | **+23,1 pp**   | +0,26% (invariata)    | —               |

---

### Effetto della risoluzione (nota complementare)

Per completezza, lo stesso confronto a **risoluzione ridotta (800×600)** mostra un salto di CPU più contenuto, coerente con meno pixel da copiare per ogni dirty region:

| Condizione   | 1024×600 (produzione) | 800×600    |
|--------------|----------------------:|-----------:|
| Idle         | 4,0–7,3%              | 4,0%       |
| Interazione  | 30,4%                 | 15,2%      |
| Δ            | **+23,1 pp**          | +11,2 pp   |

> Questo rafforza la spiegazione causale: **meno pixel → meno byte copiati → meno tempo CPU/DDR → meno salto tra idle e interazione**, indipendentemente dalla risoluzione scelta in produzione.

---

### Nota importante (per non farsi contestare dal prof)

Se il prof obietta *"ma la GPU lavora comunque"*, la risposta è:

- sì, la GPU **presenta** (compositing/present),
- ma l'evidenza mostra che la parte **più variabile e impattante per il RT** è la
  **copia CPU→texture + traffico DDR**, misurata direttamente da `updateMs` e `reqMBps`.

---

## 6) Fonte dei dati

Tutti i valori numerici di questo documento sono estratti da `registro_test_rt.md` (Test 0 — baseline strumentazione, sezione `top`/`ps` del 2026-07-09; Test 6 — Opzione D come conferma indipendente). Per il dettaglio completo di ogni test, i log grezzi e i confronti di risoluzione, vedere quel file.

**Stato:** ✅ documento completo con dati misurati — pronto per la versione finale tesi (paragrafo + tabelle + conclusione).
