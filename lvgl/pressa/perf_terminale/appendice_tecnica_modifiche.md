# Appendice tecnica — modifiche al codice

## Differenze fra il branch `lvgl-hmi` e la configurazione di test finale

Documento di dettaglio a corredo di `modifiche_progetto.md`. Qui il livello è quello del
codice: cosa è cambiato, in quale funzione, e per quale ragione tecnica.

---

## A. Panoramica quantitativa

| Repository | File | Inserimenti | Cancellazioni |
|---|---|---|---|
| `pressbrakepeg` | 43 | 1 249 | 247 |
| `pegenstein` | 16 | 83 765 | 560 |
| `SqCom_Library` | 4 (2 nuovi) | — *(non ancora committato)* | — |
| `PlcEsa` | 1 | — *(non ancora committato)* | — |

> ⚠️ **Il dato di `pegenstein` è fuorviante.** Comprende tre file **generati** finiti
> per errore sotto controllo di versione:
>
> | File | Righe | Natura |
> |---|---|---|
> | `PegLib/Makefile` | 80 582 | generato da `qmake` |
> | `Makefile` | 537 | generato da `qmake` |
> | `.qmake.stash` | 23 | cache di `qmake` |
>
> **Al netto di questi, il codice reale di `pegenstein` è di ~2 620 righe.**
> Vanno aggiunti a `.gitignore` e rimossi dal tracking con `git rm --cached`.

**Totale del codice effettivamente scritto: circa 3 900 righe.**

---

## B. I tre filoni di lavoro

Il diff contiene tre tipi di intervento, con motivazioni indipendenti:

| Filone | Obiettivo | Peso |
|---|---|---|
| **1. Robustezza** | Eliminare crash e freeze del CAD 2D, delle liste e dell'ottimizzatore | ~60 % |
| **2. Prestazioni real-time** | Ridurre il jitter del thread RT | ~25 % |
| **3. Infrastruttura di misura** | Poter misurare e diagnosticare | ~15 % |

Il primo filone non era l'obiettivo iniziale: è emerso perché i crash impedivano di
eseguire i test di durata necessari alle misure. **Non si può misurare per un'ora
un'applicazione che va in segmentation fault dopo dieci minuti.**

---

## C. Filone 1 — Robustezza

### C.1 Il pattern ricorrente: `RecGrafPtr()` è una cache non validata

**Il problema.** `CPPGBaseDocument` mantiene `m_pRecG`, un puntatore *cache* al record
grafico corrente, aggiornato da `UpdateRecGrafPtr()`. La versione originale:

```cpp
LPRECGRAFICO CPPGBaseDocument::UpdateRecGrafPtr()
{
  return (m_pRecG = (LPRECGRAFICO)m_pViewG->m_ListaGrafica[m_nStepAttivo]);
}
```

Nessun controllo su `m_pViewG`, nessun controllo che `m_nStepAttivo` sia dentro la lista.
Quando l'indice usciva dal range — dopo un'aggiunta di sezione fallita, o con una vista
inconsistente — il puntatore diventava spazzatura e il primo `GetCodiceRecord()` a valle
produceva un `SIGSEGV`.

**La correzione**, in `cad2d/Ppgdoc.cpp`:

```cpp
LPRECGRAFICO CPPGBaseDocument::UpdateRecGrafPtr()
{
  if (!m_pViewG || m_nStepAttivo < 0 ||
      m_nStepAttivo >= m_pViewG->NumeroElementi())
  {
    m_pRecG = NULL;
    return NULL;
  }
  m_pRecG = (LPRECGRAFICO)m_pViewG->m_ListaGrafica[m_nStepAttivo];
  return m_pRecG;
}
```

**E il pattern applicato ai chiamanti.** Sistemare solo la funzione non basta: tutti i
punti che usavano `RecGrafPtr()` direttamente dereferenziandolo sono stati riscritti per
rileggere dalla lista con controllo di range, invece di fidarsi della cache:

```cpp
// prima
LPRECGRAFICO pRecGraf = pDoc->RecGrafPtr();

// dopo
LPRECGRAFICO pRecGraf = NULL;
LPVISTAGRAFICA pView = pDoc->ViewGrafPtr();
const int nRec = pDoc->RecGrafNum();
if (pView && nRec >= 0 && nRec < pView->NumeroElementi())
    pRecGraf = pDoc->RecGrafPtrAt(nRec);
if (pRecGraf == NULL)
    return;
```

