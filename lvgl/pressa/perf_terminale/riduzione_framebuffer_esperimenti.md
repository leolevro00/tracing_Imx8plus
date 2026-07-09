

# Parte I — Censimento dei buffer nella pipeline attuale

## 1.1 Premessa: perché "numero" conta più di "dimensione"

Nel sistema in esame il problema **non è la dimensione** dei framebuffer: un frame
completo a 960×640 RGB565 pesa ~1,17 MiB, una quantità irrisoria sia per la RAM
sia per la memoria GPU.

Il problema è il **numero di buffer nella catena**, perché ogni buffer in più
implica una **copia in più** per portare i pixel da PEG al display. E le copie
sono esattamente ciò che genera il traffico DDR e il tempo CPU misurati con la
strumentazione `[RT] uploadDirtyRegion` (`reqMBps`, `updateMs`).

Quindi il punto del prof va letto come:

```text
ridurre i framebuffer = ridurre le copie nella pipeline
```

## 1.2 Buffer individuati (stato attuale)

Dalla pipeline documentata in `pipeline_peg_sdl_drm_rt.md` risultano **almeno 4 buffer**:

| # | Buffer | Dove risiede | Come ci arrivano i pixel | Chi paga il costo |
|---|--------|--------------|--------------------------|-------------------|
| 1 | Framebuffer software PEG (`g_pyBitmap` / `m_framebuffer`) | RAM normale | PEG disegna con la CPU | CPU (disegno) |
| 2 | `SDL_Texture` | Memoria gestita dal driver GPU | copia CPU in `SDL_UpdateTexture()` | **CPU + banda DDR** ← punto critico misurato |
| 3 | Back buffer del renderer SDL | Memoria GPU | `SDL_RenderCopy()` (compositing GPU) | GPU |
| 4 | Front buffer / scanout DRM | Memoria GPU/scanout | pageflip in `SDL_RenderPresent()` | GPU/display controller |

Inoltre il backend **KMSDRM di SDL potrebbe usare triple buffering**, quindi
potrebbe esserci un buffer 3-bis (terzo buffer di swap).

## 1.3 Verifiche da fare sul codice/target (checklist)

Prima di modificare qualsiasi cosa, va confermato lo stato reale:

- [ ] **Flag di creazione della texture**: `SDL_CreateTexture(..., SDL_TEXTUREACCESS_STATIC | STREAMING, ...)`
      → determina quali strategie sono possibili
- [ ] **Formato della texture**: se non è `SDL_PIXELFORMAT_RGB565` ma ad es. ARGB8888,
      `SDL_UpdateTexture()` fa **conversione di formato** e il volume copiato raddoppia
- [ ] **Renderer effettivo sul target**: `SDL_GetRendererInfo()` + log SDL verbose
      (`SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE)`) → quale driver, quali formati nativi
- [ ] **Numero di buffer del backend KMSDRM**: double o triple buffering?
      (verificare hint `SDL_HINT_VIDEO_DOUBLE_BUFFER`)
- [ ] **Risoluzione e bpp effettivi** di `g_pyBitmap` (attesi: 960×640, RGB565 16 bpp)

### Risultati del censimento

*(da compilare dopo lettura del codice / test sul target)*

| Verifica | Risultato | Note |
|----------|-----------|------|
| Access flag texture | *(TODO)* | |
| Formato texture | *(TODO)* | |
| Renderer sul target | *(TODO)* | |
| N. buffer KMSDRM | *(TODO)* | |
| Risoluzione/bpp framebuffer PEG | *(TODO)* | |

---

# Parte II — Opzioni di modifica individuate

Ordinate dalla meno alla più invasiva.

## Opzione A — Allineamento formato texture (costo minimo, guadagno potenziale alto)

**Idea**: se il formato della texture SDL non coincide con un formato supportato
nativamente dal renderer, ogni upload paga una conversione di pixel nascosta.
Allineare i formati elimina la conversione.

**Meccanismo (SDL2)**: quando `SDL_CreateTexture()` riceve un formato che il
renderer **non supporta nativamente**, SDL non fallisce: crea silenziosamente
una texture interna in un formato supportato, e a ogni `SDL_UpdateTexture()`
esegue una **`SDL_ConvertPixels()` nascosta** su un buffer di staging prima
dell'upload vero. Lo schermo funziona perfettamente, ma ogni upload costa una
conversione pixel-per-pixel in più.

