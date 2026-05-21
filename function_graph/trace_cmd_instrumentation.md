# Instrumentation con `trace-cmd` e `function_graph`

Questo documento descrive la strategia utilizzata per tracciare, tramite `trace-cmd` e `function_graph`, il comportamento del sistema nell'intorno di uno spike di latenza della `clock_nanosleep()`.

L'obiettivo del file è documentare **il metodo di instrumentation** e **il motivo per cui i punti di tracing sono stati scelti**.

---

## Dove è stato avviato il tracing

Il tracing è stato avviato all'interno del thread di test RTC-like, prima di entrare nel ciclo periodico di attesa.

La logica concettuale era:

```text
se è la prima iterazione e il tracing non è ancora attivo:
    avvia trace-cmd con function_graph
    marca il tracing come attivo
```

Il motivo di questa scelta è che il thread RTC-like è il componente che esegue il ciclo temporale periodico.

Avviare il trace in quel punto permette di catturare l'intera sequenza:

```text
inizio attesa periodica
  -> clock_nanosleep()
  -> interrupt del timer
  -> path hrtimer/wakeup
  -> il thread real-time torna runnable
```

La sessione di tracing veniva avviata con un buffer limitato, ad esempio:

```bash
 if(global_count==0 && active_trace==false)
        {
            system("trace-cmd start -p function_graph -b 30000");
            active_trace=true;
            cout<<"ho attivato il trace"<<endl;
        }
```

Il buffer è stato limitato intenzionalmente perché l'obiettivo non era tracciare il sistema per molto tempo, ma mantenere in memoria solo l'attività recente attorno all'evento interessante ed evitare che l'HMI venissa killata durante l'esecuzione del test.

---

## Perché il tracing non veniva fermato subito

La traccia non veniva fermata dopo un numero fisso di iterazioni.

La strategia utilizzata era invece quella di misurare continuamente il ritardo della `clock_nanosleep()` e fermare il trace solo quando il ritardo superava una soglia scelta.

La logica concettuale era:

```text
esegui clock_nanosleep()
misura il ritardo di risveglio

se il ritardo supera la soglia:
    scrivi un marker nella traccia
    ferma trace-cmd
    considera la traccia valida
```

```bash
f (active_trace==true && diff_us_nanosleep >= 900)
        {
            char cmd[256];
            valide_trace=true;
            snprintf(cmd, sizeof(cmd),
                     "echo NANOSLEEP_SPIKE diff_us_nanosleep=%g us  e attivazione_nanosleep=%g> /sys/kernel/debug/tracing/trace_marker; trace-cmd stop",
                     diff_us_nanosleep,activation_us_nanosleep);
            //cout<<"prima di echo:"<<cmd<<endl;
            system(cmd);
            //cout<<"dopo di echo"<<endl;
             system("trace-cmd stop");
        }
```

Questa scelta è stata fatta perché l'evento interessante è raro.

Fermare il trace solo in presenza di uno spike rende la traccia molto più utile, perché aumenta la probabilità che il buffer contenga proprio il comportamento anomalo.

---

## Perché il controllo dello spike è stato inserito dopo `clock_nanosleep()`

Il controllo della soglia è stato inserito subito dopo il ritorno dalla `clock_nanosleep()`.

Questo punto è fondamentale: prima del ritorno dalla `clock_nanosleep()`, il codice user-space del thread non è in esecuzione. Il thread è addormentato e il lavoro rilevante avviene nel kernel.

Appena la `clock_nanosleep()` ritorna, il programma può calcolare:

```text
tempo_reale_di_risveglio - tempo_teorico_di_risveglio
```

Se questo valore supera la soglia, significa che l'attivazione appena avvenuta è stata problematica.

Per questo motivo il trace viene fermato immediatamente dopo aver rilevato lo spike: in questo modo il buffer di `function_graph` contiene ancora il path kernel che ha portato al risveglio anomalo.

---

## Perché è stato usato `trace_marker`

Quando veniva rilevato uno spike, veniva scritto un messaggio in:

```text
/sys/kernel/debug/tracing/trace_marker
```

Il marker conteneva informazioni come:

```text
NANOSLEEP_SPIKE diff_us_nanosleep=<valore> activation_nanosleep=<valore>
```

Lo scopo del marker era rendere più semplice la lettura della traccia.

Nel trace finale, il marker diventa un punto di riferimento visivo:

```text
prima del marker:
    attività kernel che ha preceduto il risveglio anomalo

marker:
    lo user-space ha rilevato uno spike

dopo il marker:
    operazioni di stop del trace e terminazione del test
```

Questo permette di cercare rapidamente l'evento interessante dentro al trace e collegare il valore numerico della latenza al path kernel osservato.

---

## Perché `trace-cmd stop` veniva chiamato appena rilevato lo spike

`trace-cmd stop` veniva eseguito appena veniva rilevato un risveglio anomalo.

Il motivo è che il buffer di ftrace è circolare.