Applicato in: `PezzoForm::EditDraw`, `PezzoFormLAlpha::EditDraw`,
`CPezzoView::OnLButtonUp` (due punti), `CPezzoView::SelezionatoTratto`,
`CPezzoView::InizioSequenza`, `TestChiudiPagCad2D`.

### C.2 Overflow dell'array a `MAX_GBEND`

**Il problema.** Diversi array hanno dimensione fissa `MAX_GBEND` (`prof_lin[]`,
`sPezzoGrafico[]`, `m_dXAbs[]`, `bFindLine[]`), ma il numero di elementi grafici della
sezione — `SezioniCadGrafico[]` — **poteva superarlo**. Il risultato era scrittura oltre
la fine dell'array, con corruzione di memoria e crash differito e difficile da attribuire.

**Le correzioni**, a più livelli della catena:

*A monte*, in `CPPGBaseDocument::GetDatiOttPezzo` (`cad2d/Ppgdoc.cpp`):

```cpp
SezioniCadGrafico[nView] = (short)lpVistaGraf->NumeroElementi();
if (SezioniCadGrafico[nView] > MAX_GBEND)      // clamp a monte dell'ottimizzatore
    SezioniCadGrafico[nView] = MAX_GBEND;
```

E l'errore che prima era un commento diventa un errore vero:

```cpp
if (nElemento >= MAX_GBEND)                     // era: == MAX_GBEND
{
  // TRACE("ATTENZIONE: troppi dati nel disegno...\n");   ← prima
  CadErrorGlb.Errore(lErrTroppiElementiGrafici);          ← ora
  return;
}
```

*Nell'ottimizzatore*, in `Ottinit.cpp`, `Ottpunti.cpp`, `Ottcomp.cpp`, `Ottutens.cpp`: il
valore letto da `SezioniCadGrafico[]` viene copiato in una variabile locale e limitato
prima di essere usato come estremo di ciclo o come indice.

*Nel disegno*, in `Pezzoview.cpp`, `Sim2DView.cpp`: il numero di elementi da disegnare
viene limitato prima del ciclo.

**E un bug latente scoperto strada facendo**, in `CPezzoView::Paint`:

```cpp
// prima — la variabile di ciclo dichiarata è i, ma il corpo incrementa e indicizza nStep
for( int i = nStep; nStep<MAX_GBEND; nStep++)
{
    m_dXAbs[nStep]=0;  ...
}

// dopo
for( int iClear = nStep; iClear < MAX_GBEND; iClear++)
{
    m_dXAbs[iClear]=0;  ...
}
```

Il ciclo originale dichiarava `i` e non lo usava mai: un errore di scrittura rimasto
inosservato perché il comportamento risultante era comunque "quasi" corretto.

### C.3 Il bug di allocazione delle liste (`bad_array_new_length`)

Il crash più insidioso, perché la causa era a tre livelli di distanza dal sintomo.

**La catena.** In `CFileView::Create`:

```cpp
int nRowsToAdd = nHeightForRows / (rcCell.Height() + 1) - 1;
while (nRowsToAdd--)
    m_pPegSSheetFileSys->AddRow();
```

Con un layout del notebook non ancora completo, `nHeightForRows` poteva risultare ≤ 0 e
quindi `nRowsToAdd` **negativo**. Ma `while (nRowsToAdd--)` con valore negativo è vero
per miliardi di iterazioni: il codice chiamava `AddRow()` senza fine, finché
`InsertRow()` tentava `new CSpreadSheetRowBase*[miRows + 1]` con `miRows` fuori scala e
il runtime lanciava `std::bad_array_new_length`.

**La correzione su tre livelli.**

*Livello 1 — la causa*, in `liste/FileView.cpp`:

```cpp
int nCellH = rcCell.Height() + 1;
int nRowsToAdd = 0;
if (nCellH > 0 && nHeightForRows > 0)
    nRowsToAdd = nHeightForRows / nCellH - 1;
if (nRowsToAdd < 0)  nRowsToAdd = 0;
if (nRowsToAdd > 128) nRowsToAdd = 128;      // tetto UI ragionevole

while (nRowsToAdd-- > 0)                      // era: while (nRowsToAdd--)
    m_pPegSSheetFileSys->AddRow();
```