**Indizio già presente nei dati baseline**: durante il drag del grafico,
`req≈24 MB` in `updateMs≈210 ms` → **`effMBps` ≈ 115 MB/s**. Una memcpy pura su
i.MX8 viaggia a diversi GB/s: 115 MB/s effettivi dentro `SDL_UpdateTexture()`
sono compatibili con una conversione di formato o con scritture su memoria
write-combined. La diagnosi è quindi motivata dai dati.

- **Costo di sviluppo**: minimo per la diagnosi; la modifica dipende dall'esito
- **Guadagno atteso**: fino a ~50% su `reqMBps`/`updateMs` se il mismatch esiste
- **Rischio**: nullo per la diagnosi
- **Prerequisito**: censimento Parte I

### A.1 Fase 1 — Diagnosi (nessuna modifica funzionale)

Aggiungere una tantum, dopo la creazione di renderer e texture nel codice di
inizializzazione video del porting:

```cpp
SDL_RendererInfo info;
SDL_GetRendererInfo(m_renderer, &info);
SDL_Log("[RT] Renderer: %s (flags=0x%x)", info.name, info.flags);
for (Uint32 i = 0; i < info.num_texture_formats; ++i)
    SDL_Log("[RT]   formato nativo %u: %s", i,
            SDL_GetPixelFormatName(info.texture_formats[i]));

Uint32 fmt; int access, w, h;
SDL_QueryTexture(m_texture, &fmt, &access, &w, &h);
SDL_Log("[RT] Texture: %s, access=%d, %dx%d",
        SDL_GetPixelFormatName(fmt), access, w, h);
```

Ricompilare, lanciare sul target, leggere dai log:

1. **formato della texture** (quello richiesto dal porting);
2. **se quel formato è nella lista dei formati nativi del renderer** —
   se NON c'è, la conversione nascosta esiste ed è confermata;
3. **quale renderer sta girando davvero** (opengles2? software?).

### A.2 Fase 2 — Modifica, in base all'esito della diagnosi

| Esito diagnosi | Azione |
|----------------|--------|
| Texture RGB565 **e** RGB565 tra i formati nativi | Nessun mismatch: Opzione A non dà guadagno. Documentare l'esito negativo e passare alla B |
| Texture ARGB8888 con conversione manuale nel porting prima dell'upload | Cambiare in `SDL_PIXELFORMAT_RGB565` (se nativo) e rimisurare |
| Texture RGB565 ma renderer **non** la supporta nativamente | Conversione nascosta confermata. Strade: (a) creare la texture nel formato nativo (es. ARGB8888) e portare il framebuffer PEG a 32 bpp — raddoppia i byte disegnati ma elimina la conversione, trade-off da misurare; (b) saltare all'Opzione B/D |

In tutti i casi in cui si applica una modifica, validare con il protocollo di
misura della Parte III (idle + drag, confronto con baseline).

## Opzione B — Texture STREAMING con `SDL_LockTexture` sulla dirty region

**Idea**: invece di `SDL_UpdateTexture()` (che internamente può fare copia +
staging), con una texture `SDL_TEXTUREACCESS_STREAMING` si usa
`SDL_LockTexture(rect)` e si copia direttamente nella memoria mappata dal driver.
Su alcuni driver evita una copia intermedia.

- **Costo di sviluppo**: basso (modifica localizzata in `uploadDirtyRegion()`)
- **Guadagno atteso**: da verificare empiricamente — su i.MX8/Vivante può cambiare
  molto o nulla
- **Rischio**: basso; attenzione al pitch restituito dal lock (può differire dal
  pitch del framebuffer PEG → copia riga per riga)

## Opzione C — Ridurre i buffer di swap SDL: da triple a double buffering

**Idea**: forzare il double buffering sul backend KMSDRM con
`SDL_SetHint(SDL_HINT_VIDEO_DOUBLE_BUFFER, "1")`.

- **Costo di sviluppo**: minimo (un hint)
- **Guadagno atteso**: riduce memoria GPU e latenza di presentazione; **non riduce
  le copie CPU**, quindi guadagno RT modesto
- **Rischio**: `SDL_RenderPresent()` può bloccare più spesso in attesa del vsync
- **Nota**: è l'interpretazione letterale del punto del prof ("ridurre il numero
  dei framebuffer"), quindi va comunque provata e documentata

## Opzione D — Eliminare la texture: PEG → DRM dumb buffer diretto

**Idea**: bypassare SDL per l'output. Si allocano 2 **DRM dumb buffer**
(double buffering), si copia la dirty region dal framebuffer PEG direttamente
nel dumb buffer e si fa pageflip via DRM/KMS. La catena diventa:

```text
PEG disegna → 1 copia (dirty region) → pageflip
```

eliminando: texture SDL, compositing GPU (`RenderCopy`), presentazione SDL.

- **Costo di sviluppo**: alto (gestione DRM manuale: modeset, dumb buffer, pageflip)
- **Guadagno atteso**: elimina un buffer intero e il coinvolgimento GPU;
  resta una sola copia CPU per frame
- **Rischio**: medio-alto; SDL resterebbe utile solo per l'input (fattibile:
  si può tenere SDL solo per gli eventi)

## Opzione E — Disegno diretto di PEG nella texture (zero-copy) — VALUTATA E SCARTATA

**Idea**: far disegnare PEG direttamente nei pixel mappati di una texture
streaming, eliminando del tutto `g_pyBitmap`.

**Perché non è fattibile così com'è**:

1. PEG fa **redraw parziali** e ha bisogno di **rileggere il contenuto
   precedente** del framebuffer;
2. il contenuto di una texture dopo `SDL_LockTexture()` è **indefinito**
   (non è garantito che contenga il frame precedente);
3. la memoria mappata dal driver è tipicamente **write-combined**: le letture
   sono lentissime, e PEG legge dal framebuffer mentre disegna.

→ Documentata come opzione valutata e scartata, con motivazione tecnica.

## Nota sulla "dimensione" dei framebuffer

Ridurre la **dimensione** in senso stretto è praticamente impossibile:

- il formato è già **RGB565 a 16 bpp**, il minimo sensato per una GUI a colori;
- la **risoluzione è vincolata dal pannello** fisico.

Questa risposta va data esplicitamente al prof: sulla dimensione non c'è margine,
il margine è sul **numero di buffer/copie**.

---

# Parte III — Esperimenti e risultati

## 3.1 Protocollo di misura (uguale per tutti gli esperimenti)

Per ogni modifica si misurano **due scenari** sul target:

1. **Idle**: GUI avviata, schermo statico, nessuna interazione
2. **Interazione**: drag touch del grafico (scenario più critico osservato)

Metriche raccolte:

- `[RT] uploadDirtyRegion`: `calls`, `reqMBps`, `updateMs`, `effMBps`, `maxRectPx`
- `top`/`htop`: %CPU e RES di `PegExec`
- `perf`: contatori DDR/cache (`mem_access`, `bus_access`, `l2d_cache_refill`,
  `l2d_cache_wb`, `imx8_ddr0/write-accesses`)

### Baseline (stato attuale, nessuna modifica)

| Scenario | calls/s | reqMBps | updateMs | maxRectPx | %CPU PegExec |
|----------|---------|---------|----------|-----------|--------------|
| Idle | 7–9 | ~0,75–1 | ~22–30 | ~66912 | *(TODO)* |
| Drag grafico | ~33 | ~24 | ~208–220 | 485051 (~79% schermo) | *(TODO)* |

## 3.2 Esperimento A — Allineamento formato texture

- **Stato**: ⬜ non ancora eseguito
- **Modifica applicata**: *(TODO)*
- **Risultati**:

| Scenario | calls/s | reqMBps | updateMs | maxRectPx | %CPU PegExec |
|----------|---------|---------|----------|-----------|--------------|
| Idle | | | | | |
| Drag grafico | | | | | |

- **Conclusione**: *(TODO)*

## 3.3 Esperimento B — LockTexture streaming

- **Stato**: ⬜ non ancora eseguito
- **Modifica applicata**: *(TODO)*
- **Risultati**: *(tabella come sopra)*
- **Conclusione**: *(TODO)*

## 3.4 Esperimento C — Double buffering KMSDRM

- **Stato**: ⬜ non ancora eseguito
- **Modifica applicata**: *(TODO)*
- **Risultati**: *(tabella come sopra)*
- **Conclusione**: *(TODO)*

## 3.5 Esperimento D — DRM dumb buffer diretto

- **Stato**: ⬜ non ancora eseguito (da valutare solo se A/B/C insufficienti,
  visto il costo di sviluppo)
- **Modifica applicata**: *(TODO)*
- **Risultati**: *(tabella come sopra)*
- **Conclusione**: *(TODO)*

---

# Conclusioni (da compilare a fine esperimenti)

*(TODO: riepilogo di quali opzioni sono risultate fattibili, quali guadagni hanno
prodotto rispetto alla baseline, e risposta finale al punto 1 del prof.)*