Se il tracing continuasse troppo a lungo dopo lo spike, la parte interessante della traccia potrebbe essere sovrascritta da attività successive.

La sequenza desiderata era:

```text
spike rilevato
  -> scrittura del marker
  -> stop di trace-cmd
  -> blocco delle iterazioni successive
```

Così viene preservata la porzione di trace che contiene il path kernel responsabile o correlato al ritardo.

---

## Perché è stato usato un flag di validità della traccia

È stato usato un flag per indicare che era stata catturata una traccia valida.

La logica concettuale era:

```text
se viene rilevato uno spike:
    valid_trace = true
```

Il ciclo RTC-like controllava questo flag e, una volta trovata una traccia valida, terminava il test in modo controllato.

Questo evita di continuare il test dopo aver già catturato l'evento interessante.

Inoltre permette di fermare correttamente anche il secondo thread real-time coinvolto nel test.

---

## Perché veniva rilasciato il semaforo durante la chiusura

Il test includeva un secondo thread real-time, simile a un thread PLC fast, che rimaneva in attesa su un semaforo.

Durante il funzionamento normale, il callback RTC rilasciava periodicamente questo semaforo.

Quando veniva rilevato uno spike e il test doveva terminare, il thread in attesa poteva trovarsi ancora bloccato sul semaforo.

Per questo motivo, durante la fase di chiusura, il semaforo veniva rilasciato una volta.

La logica concettuale era:

```text
traccia valida catturata
  -> richiesta di terminazione del thread PLC-like
  -> rilascio del semaforo
  -> attesa della terminazione del thread
```

Questo evita che il thread rimanga bloccato indefinitamente in attesa del semaforo.

---

## Perché `trace-cmd extract` veniva eseguito alla fine

Dopo aver fermato il tracing, i dati venivano estratti in un file:

```bash
trace-cmd extract -o /tmp/trace.dat
```

Questo produce un file `trace.dat` analizzabile successivamente con:

```bash
trace-cmd report /tmp/trace.dat
```

oppure copiabile su un altro computer per analisi offline.

Dopo l'estrazione, il tracer veniva riportato a `nop`:

```bash
echo nop > /sys/kernel/debug/tracing/current_tracer
```

Questo è importante perché `function_graph` è invasivo e non dovrebbe rimanere attivo dopo la fine dell'esperimento.

---

## Flusso generale dell'instrumentation

Il flusso complessivo era:

```text
1. Avvio del test real-time.
2. Avvio di trace-cmd con function_graph.
3. Esecuzione del ciclo periodico basato su clock_nanosleep().
4. Misura del ritardo di risveglio a ogni iterazione.
5. Se il ritardo supera la soglia:
   - scrittura di un marker in trace_marker;
   - stop di trace-cmd;
   - marcatura della traccia come valida;
   - uscita dal ciclo di test.
6. Terminazione controllata del thread real-time secondario.
7. Estrazione della traccia in /tmp/trace.dat.
8. Reset del tracer a nop.
```

---

## Perché questo approccio è stato utile

Questo metodo ha permesso di catturare l'attività kernel nell'intorno esatto di uno spike di latenza.

Le tracce ottenute hanno mostrato che, nei casi peggiori, sulla CPU3 non veniva eseguito un processo utente estraneo prima della ripresa del thread real-time.

Il ritardo sembrava invece collocarsi nel path kernel di timer/wakeup, in particolare attorno a funzioni come:

```text
hrtimer_interrupt()
__hrtimer_run_queues()
hrtimer_wakeup()
try_to_wake_up()
ttwu_do_activate()
```

Questo ha supportato l'ipotesi che il jitter non fosse causato principalmente da normale interferenza di scheduling user-space, ma dall'aumento del costo del path kernel di risveglio.

Successivamente, le misure tramite performance counter hardware hanno mostrato che, nei casi peggiori, il numero di istruzioni eseguite rimaneva simile rispetto ai casi migliori, mentre aumentavano molto i cicli CPU, il CPI e i contatori legati al bus/memoria.

Questo suggerisce che lo stesso path kernel possa diventare molto più lento in presenza di interferenza su cache, memoria e bus, causata ad esempio da HMI, GUI, GPU o sottosistema grafico.

---

## Note sull'overhead

Questo metodo di tracing è invasivo.

Sia `function_graph` sia l'uso di `trace-cmd`, `trace_marker` e comandi lanciati tramite shell introducono overhead.

Per questo motivo, i tempi assoluti osservati all'interno della traccia devono essere interpretati con cautela.

La traccia è utile soprattutto per capire:

- la sequenza degli eventi;
- quali funzioni kernel vengono attraversate;
- se ci sono altri processi utente in mezzo;
- dove si colloca concettualmente il ritardo.

Per misure quantitative più affidabili, il tracing tramite `function_graph` deve essere affiancato a strumenti meno invasivi, come performance counter hardware tramite `perf_event_open()`.