*Livello 2 — il contenimento*, in `editorbase/TabWindowBase.cpp`, sia nel costruttore
(stessa divisione, stesso rischio) sia in `AddRow()` con un tetto strutturale:

```cpp
void CTabWindowBase::AddRow()
{
  if (m_nNumMaxRow > 0 && m_nTotalRows >= m_nNumMaxRow)
    return;
  ...
}
```

*Livello 3 — la difesa di ultima istanza*, in `editorbase/SpreadSheetBase.cpp`:

```cpp
static const SIGNED kSpreadSheetMaxRows = 512;

SIGNED CSpreadSheetBase::InsertRow(SIGNED iRow, PEGCHAR *pHeader)
{
    if (miRows < 0 || miRows >= kSpreadSheetMaxRows || miCols <= 0)
    {
        fprintf(stderr, "[LISTE] InsertRow refused (miRows=%d miCols=%d)\n", ...);
        return miRows;
    }
    if (iRow < 0 || iRow > miRows)
        iRow = miRows;
    ...
```

Tutte le allocazioni sono state convertite a `new (std::nothrow)` con gestione del
fallimento e rollback, invece di lasciar propagare l'eccezione:

```cpp
CSpreadSheetRowBase **pRows = new (std::nothrow) CSpreadSheetRowBase *[1];
if (!pRows)
    return miRows;
CSpreadSheetRowBase *pRow = new (std::nothrow) CSpreadSheetRowBase(miCols);
if (!pRow)
{
    delete[] pRows;
    return miRows;
}
```

**E un bug di memoria indipendente**, nella stessa funzione:

```cpp
delete pOldRowData;     // prima — array allocato con new[], liberato con delete
delete[] pOldRowData;   // dopo
```

Comportamento non definito, presente nel codice originale.

### C.4 Doppia distruzione nell'albero PEG

**Il problema.** In PegLib, `CDialogBase::Message(PM_HIDE)` invoca `FreeMemoria(this)`,
che distrugge **ricorsivamente tutti i figli**. Il codice applicativo però distruggeva
*anche* esplicitamente alcuni di quei figli, oppure ne riusava i puntatori dopo l'hide.
Risultato: double free o use-after-free, con crash in `MessageChildren()`.

**Le correzioni.**

In `CListaView::Message(PM_HIDE)` — non distruggere, solo azzerare:

```cpp
// CDialogBase::Message(PM_HIDE) chiama FreeMemoria(this) → Destroy ricorsivo di
// TUTTI i figli (FileView compreso). NON Destroy/delete qui.
iRet = CDialogBase::Message(Mesg);
mFileView = NULL;
mFileViewOld = NULL;
m_bStatusBarCreated = FALSE;
m_bUiCreated = FALSE;
```

In `VisualizzaFileView()` — distruggere solo se ancora agganciato all'albero:

```cpp
if (mFileViewOld)
{
    if (mFileViewOld->Parent() != NULL)
        Destroy(mFileViewOld);
    // altrimenti è già stato liberato da FreeMemoria: solo NULL
    mFileViewOld = NULL;
}
```

In `CToolListTabVw::OnShow()` — riconoscere che i puntatori sono *dangling* e ricreare:

```cpp
// Su PM_HIDE, CDialogBase::FreeMemoria(this) ha già distrutto notebook + Mat +
// Pun + FileView. I puntatori sotto sono DANGLING.
//   - NON Destroy (già liberati → SIGSEGV)
//   - NON reuse  (use-after-free → lista vuota / crash)
m_pToolNotebook = NULL;
m_pMatListVw    = NULL;
m_pPunzListVw   = NULL;
m_pToolTab[0]   = NULL;
m_pToolTab[1]   = NULL;
OnCreate();
```

**Il caso opposto**, in `CMatListVw` e `CPunListVw`: quando la lista è *page-client* di un
notebook, il cambio di scheda genera `Remove` → `PM_HIDE`, e lasciar fare `FreeMemoria`
distruggerebbe l'albero a ogni cambio tab. Qui la soluzione è **evitare** la catena:

```cpp
// Mat è page-client del notebook: su cambio tab PegNotebook fa Remove → PM_HIDE.
// FreeMemoria distruggerebbe TabWindow/FileView mentre l'oggetto Mat resta vivo.
// Solo nascondi PEG; ToolTab FreeMemoria distrugge tutto all'uscita pagina.
iRet = CPegDeskDialog::Message(Mesg);   // salta CListaView::Message
break;
```

Con i corrispondenti flag di idempotenza (`m_bUiCreated`, `m_bStatusBarCreated`,
guard su `m_pTabWinListaCave`) perché `OnShow`/`OnCreate` non ricostruiscano un albero
che esiste già.

### C.5 Dialog modali annidate → freeze dell'interfaccia

**Il problema.** `COttimDlg::OnHide()` viene invocata *durante* la `Execute()` modale
della dialog stessa. Aprire lì una seconda finestra modale significa annidare due
`Execute()`, e l'interfaccia si blocca.

```cpp
case THR_IMPOSS:
case THR_ENDSOL:
    PpgMessageWindow(LS(IDS_OTT_NOSOLUZ), ...);   // ← freeze
    break;
```

**La correzione: differire il messaggio al chiamante**, dopo che `Execute()` è tornata.
In `sim2d/Ottimdlg.cpp` il messaggio viene rimosso, e in `Sim2DFrame.cpp` — sia in
`OnOttimizza()` sia in `OttimizzaAutomatico()` — viene mostrato al posto giusto:

```cpp
pDlg->Execute();
SetDlgModale(NULL);

if(sSoluzTrovata == THR_IMPOSS || sSoluzTrovata == THR_ENDSOL)
    PpgMessageWindow(LS(IDS_OTT_NOSOLUZ), LS(IDS_GEN_ERRORE), MW_WARNINGOK);
```

Stesso schema in `GetPRGFileVersion()` (`cad2d/StdAfx.cpp`), chiamata in modo sincrono da
`OnSelChange`: la finestra di errore è stata sostituita da un codice di ritorno.

### C.6 Rollback su operazione fallita

`CPezzoDoc::ViewGrafSucc()` incrementava la sezione attiva e poi tentava di aggiungere una
piega. Se l'aggiunta falliva (per esempio al limite `MAX_GBEND`), restava selezionata una
**sezione vuota con `RecGrafPtr()` NULL**, e il primo `PopulateTable()` successivo andava
in crash.

La correzione salva lo stato prima e lo ripristina in caso di fallimento:

```cpp
const int nViewPrev = ViewGrafNum();
LPVISTAGRAFICA pViewPrev = m_pViewG;
const int nStepPrev = m_nStepAttivo;
LPRECGRAFICO pRecPrev = m_pRecG;

...

if (AggiungiPiega(20.0,0) != SUCCESS_PPG)
{
    // rollback: resta sulla sezione precedente
    ((CFilePPGPezzo*)m_pFile)->m_Disegno.m_nVistaGrAttiva = nViewPrev;
    m_pViewG = pViewPrev;
    m_nStepAttivo = nStepPrev;
    m_nSubStepAttivo = 0;
    m_pRecG = pRecPrev;
    return FAILURE_PPG;
}
```

E i due chiamanti sono stati adeguati a **non** aggiornare la tabella quando l'operazione
fallisce (`CPezzoForm::OnSezSucc`, `CPezzoFormLAlpha::FieldUpdate`).

### C.7 Altri difetti corretti

| Punto | Difetto | Correzione |
|---|---|---|
| `GetDatiPreviewFromCad2D` | `m_NumCave` non inizializzato: se il `while` non entrava restava spazzatura, e `InserimentoInListaCave` faceva `AddRow` fino a 512 con `SetRow` fuori range | inizializzazione esplicita a 0 |
| `CSaveAsListVw::OrdinaNomiFile` | QuickSort senza controllo di estremi: i due `while` interni potevano uscire dall'array → freeze | early-exit `nSinistra >= nDestra` e bound check nei cicli |
| `CSaveAsListVw::OnActivateView` | `OrdinaNomiFile(0, -1)` su lista vuota | guard `miNumOfFiles > 1` |
| `CSaveAsListVw::InserimentoInLista` | `Destroy` su oggetto mai aggiunto all'albero; `Add` doppio della stessa lista | `Destroy` se ha parent, altrimenti `delete`; `Add` solo se `!Parent()` |
| `CPezzoFrame::OnPrev` | inviava `PK_F9`, non gestito da `TastoGestitoDaFieldUpdate` | sostituito con `ID_PREV` |
| `CDraftPieceWnd` | `g_pDraftPieceWnd` restava a puntare un oggetto distrutto | distruttore che azzera il globale |
| `CPezzoForm::OnUpdate` | con la form L,alpha attiva, `EditDraw()` della form classica usava un `RecGrafPtr()` non aggiornato | early return se `GetForm_LAlpha()` è attiva |

---

## D. Filone 2 — Prestazioni real-time

### D.1 Coalescing temporale del ridisegno durante il pan

**Prima**, in `CSim2DView::OnMouseMove`:

```cpp
Invalidate();
Draw();          // a ogni evento di movimento del dito
```

**Dopo:**

```cpp
m_bPanRedrawPending = TRUE;
DrawPanIfDue(FALSE);
```

con:

```cpp
void CSim2DView::DrawPanIfDue(BOOL bForce)
{
  static const DWORD kPanMinRedrawIntervalMs = 33;      // ~30 fps

  if (!m_bPanRedrawPending)
    return;

  const DWORD now = GetTickCount();
  if (!bForce && (now - m_dwLastPanDrawMs) < kPanMinRedrawIntervalMs)
    return;

  m_dwLastPanDrawMs = now;
  m_bPanRedrawPending = FALSE;
  Invalidate();
  Draw();
}
```

**I due dettagli che rendono corretto lo schema:**

*L'origine del disegno continua ad aggiornarsi a ogni evento.* Solo il ridisegno è
limitato: il gesto resta agganciato al dito, cambia la frequenza dei fotogrammi.

*Il frame finale è forzato.* Al rilascio del tocco si chiama `DrawPanIfDue(TRUE)`, che
bypassa il limite temporale. Senza questa chiamata l'ultimo frame poteva restare
disallineato di un intervallo rispetto alla posizione definitiva.

Nella variante CAD (`CPPGBaseView::DrawPanIfDue`, `cad2d/Ppgviews.cpp`) c'è un guadagno
aggiuntivo: la sostituzione di `pDoc->UpdateAllViews()` con il solo ridisegno del canvas.

```cpp
// prima
CPPGBaseDocument* pDoc = GetDocument();
pDoc->UpdateAllViews();       // ridisegna form, tabelle, preview

// dopo
m_bPanRedrawPending = TRUE;   // solo il canvas
DrawPanIfDue(FALSE);
```

Durante un trascinamento, form e tabelle non cambiano: aggiornarle era lavoro
interamente sprecato.

### D.2 Griglia disattivata durante il trascinamento

In `MatView.cpp`, `Punzview.cpp` e `Pezzoview.cpp`:

```cpp
if (GridFlagGlb==0 && !m_bTracking)      // griglia light: skip in pan
```

La griglia è un reticolo di linee che copre l'intera area: costosa da rasterizzare e
inutile durante un trascinamento, dove serve solo il riferimento del disegno. Alla
chiusura del gesto, `DrawPanIfDue(TRUE)` viene chiamata **dopo** `m_bTracking = FALSE`,
così l'ultimo frame ha la griglia completa.

### D.3 Differimento del lavoro pesante al cambio pagina

La funzione originale `CMDINum::AttivaPaginaCH0` faceva tutto in sequenza: stato macchina,
feedback dei pulsanti, `SettaControlli()`, `GetEntry()`, `AttivaMenu()`.

**La riorganizzazione la scompone in tre parti con tempistiche diverse:**

| Parte | Quando | Perché |
|---|---|---|
| `ApplyCH0ToolbarFeedback()` | **subito** | è il riscontro visivo alla pressione: ritardarlo si percepisce |
| `ScriviStatoMacchina()` | **subito** | è l'unica fonte di verità letta in modo sincrono da `GestionePagine::KeyCambiaPagina` e da `OnImpostazioni`/`OnMan` per decidere la navigazione |
| `CompletaAttivaPaginaCH0()` | **differito 300 ms** | `SettaControlli` + `GetEntry` + `AttivaMenu`: il lavoro pesante, che nessuno legge in modo sincrono |

```cpp
void CMDINum::AttivaPaginaCH0(int nStato)
{
    ApplyCH0ToolbarFeedback(nStato);          // feedback immediato

    theApp.m_bFirstTimeInStop = TRUE;
    theApp.m_bFirstTimeInStart = FALSE;
    ScriviStatoMacchina(nStato);              // stato: SEMPRE immediato
    QuotaPosOK(0., RES);

    if((nStato == IMP) || (nStato == MAN))
    {
        // Prima configurazione: subito (altrimenti Add su controlli non pronti)
        if(m_nLastCompletedCH0State < 0)
        {
            CancelCH0HeavyWorkSchedule();
            CompletaAttivaPaginaCH0(nStato);
            return;
        }

        m_nPendingCH0State = nStato;
        m_bCH0WorkPending  = TRUE;
        m_dwTimeLast       = GetTickCount();
        ScheduleCH0HeavyWork(nStato);
        return;
    }

    // AUTO/SAUTO: nessun defer (stati macchina "operativi")
    CancelCH0HeavyWorkSchedule();
    CompletaAttivaPaginaCH0(nStato);
}
```

**Quattro difficoltà risolte, tutte documentate nel codice.**

*Lo stato macchina non può essere differito.* È la lezione più importante. Il primo
tentativo differiva anche `ScriviStatoMacchina()`: il risultato era che il tasto per
tornare allo stato precedente sembrava non fare nulla, perché la logica di navigazione
esterna leggeva ancora lo stato vecchio.

*`KillTimer` non svuota la coda dei messaggi.* I `PM_TIMER` già accodati arrivano
comunque. Il gestore deve quindi **ricalcolare ogni volta** se il tempo è davvero
trascorso, invece di fidarsi del fatto che il timer sia stato fermato:

```cpp
BOOL CMDINum::HandleCH0DeferTimer(WORD wTimerId)
{
    if(wTimerId != TIMER_CH0_DEFER)
        return FALSE;

    if(!m_bCH0WorkPending || m_bCH0Completing)
        return TRUE;

    const DWORD elapsed = GetTickCount() - m_dwTimeLast;
    if(elapsed < CH0_DEFER_DELAY_MS)
        return TRUE;                          // non ancora: lascia ticchettare

    m_bCH0WorkPending = FALSE;
    CancelCH0HeavyWorkSchedule();

    // Martellando e tornando allo stato già a video: niente lavoro
    if(m_nPendingCH0State == m_nLastCompletedCH0State)
        return TRUE;

    CompletaAttivaPaginaCH0(m_nPendingCH0State);
    return TRUE;
}
```

*Il valore di ritorno `TRUE` significa "è il mio timer", non "ho lavorato".* Un tentativo
intermedio faceva `KillTimer` in `CPpgViewBase::Message` a ogni tick, e il lavoro
differito non partiva quasi mai. Il commento nel codice lo annota esplicitamente.

*Il timer non deve sopravvivere all'hide della pagina.* Se resta armato mentre la pagina è
nascosta, scatta più tardi e rifà `SettaControlli`/`GetEntry` sopra a un setup già
eseguito da `OnChiave0`, con `Add` duplicati e testo sovrapposto. Da cui la protezione in
`CPpgView::Message(PM_HIDE)`:

```cpp
CancelCH0HeavyWorkSchedule();
m_nLastCompletedCH0State = -1;
```

**L'ottimizzazione che nasce dal differimento** è la coalescenza: premendo rapidamente più
pulsanti, ogni pressione riprogramma il timer, e il confronto
`m_nPendingCH0State == m_nLastCompletedCH0State` elimina anche il caso in cui si torni
allo stato già a video. N pressioni producono **al massimo una** ricostruzione.

> **Vincolo di sicurezza.** `ConfigButtonsStartStopPlusMinus`, che mappa i pulsanti fisici
> Start/Stop/Plus/Minus, è invocata dentro `AttivaMenu` per gli stati AUTO e SAUTO —
> **esclusi dal differimento**. Non è un dettaglio implementativo: differire quella
> configurazione aprirebbe una finestra in cui i comandi fisici della pressa non sono
> correttamente mappati.

### D.4 Diagnostica delle liste compilata fuori dal binario

`liste/liste_diag.h`, file nuovo. Trenta punti di chiamata resi a costo zero:

```cpp
inline void ListeDiag(const char *fmt, ...)
{
#ifdef LISTE_DIAG_ENABLED
	std::fprintf(stderr, "[LISTE] ");
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	std::fprintf(stderr, "\n");
	std::fflush(stderr);
#else
	(void)fmt;
#endif
}
```

Il commento nel file spiega la ragione, ed è quella giusta:

> *"`fprintf`+`fflush` sono I/O sincroni e falsano le misure RT a livello di 100 µs.
> Era stata tenuta sempre attiva durante la caccia al crash Die↔Program List; risolto
> quel bug, il costo non è più giustificato durante le misure."*

### D.5 Uscita video diretta su DRM (`pegenstein`)

L'intervento più esteso: `PegLib/pegdrmoutput.cpp` (737 righe nuove) e
`PegLib/pegdrm_evdev.cpp` (409 righe nuove), più la riscrittura di `peglvglwindow.cpp`.

**Il cuore è la copia della sola regione modificata**, in `copyRectFromSource`:

```cpp
const int dstPitch = static_cast<int>(dst.pitch);
if (left == 0 && rectW == m_width && srcPitch == dstPitch)
{
    // righe contigue: una sola memcpy
    std::memcpy(dst.map + top * dstPitch,
        src + top * srcPitch,
        (size_t)rectH * (size_t)dstPitch);
    return;
}
for (int row = 0; row < rectH; ++row)
{
    const unsigned char *srcRow = src + (top + row) * srcPitch + left * 2;
    unsigned char *dstRow = dst.map + (top + row) * dstPitch + left * 2;
    std::memcpy(dstRow, srcRow, (size_t)rectW * 2u);   // 2 byte/pixel RGB565
}
```

**Il problema non ovvio del double buffering** è che il buffer su cui si scrive non è
quello che era a video l'ultimo frame: ha "perso" gli aggiornamenti fatti mentre era
davanti. `blitDirtyRegion` lo risolve con un meccanismo di recupero:

```cpp
if (m_bufferNeedsFullCopy[m_backIndex])
{
    copyRectFromSource(src, srcPitch, dstBuf, 0, 0, m_width-1, m_height-1);
    m_bufferNeedsFullCopy[m_backIndex] = false;
    m_staleDamage[m_backIndex].valid = false;
}
else if (m_staleDamage[m_backIndex].valid)
{
    const DamageRect &stale = m_staleDamage[m_backIndex];
    const int thresholdPx = (m_width * m_height * kFullSyncDamageRatioPercent) / 100;
    const int stalePx = damagePixelCount(stale, m_width, m_height);
    if (stalePx >= thresholdPx)      // scroll pesante: il bounding box non conviene
        copyRectFromSource(src, srcPitch, dstBuf, 0, 0, m_width-1, m_height-1);
    else
        copyRectFromSource(src, srcPitch, dstBuf,
            stale.left, stale.top, stale.right, stale.bottom);
    m_staleDamage[m_backIndex].valid = false;
}

copyRectFromSource(src, srcPitch, dstBuf, left, top, right, bottom);
damageUnion(m_pendingDamage, left, top, right, bottom);
```

Ogni buffer tiene traccia della propria regione "arretrata"; quando questa supera
`kFullSyncDamageRatioPercent` dell'area, conviene copiare tutto invece di inseguire un
bounding box ormai grande quanto lo schermo.

> **Questo è il parametro da tarare per primo** in un eventuale seguito: è la soglia che
> decide fra una copia da 1,2 MB e una copia proporzionale al danno reale.

Il page flip è **non bloccante** (`DRM_MODE_PAGE_FLIP_EVENT` con handler di completamento)
e non ne viene accodato un secondo finché il primo non è concluso:

```cpp
bool PegDrmOutput::pageFlip()
{
    if (!m_initialized || m_flipPending)     // non accodare flip se uno è in corso
        return false;
    ...
}
```

---

## E. Filone 3 — Infrastruttura di misura

### E.1 `CAD_DIAG` — tracciatura a costo zero, attivabile dal `.pro`

In `IncPPG/CommonConst.h`:

```cpp
#ifndef CAD_DIAG_CALCULATE
#define CAD_DIAG_CALCULATE 0
#endif
#if CAD_DIAG_CALCULATE
#include <stdio.h>
#define CAD_DIAG(tag, fmt, ...) do { \
    fprintf(stderr, "[CAD] diag %s: " fmt "\n", (tag), ##__VA_ARGS__); \
    fflush(stderr); \
} while(0)
#else
#define CAD_DIAG(tag, fmt, ...)
#endif
```

Usata per diagnosticare un blocco dell'ottimizzatore ("freeze Optimize @ 40/40"),
strumentando l'intero percorso: `OnCalcola` → `GetInfoOttimizzatore` →
`InitCompilatore` → `InitSoluzioneEsistente` → `InitDatiSezione` → `linearizza` →
`ottimize_cycle` → `ottimizza`.

Con un dettaglio utile per un ciclo di ricerca combinatoria, dove stampare a ogni
iterazione sarebbe insostenibile:

```cpp
#if CAD_DIAG_CALCULATE
    if ((++g_diagOttPermuta % 1000) == 0)
        CAD_DIAG("ottimizza", "permuta=%u passi_seq=%d", g_diagOttPermuta, passi_seq);
#endif
```

Nei `.pro` di `sim2d` e `ottimizzatore` la definizione resta **commentata**, con la nota
sul perché esiste:

```
# [AI] build diagnostica freeze Optimize @ 40/40 — rimuovere dopo debug
#DEFINES += CAD_DIAG_CALCULATE
```

### E.2 `PerfMonitor` — contatori hardware per iterazione real-time

`SqCom/PerfMonitor.cpp` e `.h`, file nuovi (non ancora committati).

Legge i contatori PMU via `perf_event_open()` **dentro la singola iterazione** del ciclo
RT su CPU3: `cpu_cycles`, `instructions`, `l2d_cache`, `l2d_cache_refill`, `bus_access`,
`bus_cycles`. Traccia la peggiore iterazione per ritardo di risveglio conservandone tutti
i contatori, ed esporta la distribuzione completa in CSV.

Con una finestra di warm-up configurabile (`PERF_WARMUP_ITER`, default 15 000) perché le
prime iterazioni contengono i transitori di avvio e dominavano il massimo riportato.

---

## F. Note per la manutenzione

Punti che vanno chiusi prima di considerare il lavoro consegnabile.

| # | Punto | Azione |
|---|---|---|
| 1 | **File generati sotto controllo di versione** in `pegenstein`: `PegLib/Makefile` (80 582 righe), `Makefile`, `.qmake.stash` | `git rm --cached` + `.gitignore` |
| 2 | **`fprintf` incondizionati residui** in `liste/SaveAsListVw.cpp` (`OnShow`, `OnSelChange`): non passano da `ListeDiag`, quindi sono **sempre attivi** con `fflush` sincrono | ricondurli a `ListeDiag()` |
| 3 | `PerfMonitor_Init()` incondizionata in `RTCHndlr.cpp`: anche una build di produzione apre i contatori e verifica `PerfMonitor_IsEnabled()` a ogni iterazione RT | proteggere con flag di compilazione |
| 4 | `PerfMonitor` e `Lnk/main.cpp` non ancora committati | commit sui rispettivi branch |
| 5 | Refuso in un commento di `Sim2DView.cpp`: `cacharlo` → `cachearlo` | correzione cosmetica |
| 6 | `CAD_DIAG_CALCULATE` resta commentata nei `.pro` | scelta corretta: nessuna azione, ma da non riabilitare per errore |

---

## G. Sintesi per la valutazione

Il lavoro si articola su tre livelli, ciascuno prerequisito del successivo:

**Robustezza.** Un'applicazione che va in crash non può essere misurata per un'ora, e
senza test da un'ora non si osservano eventi che valgono cinque per milione. Le correzioni
del CAD 2D e delle liste non erano l'obiettivo, ma erano la condizione per raggiungerlo.

**Strumentazione.** Prima di ottimizzare serviva poter misurare: `PerfMonitor` per i
contatori hardware, `CAD_DIAG` e `ListeDiag` per la diagnosi funzionale — entrambi
progettati per **costare zero quando disattivati**, perché uno strumento di misura che
altera la grandezza misurata è inutile.

**Ottimizzazione.** Sette interventi, ciascuno con una misura di prima e dopo, che portano
il caso peggiore da 158 a 98 µs con zero superamenti della soglia dei 100 µs su
1 451 000 attivazioni.

Le modifiche condividono un criterio: **sono tutte reversibili e documentate nel punto in
cui stanno**. Ogni compromesso — i 33 ms del pan, i 300 ms del defer, il 15 % del
throttling — ha accanto il commento che spiega cosa si guadagna, cosa si perde e come
tornare indietro.
