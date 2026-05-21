# Documentazione del modulo `perf_event_open` per l'analisi del jitter real-time

Questo documento descrive il codice scritto per usare i performance counter hardware tramite `perf_event_open()` all'interno del test real-time.

L'obiettivo è documentare in modo completo:

- cosa fanno le funzioni aggiunte;
- perché sono utili;
- quali eventi hardware vengono misurati;
- dove ha senso abilitare/disabilitare i contatori;
- come aggiungere nuovi eventi;
- cosa fare se i contatori hardware disponibili non bastano;
- quali parti possono essere pubblicate e quali invece vanno solo descritte.

---

## 1. Obiettivo della misura con `perf`

Inizialmente il confronto veniva fatto lanciando `perf stat` esternamente, generando un file per ogni iterazione.

Questo approccio funzionava, ma aveva diversi problemi:

1. creava overhead elevato;
2. lanciava processi esterni;
3. produceva molti file;
4. rendeva più difficile collegare una specifica iterazione al valore dei contatori;
5. non era ideale per misure ripetute su molte migliaia di attivazioni.

Per ridurre l'overhead è stato introdotto l'uso diretto della syscall:

```text
perf_event_open()
```

In questo modo i contatori vengono aperti una sola volta all'inizio, poi a ogni iterazione vengono solo:

```text
reset -> enable -> disable -> read
```

Questa soluzione è molto più leggera rispetto a eseguire ogni volta un comando `perf stat` tramite shell.

---

## 2. Idea generale del funzionamento

L'idea del modulo è:

1. cercare gli eventi hardware disponibili nel sistema;
2. aprire i contatori PMU per ogni CPU da monitorare;
3. raggruppare gli eventi in un unico gruppo per CPU;
4. abilitare tutti i contatori insieme all'inizio della finestra di misura;
5. fermarli subito dopo la finestra di misura;
6. leggere i valori;
7. calcolare metriche derivate;
8. salvare i risultati associandoli all'iterazione corrente;
9. a fine test stampare statistiche globali e per CPU.

La finestra di misura scelta è quella intorno all'attesa periodica basata su `clock_nanosleep()`.

In modo concettuale:

```text
inizio iterazione
  -> reset e start contatori
  -> clock_nanosleep()
  -> ritorno dalla nanosleep
  -> stop e lettura contatori
  -> salvataggio valori dell'iterazione
fine iterazione
```

Questo permette di collegare il ritardo della `clock_nanosleep()` ai valori dei contatori hardware misurati nella stessa finestra temporale.

---

## 3. Eventi misurati

Nel codice vengono misurati questi eventi:

```text
l2d_cache
l2d_cache_refill
bus_access
bus_cycles
cpu_cycles
inst_retired
```

### `l2d_cache`

Conta gli accessi alla cache dati L2.

È utile perché permette di capire quante volte il codice misurato ha interagito con la gerarchia cache L2.

### `l2d_cache_refill`

Conta i refill della cache dati L2.

Un refill avviene quando il dato richiesto non è disponibile dove serve e deve essere recuperato da un livello più lontano, tipicamente dalla memoria principale o da un livello inferiore della gerarchia memoria.

È utile perché, insieme a `l2d_cache`, permette di stimare una percentuale di miss/refill L2:

```text
L2 miss % = l2d_cache_refill / l2d_cache * 100
```

### `bus_access`

Conta gli accessi al bus osservati dalla PMU.

È utile perché dà un'indicazione del traffico verso il sottosistema memoria/interconnect.

### `bus_cycles`

Conta i cicli del bus.

È utile perché dà un'indicazione di quanto tempo il bus risulta coinvolto/occupato durante la finestra di misura.

Dal rapporto tra `bus_access` e `bus_cycles` si calcolano due metriche:

```text
bus_access/bus_cycles
bus_cycles/bus_access
```

La prima dà un'idea di quanti accessi bus avvengono rispetto ai cicli bus.  
La seconda dà un'idea di quanti cicli bus corrispondono mediamente a un accesso.

Non va interpretata come una latenza fisica esatta di ogni accesso, ma è utile come indicatore relativo tra scenari diversi.

### `cpu_cycles`

Conta i cicli CPU.

È fondamentale per capire quanto tempo di CPU viene consumato durante la finestra di misura.

Se il numero di istruzioni resta simile ma `cpu_cycles` cresce molto, significa che il codice sta avanzando più lentamente.

### `inst_retired`

Conta le istruzioni ritirate, cioè completate.

È utile perché permette di calcolare:

```text
IPC = inst_retired / cpu_cycles
CPI = cpu_cycles / inst_retired
```

### `IPC`

IPC significa Instructions Per Cycle.

```text
IPC = istruzioni / cicli CPU
```

Più è alto, più la CPU sta avanzando bene.

### `CPI`

CPI significa Cycles Per Instruction.

```text
CPI = cicli CPU / istruzioni
```

Più è alto, più ogni istruzione costa mediamente molti cicli.

Nei casi peggiori osservati, il numero di istruzioni rimaneva spesso simile rispetto ai casi migliori, ma `cpu_cycles` e `CPI` aumentavano molto. Questo suggerisce che il problema non fosse semplicemente "più codice eseguito", ma maggiore tempo perso in attese/stall dovuti a cache, bus o memoria.

---

## 4. Inclusioni necessarie

Per usare `perf_event_open()` servono alcuni header Linux specifici:

```cpp
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <errno.h>
#include <stdint.h>
```

Servono per:

- definire `struct perf_event_attr`;
- usare `ioctl()` sui file descriptor dei contatori;
- chiamare direttamente la syscall `perf_event_open`;
- leggere sysfs;
- scorrere le directory delle PMU;
- gestire stringhe e parsing dei valori numerici.

---

## 5. `perf_event_open_syscall()`

```cpp
static long perf_event_open_syscall(struct perf_event_attr *hw_event,
                                    pid_t pid,
                                    int cpu,
                                    int group_fd,
                                    unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}
```

Questa funzione è un wrapper della syscall `perf_event_open()`.

Serve perché in C/C++ non sempre esiste una funzione di libreria già pronta chiamata `perf_event_open()`, quindi si invoca direttamente la syscall con:

```cpp
syscall(__NR_perf_event_open, ...)
```

I parametri principali sono:

### `hw_event`

Puntatore a una `struct perf_event_attr`.

Questa struttura descrive quale evento si vuole contare.

Contiene informazioni come:

```text
tipo PMU
config evento
se il contatore parte disabilitato
se contare user-space
se contare kernel-space
formato di lettura
```

### `pid`

Nel codice viene usato:

```cpp
pid = -1
```

Questo significa che non si sta misurando un singolo processo, ma si sta aprendo un contatore legato a una CPU specifica.

### `cpu`

Indica la CPU su cui aprire il contatore.

Per esempio:

```text
CPU0
CPU1
CPU2
CPU3
```

### `group_fd`

Serve per creare gruppi di eventi.

Il primo evento viene aperto con:

```cpp
group_fd = -1
```

e diventa il leader del gruppo.

Gli eventi successivi vengono aperti passando come `group_fd` il file descriptor del leader.

Così tutti gli eventi del gruppo vengono abilitati, disabilitati, resettati e letti insieme.

### `flags`

Nel codice è impostato a `0`.

---

## 6. `PmuEventDesc`

```cpp
struct PmuEventDesc
{
    bool valid = false;
    int type = -1;
    uint64_t config = 0;
    std::string pmu_name;
    std::string event_name;
};
```

Questa struttura descrive un evento PMU trovato nel sistema.

Contiene:

### `valid`

Indica se l'evento è stato trovato correttamente.

### `type`

È il tipo della PMU.

Questo valore viene letto da:

```text
/sys/bus/event_source/devices/<pmu>/type
```

Serve perché `perf_event_open()` richiede di sapere a quale PMU appartiene l'evento.

### `config`

È il codice numerico dell'evento hardware.

Viene letto da file come:

```text
/sys/bus/event_source/devices/<pmu>/events/<event_name>
```

Esempio:

```text
event=0x16
```

### `pmu_name`

Nome della PMU trovata, ad esempio:

```text
armv8_cortex_a53
```

oppure un nome simile esposto dal kernel.

### `event_name`

Nome logico dell'evento richiesto, ad esempio:

```text
l2d_cache_refill
```

---

## 7. `read_text_file()`

```cpp
static std::string read_text_file(const std::string& path)
{
    std::ifstream f(path.c_str());
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
```

Questa funzione legge interamente un file di testo e restituisce il contenuto come `std::string`.

È usata per leggere file sysfs, ad esempio:

```text
/sys/bus/event_source/devices/<pmu>/type
/sys/bus/event_source/devices/<pmu>/events/<event_name>
```

È utile perché in Linux molte informazioni hardware e kernel vengono esposte come file testuali dentro `/sys`.

---

## 8. `trim_copy()`

```cpp
static std::string trim_copy(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();

    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
        start++;

    return s.substr(start);
}
```

Questa funzione rimuove spazi, tab, newline e carriage return all'inizio e alla fine di una stringa.

Serve perché i file sysfs spesso contengono valori con newline finale.

Esempio:

```text
"0x16\n"
```

diventa:

```text
"0x16"
```

Senza questa pulizia, il parsing numerico potrebbe diventare più fragile.

---

## 9. `parse_uint64_auto()`

```cpp
static bool parse_uint64_auto(const std::string& s, uint64_t& out)
{
    char* end = nullptr;
    errno = 0;
    unsigned long long v = strtoull(s.c_str(), &end, 0);

    if (errno != 0 || end == s.c_str())
        return false;

    out = static_cast<uint64_t>(v);
    return true;
}
```

Questa funzione converte una stringa in un numero `uint64_t`.

Usa:

```cpp
strtoull(..., base = 0)
```

Il fatto di usare base `0` è importante perché permette di interpretare automaticamente:

```text
"22"     come decimale
"0x16"   come esadecimale
```

Restituisce:

```text
true  -> conversione riuscita
false -> conversione fallita
```

È utile perché i codici evento in sysfs possono essere scritti sia in decimale sia in esadecimale.

---

## 10. `find_pmu_event_by_name()`

```cpp
static bool find_pmu_event_by_name(const std::string& event_name, PmuEventDesc& out)
```

Questa funzione cerca un evento PMU in sysfs.

Il percorso principale è:

```text
/sys/bus/event_source/devices
```

La funzione scorre tutte le PMU disponibili e cerca un file evento con il nome richiesto:

```text
/sys/bus/event_source/devices/<pmu>/events/<event_name>
```

Per esempio:

```text
/sys/bus/event_source/devices/armv8_cortex_a53/events/l2d_cache
```

Se il file esiste, legge anche:

```text
/sys/bus/event_source/devices/<pmu>/type
```

e salva:

```text
type
config
pmu_name
event_name
```

La logica è:

```text
1. apro /sys/bus/event_source/devices
2. scorro tutte le PMU disponibili
3. per ogni PMU controllo se esiste events/<event_name>
4. se esiste, leggo il type della PMU
5. leggo il contenuto del file evento
6. estraggo event=<valore> oppure config=<valore>
7. salvo tutto in PmuEventDesc
```

Questa funzione è molto utile perché evita di hardcodare i codici raw degli eventi.

Invece di scrivere direttamente:

```text
config = 0x16
```

si usa il nome evento:

```text
l2d_cache
```

e il programma trova automaticamente il valore corretto esposto dal kernel.

---

## 11. `PerfCpuGroup`

```cpp
struct PerfCpuGroup
{
    int cpu = -1;

    int fd_leader_l2_cache = -1;
    int fd_l2_refill = -1;

    int fd_bus_access = -1;
    int fd_bus_cycles = -1;

    int fd_cpu_cycles = -1;
    int fd_inst_retired = -1;
};
```

Questa struttura rappresenta tutti i contatori aperti su una singola CPU.

Ogni campo `fd_*` è un file descriptor restituito da `perf_event_open()`.

Per ogni CPU viene creato un gruppo di eventi:

```text
leader  -> l2d_cache
sibling -> l2d_cache_refill
sibling -> bus_access
sibling -> bus_cycles
sibling -> cpu_cycles
sibling -> inst_retired
```

Il leader è importante perché tutte le operazioni di gruppo vengono eseguite sul file descriptor del leader:

```cpp
ioctl(fd_leader, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP)
ioctl(fd_leader, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP)
ioctl(fd_leader, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP)
read(fd_leader, ...)
```

Così tutti gli eventi del gruppo sono misurati nella stessa finestra temporale.

---

## 12. `PerfCpuIterationStats`

```cpp
struct PerfCpuIterationStats
{
    int cpu = -1;

    uint64_t l2d_cache = 0;
    uint64_t l2d_cache_refill = 0;
    double l2_miss_percent = 0.0;

    uint64_t bus_access = 0;
    uint64_t bus_cycles = 0;

    double bus_access_per_cycle = 0.0;
    double bus_cycles_per_access = 0.0;

    uint64_t cpu_cycles = 0;

    uint64_t inst_retired = 0;
    double ipc = 0.0;
    double cpi = 0.0;
};
```

Questa struttura contiene i valori misurati per una singola CPU in una singola iterazione.

Per esempio, per la CPU3 all'iterazione peggiore, contiene:

```text
l2d_cache
l2d_cache_refill
bus_access
bus_cycles
cpu_cycles
inst_retired
IPC
CPI
```

È utile perché permette di analizzare non solo i totali globali, ma anche quale CPU ha contribuito maggiormente al comportamento osservato.

---

## 13. `PerfIterationStats`

```cpp
struct PerfIterationStats
{
    bool valid = false;
    int iter = -1;

    double sleep_delay_us = 0.0;

    uint64_t l2d_cache = 0;
    uint64_t l2d_cache_refill = 0;
    double l2_miss_percent = 0.0;

    uint64_t bus_access = 0;
    uint64_t bus_cycles = 0;
    double bus_access_per_cycle = 0.0;
    double bus_cycles_per_access = 0.0;

    uint64_t cpu_cycles = 0;

    uint64_t inst_retired = 0;
    double ipc = 0.0;
    double cpi = 0.0;

    std::vector<PerfCpuIterationStats> per_cpu;
};
```

Questa struttura rappresenta i risultati completi di una iterazione.

Contiene:

1. il ritardo della `clock_nanosleep()`;
2. i valori aggregati su tutte le CPU;
3. il dettaglio per CPU.

Il campo `valid` serve perché non tutte le posizioni del vettore possono contenere dati validi.

Il campo `per_cpu` contiene una lista di `PerfCpuIterationStats`, una per ogni CPU monitorata.

Questa struttura è quella che permette alla fine del test di trovare:

```text
iterazione con sleep minimo
iterazione con sleep massimo
medie globali
statistiche per CPU
```

---

## 14. Classe `PerfL2Monitor`

La classe `PerfL2Monitor` è il componente principale del modulo.

Si occupa di:

```text
inizializzare i contatori
aprire gli eventi
resettarli
abilitarli
disabilitarli
leggerli
chiuderli
```

Anche se il nome contiene `L2`, nella versione attuale la classe misura anche:

```text
bus_access
bus_cycles
cpu_cycles
inst_retired
```

Quindi il nome potrebbe eventualmente essere generalizzato in futuro, ad esempio:

```text
PerfMonitor
```

oppure:

```text
PerfCpuMonitor
```

---

## 15. `PerfL2Monitor::init()`

```cpp
bool init(const std::vector<int>& cpus)
```

Questa funzione inizializza il monitor dei contatori sulle CPU passate come parametro.

Nel codice viene usata una chiamata concettuale del tipo:

```cpp
g_perf_l2.init({0, 1, 2, 3})
```

Questo significa:

```text
apri i contatori su CPU0, CPU1, CPU2 e CPU3
```

### 15.1 Ricerca degli eventi

La funzione cerca prima tutti gli eventi necessari:

```cpp
find_pmu_event_by_name("l2d_cache", ev_l2_cache)
find_pmu_event_by_name("l2d_cache_refill", ev_l2_refill)
find_pmu_event_by_name("bus_access", ev_bus_access)
find_pmu_event_by_name("bus_cycles", ev_bus_cycles)
find_pmu_event_by_name("cpu_cycles", ev_cpu_cycles)
find_pmu_event_by_name("inst_retired", ev_inst_retired)
```

Se un evento non viene trovato, l'inizializzazione fallisce.

Questo è utile perché evita di fare misure sbagliate se la piattaforma non espone un evento.

### 15.2 Controllo che gli eventi appartengano alla stessa PMU

Nel codice viene controllato che tutti gli eventi abbiano lo stesso `type`.

Questo è importante perché, per semplicità, tutti gli eventi vengono aperti nello stesso gruppo.

Un gruppo perf deve contenere eventi compatibili tra loro e appartenenti alla stessa PMU.

Se gli eventi appartengono a PMU diverse, il codice stampa un errore e termina l'inizializzazione.

### 15.3 Apertura del leader

Il primo evento aperto è:

```text
l2d_cache
```

Questo diventa il leader del gruppo.

Viene aperto con:

```cpp
g.fd_leader_l2_cache = perf_event_open_syscall(&pe, -1, cpu, -1, 0);
```

I parametri significano:

```text
pid = -1       -> misura system-wide sulla CPU indicata
cpu = cpu      -> CPU corrente del ciclo
group_fd = -1  -> questo evento è il leader
flags = 0
```

Il leader viene creato con:

```cpp
pe.disabled = 1;
```

Quindi il contatore nasce disabilitato.

### 15.4 Apertura degli eventi sibling

Gli altri eventi vengono aperti come sibling del leader:

```cpp
perf_event_open_syscall(&pe, -1, cpu, g.fd_leader_l2_cache, 0)
```

Questo collega ogni evento al gruppo del leader.

Gli eventi sibling sono:

```text
l2d_cache_refill
bus_access
bus_cycles
cpu_cycles
inst_retired
```

### 15.5 Perché usare un gruppo

Il gruppo serve perché vogliamo che tutti gli eventi siano misurati nella stessa finestra temporale.

Con un gruppo possiamo fare:

```cpp
ioctl(leader, RESET, GROUP)
ioctl(leader, ENABLE, GROUP)
ioctl(leader, DISABLE, GROUP)
read(leader, ...)
```

e ottenere valori coerenti.

Se gli eventi fossero gestiti separatamente, ci sarebbe il rischio di abilitare o disabilitare un contatore leggermente prima degli altri.

### 15.6 Reset iniziale

Alla fine dell'apertura di ogni gruppo CPU viene fatto:

```cpp
ioctl(g.fd_leader_l2_cache, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
ioctl(g.fd_leader_l2_cache, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
```

Così il gruppo parte in uno stato noto:

```text
disabilitato
azzerato
pronto per essere usato
```

---

## 16. `PerfL2Monitor::start()`

```cpp
void start()
```

Questa funzione avvia la misura.

Per ogni CPU:

```cpp
ioctl(g.fd_leader_l2_cache, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
ioctl(g.fd_leader_l2_cache, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
```

Prima resetta i contatori e poi li abilita.

Questo è importante perché ogni iterazione deve essere indipendente dalle precedenti.

La finestra concettuale è:

```text
start()
  -> reset contatori
  -> abilita contatori
  -> inizia finestra di misura
```

---

## 17. `PerfL2Monitor::stop_and_read()`

```cpp
bool stop_and_read(uint64_t& total_l2d_cache,
                   uint64_t& total_l2d_cache_refill,
                   uint64_t& total_bus_access,
                   uint64_t& total_bus_cycles,
                   uint64_t& total_cpu_cycles,
                   uint64_t& total_inst_retired,
                   std::vector<PerfCpuIterationStats>& per_cpu_stats)
```

Questa funzione ferma i contatori e legge i valori.

### 17.1 Prima disabilita tutti i gruppi

La prima cosa che fa è:

```cpp
ioctl(g.fd_leader_l2_cache, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
```

per ogni CPU.

Questo congela la finestra di misura.

È importante disabilitare prima tutti i gruppi e poi leggerli, così si riduce la differenza temporale tra CPU diverse.

### 17.2 Struttura di lettura

Il codice usa:

```cpp
struct ReadGroup
{
    uint64_t nr;
    uint64_t values[6];
};
```

`nr` indica quanti valori sono presenti.

`values` contiene i valori degli eventi del gruppo.

Poiché gli eventi sono sei, il vettore ha dimensione 6.

### 17.3 Ordine dei valori

L'ordine dei valori è l'ordine con cui gli eventi sono stati aperti:

```text
values[0] = l2d_cache
values[1] = l2d_cache_refill
values[2] = bus_access
values[3] = bus_cycles
values[4] = cpu_cycles
values[5] = inst_retired
```

Questa parte è importantissima: se si aggiunge, rimuove o riordina un evento, bisogna aggiornare anche la lettura.

### 17.4 Calcolo metriche per CPU

Per ogni CPU vengono calcolate:

```text
L2 miss %
bus_access/bus_cycles
bus_cycles/bus_access
IPC
CPI
```

Formule:

```text
L2 miss % = l2d_cache_refill / l2d_cache * 100

bus_access/bus_cycles = bus_access / bus_cycles

bus_cycles/bus_access = bus_cycles / bus_access

IPC = inst_retired / cpu_cycles

CPI = cpu_cycles / inst_retired
```

Tutte le divisioni vengono protette controllando che il denominatore sia maggiore di zero.

### 17.5 Aggregazione totale

Dopo aver calcolato i valori per CPU, la funzione somma i valori grezzi:

```text
total_l2d_cache
total_l2d_cache_refill
total_bus_access
total_bus_cycles
total_cpu_cycles
total_inst_retired
```

I valori totali sono poi usati per calcolare le metriche globali dell'iterazione.

---

## 18. `PerfL2Monitor::close_all()`

```cpp
void close_all()
```

Questa funzione chiude tutti i file descriptor aperti.

Per ogni CPU chiude:

```text
fd_inst_retired
fd_cpu_cycles
fd_bus_cycles
fd_bus_access
fd_l2_refill
fd_leader_l2_cache
```

Chiudere i file descriptor è importante perché ogni contatore perf aperto consuma risorse kernel.

Dopo la chiusura:

```cpp
groups.clear();
initialized = false;
```

Così l'oggetto torna in uno stato pulito.

---

## 19. Distruttore `~PerfL2Monitor()`

```cpp
~PerfL2Monitor()
{
    close_all();
}
```

Il distruttore chiama `close_all()`.

Questo è utile perché se l'oggetto viene distrutto, i file descriptor vengono chiusi automaticamente.

---

## 20. Variabili globali

```cpp
static PerfL2Monitor g_perf_l2;
static std::vector<PerfIterationStats> g_perf_stats;
```

### `g_perf_l2`

È l'oggetto globale che gestisce i contatori.

### `g_perf_stats`

È il vettore che contiene i risultati di tutte le iterazioni.

Viene dimensionato all'inizio del test, ad esempio:

```cpp
g_perf_stats.resize(MAX_ACTIVATIONS + 10);
```

Il `+ 10` serve come margine di sicurezza.

---

## 21. `FindCpuStatsInIteration()`

```cpp
static const PerfCpuIterationStats* FindCpuStatsInIteration(const PerfIterationStats& iter_stats,
                                                            int cpu)
```

Questa funzione cerca i dati di una specifica CPU dentro una specifica iterazione.

Scorre:

```cpp
iter_stats.per_cpu
```

e restituisce il puntatore alla struttura della CPU richiesta.

Se non trova la CPU, restituisce `nullptr`.

È utile durante la stampa finale, perché bisogna mostrare i valori della CPU specifica per l'iterazione con sleep minimo e massimo.

---

## 22. `SavePerfStatsForIteration()`

```cpp
static void SavePerfStatsForIteration(int iter, double sleep_delay_us)
```

Questa funzione salva i valori perf relativi a una singola iterazione.

La logica è:

```text
1. verifica che l'indice iter sia valido
2. chiama stop_and_read()
3. legge i totali e il dettaglio per CPU
4. calcola le metriche globali
5. salva tutto in g_perf_stats[iter]
```

Questa funzione collega il mondo timing al mondo performance counter.

Infatti riceve:

```text
iter
sleep_delay_us
```

e li associa ai valori hardware misurati nella stessa finestra.

Il risultato è che, alla fine del test, è possibile dire:

```text
l'iterazione con sleep massimo aveva:
    X cpu_cycles
    Y istruzioni
    Z CPI
    K bus_cycles
    ...
```

Questa è la parte fondamentale per correlare jitter e interferenza microarchitetturale.

---

## 23. Funzioni di formattazione

### `fmt_double()`

```cpp
static std::string fmt_double(double value, int precision = 4)
```

Formatta un numero `double` con un numero fisso di decimali.

Serve per stampare output più leggibile.

### `print_separator_line()`

Stampa una linea separatrice principale.

### `print_subseparator_line()`

Stampa una linea separatrice secondaria.

Queste funzioni non fanno misure, ma rendono l'output finale più chiaro e più facilmente copiabile in report o documentazione.

---

## 24. `print_cpu_iteration_block()`

```cpp
static void print_cpu_iteration_block(const std::string& title,
                                      double sleep_delay_us,
                                      int iter,
                                      const PerfCpuIterationStats* s)
```

Questa funzione stampa un blocco di dati relativo a una singola CPU in una specifica iterazione.

Viene usata per stampare, ad esempio:

```text
CPU3 - iterazione con MAX SLEEP
CPU3 - iterazione con MIN SLEEP
```

Stampa:

```text
iterazione
ritardo sleep
l2d_cache
l2d_cache_refill
L2 cache miss
bus_access
bus_cycles
bus_access/bus_cycles
bus_cycles/bus_access
cpu_cycles
numero_istruzioni
IPC
CPI
```

È utile perché permette di confrontare il comportamento di una singola CPU nel caso migliore e nel caso peggiore.

---

## 25. `print_total_iteration_block()`

```cpp
static void print_total_iteration_block(const std::string& title,
                                        const PerfIterationStats& s)
```

Questa funzione stampa i valori totali aggregati su tutte le CPU per una specifica iterazione.

Viene usata per stampare:

```text
[MIN SLEEP]
[MAX SLEEP]
```

a livello globale.

È utile perché mostra il comportamento complessivo del sistema nella finestra misurata.

---

## 26. `PrintFinalPerfStats()`

```cpp
static void PrintFinalPerfStats()
```

Questa funzione viene chiamata alla fine del test e produce il report finale.

Fa diverse cose:

### 26.1 Trova minimo e massimo sleep

Scorre tutte le iterazioni valide e trova:

```text
iterazione con sleep minimo
iterazione con sleep massimo
```

### 26.2 Calcola medie globali

Calcola le medie su tutte le iterazioni valide:

```text
media cpu_cycles
media inst_retired
media IPC
media CPI
media cache miss
media bus_access/bus_cycles
media bus_cycles/bus_access
```

### 26.3 Stampa statistiche totali

Stampa:

```text
STATISTICHE FINALI PERF TOTALI
```

con:

```text
iterazioni valide
medie globali
blocco [MIN SLEEP]
blocco [MAX SLEEP]
```

### 26.4 Trova le CPU presenti

Scorre tutte le iterazioni e costruisce la lista delle CPU per cui sono disponibili dati.

Poi ordina le CPU in ordine crescente.

### 26.5 Stampa statistiche per CPU

Per ogni CPU stampa:

```text
medie della CPU
[MIN SLEEP] per quella CPU
[MAX SLEEP] per quella CPU
```

Questo è particolarmente utile perché il valore totale può nascondere differenze importanti tra CPU.

Nel caso del test real-time, la CPU più interessante è spesso quella su cui gira il thread critico, ad esempio CPU3.

---

## 27. Dove viene inizializzato il monitor perf



La logica è:

```text
1. dimensiona il vettore dei risultati
2. inizializza i contatori perf sulle CPU desiderate
3. se l'inizializzazione fallisce, stampa un warning
4. prosegue eventualmente il test, ma senza dati perf validi
```

In forma concettuale:

```text
g_perf_stats.resize(numero_massimo_iterazioni + margine)

if inizializzazione perf fallisce:
    stampa warning
```

Questa posizione è utile perché:

- i contatori vengono aperti una sola volta;
- si evita overhead durante l'inizializzazione di ogni singola iterazione;
- il test real-time può poi limitarsi a fare start/stop/read.

---

## 28. Dove vengono avviati i contatori


Nel test, la finestra interessante è quella che contiene la `clock_nanosleep()` e il suo risveglio.

La logica è:

```text
inizio iterazione real-time:
    reset + enable dei contatori perf
    esecuzione della clock_nanosleep()
```

Questa posizione è importante perché si vuole misurare ciò che accade durante:

```text
thread addormentato
interrupt del timer
path hrtimer/wakeup
ritorno dalla nanosleep
```

Se i contatori venissero avviati troppo prima, misurerebbero anche attività non rilevante.  
Se venissero avviati troppo dopo, perderebbero il path kernel interessante.

---

## 29. Dove vengono fermati e letti i contatori

 i contatori vengono fermati e letti subito dopo il ritorno dalla `clock_nanosleep()`.

Questo punto è fondamentale perché appena la `clock_nanosleep()` ritorna si conosce il ritardo effettivo dell'attivazione.

La logica è:

```text
ritorno da clock_nanosleep()
calcolo ritardo sleep
disable + read dei contatori
salvataggio risultati dell'iterazione
```

Questa posizione permette di associare:

```text
ritardo sleep della iterazione N
```

ai contatori hardware misurati nella stessa finestra temporale.

---

## 30. Dove viene stampato il report finale

La stampa finale viene fatta dopo la conclusione del test real-time.

La logica è:

```text
fine test real-time
  -> stampa statistiche finali perf
```

Questo è utile perché il report finale ha bisogno di vedere tutte le iterazioni per calcolare:

```text
minimo
massimo
medie
statistiche per CPU
```

---

## 31. Come aggiungere un nuovo evento

Per aggiungere un nuovo evento, ad esempio:

```text
branch_misses
```

oppure un evento disponibile nella propria PMU, bisogna modificare più punti del codice.

### 31.1 Verificare che l'evento esista

Prima controllare sulla board:

```bash
ls /sys/bus/event_source/devices/armv8_cortex_a53/events
```

oppure:

```bash
perf list | grep -i nome_evento
```

Il nome usato nel codice deve coincidere con il nome del file evento in sysfs.

Esempio:

```text
inst_retired
cpu_cycles
bus_access
l2d_cache_refill
```

### 31.2 Aggiungere un campo in `PerfCpuGroup`

Se l'evento si chiama `new_event`, aggiungere:

```cpp
int fd_new_event = -1;
```

### 31.3 Aggiungere un campo in `PerfCpuIterationStats`

Aggiungere:

```cpp
uint64_t new_event = 0;
```

Se serve una metrica derivata, aggiungere anche un `double`.

### 31.4 Aggiungere un campo in `PerfIterationStats`

Aggiungere il totale globale:

```cpp
uint64_t new_event = 0;
```

e l'eventuale metrica derivata.

### 31.5 Cercare l'evento in `init()`

Aggiungere:

```cpp
PmuEventDesc ev_new_event;
```

e poi:

```cpp
if (!find_pmu_event_by_name("new_event", ev_new_event))
{
    std::cerr << "Errore: evento PMU 'new_event' non trovato in sysfs.\n";
    return false;
}
```

### 31.6 Controllare che appartenga alla stessa PMU

Nel controllo dei `type`, aggiungere anche:

```cpp
ev_l2_cache.type != ev_new_event.type
```

Se l'evento appartiene a una PMU diversa, non può essere inserito nello stesso gruppo in questo modo.

### 31.7 Stampare la configurazione dell'evento

Aggiungere una stampa:

```cpp
std::cout << "new_event config=0x"
          << std::hex << ev_new_event.config << std::dec << "\n";
```

### 31.8 Aprire il contatore come sibling

Dopo gli altri eventi:

```cpp
memset(&pe, 0, sizeof(pe));

pe.type = ev_new_event.type;
pe.size = sizeof(pe);
pe.config = ev_new_event.config;
pe.disabled = 0;
pe.exclude_user = 0;
pe.exclude_kernel = 0;
pe.exclude_hv = 0;
pe.read_format = PERF_FORMAT_GROUP;

g.fd_new_event = perf_event_open_syscall(&pe, -1, cpu, g.fd_leader_l2_cache, 0);

if (g.fd_new_event == -1)
{
    std::cerr << "Errore apertura new_event su CPU" << cpu << ": ";
    perror("perf_event_open");
    close_all();
    return false;
}
```

### 31.9 Aumentare la dimensione di `ReadGroup`

Se prima c'erano 6 eventi:

```cpp
uint64_t values[6];
```

e se ne aggiunge uno, bisogna cambiare in:

```cpp
uint64_t values[7];
```

Anche il controllo va aggiornato:

```cpp
if (rg.nr < 7)
```

### 31.10 Aggiornare l'ordine dei valori

Se il nuovo evento è stato aperto come settimo evento:

```cpp
uint64_t cpu_new_event = rg.values[6];
```

L'indice dipende dall'ordine di apertura.

### 31.11 Salvare il valore per CPU

Dentro `PerfCpuIterationStats`:

```cpp
cpu_stats.new_event = cpu_new_event;
```

### 31.12 Aggregare il totale

Aggiungere:

```cpp
total_new_event += cpu_new_event;
```

### 31.13 Modificare `SavePerfStatsForIteration()`

Aggiungere il nuovo totale nei parametri, nelle variabili locali e nel salvataggio:

```cpp
g_perf_stats[iter].new_event = total_new_event;
```

### 31.14 Stampare il nuovo evento

Aggiungere il campo in:

```text
print_cpu_iteration_block()
print_total_iteration_block()
PrintFinalPerfStats()
```

Se serve una media, aggiungere anche una variabile di somma nel ciclo di `PrintFinalPerfStats()`.

### 31.15 Chiudere il file descriptor

In `close_all()` aggiungere:

```cpp
if (g.fd_new_event >= 0)
{
    close(g.fd_new_event);
    g.fd_new_event = -1;
}
```

Questa parte è importante per non lasciare file descriptor aperti.

---

## 32. Problema: i contatori hardware disponibili possono finire

Un problema possibile è che la PMU hardware abbia un numero limitato di contatori programmabili.

Per esempio, se la CPU ha pochi counter disponibili e si provano ad aprire troppi eventi nello stesso gruppo, può accadere che:

```text
perf_event_open() fallisca
```

con errori come:

```text
Invalid argument
No space left on device
Device or resource busy
```

oppure che il gruppo non possa essere schedulato correttamente.

Questo accade perché non tutti gli eventi possono essere misurati simultaneamente in hardware.

---

## 33. Come capire se i counter sono finiti

Sintomi possibili:

```text
errore apertura evento su CPUx
perf_event_open: No space left on device
perf_event_open: Invalid argument
read perf group fallisce
rg.nr minore del numero atteso
valori sempre zero per alcuni eventi
```

Per verificare, si può provare con `perf stat`:

```bash
perf stat -a -A -e l2d_cache,l2d_cache_refill,bus_access,bus_cycles,cpu_cycles,inst_retired sleep 1
```

Se anche `perf stat` segnala problemi o multiplexing, allora probabilmente si sta chiedendo troppo alla PMU.

---

## 34. Soluzione 1: ridurre il numero di eventi

La soluzione più semplice è misurare meno eventi contemporaneamente.

Per esempio, invece di misurare:

```text
l2d_cache
l2d_cache_refill
bus_access
bus_cycles
cpu_cycles
inst_retired
```

si può dividere in due test:

### Test A: memoria/cache

```text
l2d_cache
l2d_cache_refill
bus_access
bus_cycles
```

### Test B: esecuzione CPU

```text
cpu_cycles
inst_retired
```

Il vantaggio è che i contatori non finiscono.

Lo svantaggio è che i valori non sono misurati nella stessa identica iterazione, quindi il confronto è meno diretto.

---

## 35. Soluzione 2: creare più gruppi

Un'altra soluzione è creare più gruppi di eventi.

Per esempio:

```text
Gruppo 1:
    l2d_cache
    l2d_cache_refill
    bus_access

Gruppo 2:
    bus_cycles
    cpu_cycles
    inst_retired
```

Ogni gruppo ha un suo leader.

A ogni iterazione bisogna:

```text
reset gruppo 1
reset gruppo 2
enable gruppo 1
enable gruppo 2
disable gruppo 1
disable gruppo 2
read gruppo 1
read gruppo 2
```

Questa soluzione mantiene la misura nella stessa iterazione, ma aumenta la complessità del codice.

Inoltre, se la PMU non ha abbastanza contatori fisici, il kernel potrebbe comunque dover multiplexare gli eventi.

---

## 36. Soluzione 3: usare multiplexing perf

Il kernel perf può multiplexare eventi quando i contatori hardware non bastano.

Questo significa che non tutti gli eventi vengono misurati per tutta la durata della finestra: alcuni vengono attivati per una parte del tempo e poi scalati.

Per usare correttamente il multiplexing bisognerebbe aggiungere nel `read_format`:

```cpp
PERF_FORMAT_TOTAL_TIME_ENABLED
PERF_FORMAT_TOTAL_TIME_RUNNING
```

In questo modo si possono leggere anche:

```text
time_enabled
time_running
```

e correggere i valori.

Formula concettuale:

```text
valore_scalato = valore_letto * time_enabled / time_running
```

Tuttavia, per finestre molto brevi e real-time, il multiplexing può essere poco desiderabile, perché introduce incertezza.

Per questo motivo, in un'analisi real-time è spesso preferibile misurare meno eventi ma realmente simultanei.

---

## 37. Soluzione 4: fare più run separate

Se si vogliono tanti eventi ma pochi contatori sono disponibili, si possono fare run separate.

Esempio:

```text
Run 1:
    l2d_cache
    l2d_cache_refill

Run 2:
    bus_access
    bus_cycles

Run 3:
    cpu_cycles
    inst_retired
```

Questo approccio è semplice e robusto, ma richiede che gli scenari siano ripetibili.

È utile quando si confrontano condizioni come:

```text
no HMI
HMI attiva
HMI con interazione
kmscube
modetest statico
```

---

## 38. Soluzione 5: misurare solo la CPU interessata

Se il problema principale riguarda CPU3, si può ridurre il numero di CPU monitorate.

Invece di:

```cpp
g_perf_l2.init({0, 1, 2, 3})
```

si può usare concettualmente:

```cpp
g_perf_l2.init({3})
```

Questo non riduce il numero di eventi per singola CPU, ma riduce il numero totale di file descriptor e il lavoro di lettura.

È utile per abbassare overhead e semplificare l'analisi.

---

## 39. Soluzione 6: rendere configurabile la lista eventi

Una possibile evoluzione del codice è rendere la lista eventi configurabile.

Invece di avere campi fissi:

```text
fd_l2_refill
fd_bus_access
fd_bus_cycles
...
```

si potrebbe creare una struttura generica:

```cpp
struct PerfEvent
{
    std::string name;
    PmuEventDesc desc;
    int fd;
    uint64_t value;
};
```

e poi usare un vettore:

```cpp
std::vector<PerfEvent> events;
```

Questo renderebbe molto più semplice aggiungere o rimuovere eventi.

Tuttavia, per una prima versione sperimentale, la struttura esplicita usata nel codice è più semplice da capire e da debuggare.

---

## 40. Nota sulla pubblicabilità del codice

La parte pubblicabile è il modulo di gestione dei contatori:

```text
perf_event_open_syscall()
PmuEventDesc
read_text_file()
trim_copy()
parse_uint64_auto()
find_pmu_event_by_name()
PerfCpuGroup
PerfCpuIterationStats
PerfIterationStats
PerfL2Monitor
SavePerfStatsForIteration()
PrintFinalPerfStats()
funzioni di stampa
```

Per questa parte è sufficiente documentare:

```text
dove vengono inizializzati i contatori
dove vengono avviati
dove vengono fermati/letti
perché quelle posizioni sono state scelte
```

---

## 41. Codice pubblicabile del modulo perf

Il seguente codice rappresenta la parte pubblicabile relativa alla gestione dei performance counter.

```cpp
#ifdef OS_LINUX
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <dirent.h>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <errno.h>
#include <stdint.h>

#ifdef OS_LINUX


//Wrapper della syscall perf_event_open() -> aprimi un contatore hardware per un certo evento, su una certa CPU
static long perf_event_open_syscall(struct perf_event_attr *hw_event,
                                    pid_t pid,
                                    int cpu,
                                    int group_fd,
                                    unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

//questa struttura mi serve per descrivere un evento perf
struct PmuEventDesc
{
    bool valid = false;
    int type = -1;
    uint64_t config = 0;
    std::string pmu_name;
    std::string event_name;
};

//funzione di utilità per leggere il file sysf
static std::string read_text_file(const std::string& path)
{
    std::ifstream f(path.c_str());
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}


//funzione che pulisce una stringa da spazi, \n, \r, tab iniziali/finali
static std::string trim_copy(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();

    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
        start++;

    return s.substr(start);
}

//converte una stringa in un numero
static bool parse_uint64_auto(const std::string& s, uint64_t& out)
{
    char* end = nullptr;
    errno = 0;
    unsigned long long v = strtoull(s.c_str(), &end, 0);

    if (errno != 0 || end == s.c_str())
        return false;

    out = static_cast<uint64_t>(v);
    return true;
}

/*
    Cerca un evento PMU in:
        /sys/bus/event_source/devices/<pmu>/events/<event_name>

    Esempio tipico ARM:
        /sys/bus/event_source/devices/armv8_pmuv3_0/events/l2d_cache
        contenuto: event=0x16

    Il type della PMU viene letto da:
        /sys/bus/event_source/devices/<pmu>/type


            1. apro /sys/bus/event_source/devices
            2. scorro tutte le PMU disponibili
            3. per ogni PMU controllo se esiste events/l2d_cache
            4. se esiste, leggo:
               - il type della PMU
               - il config dell’evento
            5. salvo tutto in PmuEventDesc
*/
static bool find_pmu_event_by_name(const std::string& event_name, PmuEventDesc& out)
{
    const std::string base = "/sys/bus/event_source/devices";

    DIR* dir = opendir(base.c_str());
    if (!dir)
    {
        perror("opendir /sys/bus/event_source/devices");
        return false;
    }

    struct dirent* de = nullptr;

    while ((de = readdir(dir)) != nullptr)
    {
        std::string pmu = de->d_name;

        if (pmu == "." || pmu == "..")
            continue;

        std::string event_path = base + "/" + pmu + "/events/" + event_name;
        std::ifstream evf(event_path.c_str());

        if (!evf.good())
            continue;

        std::string type_path = base + "/" + pmu + "/type";
        std::string type_text = trim_copy(read_text_file(type_path));

        uint64_t type_value = 0;
        if (!parse_uint64_auto(type_text, type_value))
            continue;

        std::string event_text = trim_copy(read_text_file(event_path));

        /*
            Gestiamo casi semplici tipo:
                event=0x16
                config=0x16
            Su molte PMU ARM è proprio event=0xNN.
        */
        uint64_t config = 0;
        bool found_config = false;

        std::stringstream ss(event_text);
        std::string token;

        while (std::getline(ss, token, ','))
        {
            token = trim_copy(token);

            size_t eq = token.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = token.substr(0, eq);
            std::string val = token.substr(eq + 1);

            key = trim_copy(key);
            val = trim_copy(val);

            if (key == "event" || key == "config")
            {
                if (parse_uint64_auto(val, config))
                {
                    found_config = true;
                    break;
                }
            }
        }

        if (!found_config)
            continue;

        out.valid = true;
        out.type = static_cast<int>(type_value);
        out.config = config;
        out.pmu_name = pmu;
        out.event_name = event_name;

        closedir(dir);
        return true;
    }

    closedir(dir);
    return false;
}


// questa struttura rappresenta i contatori aperti su una singola cpu
// questa struttura rappresenta i contatori aperti su una singola cpu
// questa struttura rappresenta i contatori aperti su una singola CPU
// questa struttura rappresenta i contatori aperti su una singola CPU
struct PerfCpuGroup
{
    int cpu = -1;

    int fd_leader_l2_cache = -1;
    int fd_l2_refill = -1;

    int fd_bus_access = -1;
    int fd_bus_cycles = -1;

    int fd_cpu_cycles = -1;
    int fd_inst_retired = -1;
};


// Valori letti da perf per una singola CPU in una singola iterazione
struct PerfCpuIterationStats
{
    int cpu = -1;

    uint64_t l2d_cache = 0;
    uint64_t l2d_cache_refill = 0;
    double l2_miss_percent = 0.0;

    uint64_t bus_access = 0;
    uint64_t bus_cycles = 0;

    double bus_access_per_cycle = 0.0;
    double bus_cycles_per_access = 0.0;

    uint64_t cpu_cycles = 0;

    uint64_t inst_retired = 0;
    double ipc = 0.0;
    double cpi = 0.0;
};


// Questa struct rappresenta i risultati di una iterazione.
// Contiene sia i totali su tutte le CPU, sia il dettaglio per CPU.
struct PerfIterationStats
{
    bool valid = false;
    int iter = -1;

    double sleep_delay_us = 0.0;

    // Totali aggregati su tutte le CPU
    uint64_t l2d_cache = 0;
    uint64_t l2d_cache_refill = 0;
    double l2_miss_percent = 0.0;

    uint64_t bus_access = 0;
    uint64_t bus_cycles = 0;
    double bus_access_per_cycle = 0.0;
    double bus_cycles_per_access = 0.0;

    uint64_t cpu_cycles = 0;

    uint64_t inst_retired = 0;
    double ipc = 0.0;
    double cpi = 0.0;

    // Dettaglio per CPU
    std::vector<PerfCpuIterationStats> per_cpu;
};


// Oggetto fondamentale che permette di inizializzare i contatori,
// resettarli, abilitarli, disabilitarli, leggerli e chiuderli.
class PerfL2Monitor
{
public:
    bool init(const std::vector<int>& cpus)
    {
        PmuEventDesc ev_l2_cache;
        PmuEventDesc ev_l2_refill;
        PmuEventDesc ev_bus_access;
        PmuEventDesc ev_bus_cycles;
        PmuEventDesc ev_cpu_cycles;
        PmuEventDesc ev_inst_retired;

        if (!find_pmu_event_by_name("l2d_cache", ev_l2_cache))
        {
            std::cerr << "Errore: evento PMU 'l2d_cache' non trovato in sysfs.\n";
            std::cerr << "Verifica con: perf list | grep -i l2\n";
            return false;
        }

        if (!find_pmu_event_by_name("l2d_cache_refill", ev_l2_refill))
        {
            std::cerr << "Errore: evento PMU 'l2d_cache_refill' non trovato in sysfs.\n";
            std::cerr << "Verifica con: perf list | grep -i l2\n";
            return false;
        }

        if (!find_pmu_event_by_name("bus_access", ev_bus_access))
        {
            std::cerr << "Errore: evento PMU 'bus_access' non trovato in sysfs.\n";
            std::cerr << "Verifica con: perf list | grep -i bus\n";
            return false;
        }

        if (!find_pmu_event_by_name("bus_cycles", ev_bus_cycles))
        {
            std::cerr << "Errore: evento PMU 'bus_cycles' non trovato in sysfs.\n";
            std::cerr << "Verifica con: perf list | grep -i bus\n";
            return false;
        }

        if (!find_pmu_event_by_name("cpu_cycles", ev_cpu_cycles))
        {
            std::cerr << "Errore: evento PMU 'cpu_cycles' non trovato in sysfs.\n";
            std::cerr << "Verifica con: perf list | grep -i cpu_cycles\n";
            return false;
        }
        if (!find_pmu_event_by_name("inst_retired", ev_inst_retired))
        {
            std::cerr << "Errore: evento PMU 'inst_retired' non trovato in sysfs.\n";
            std::cerr << "Verifica con: perf list | grep -i inst\n";
            return false;
        }
        /*
            Per semplicità apriamo tutti gli eventi nello stesso gruppo.
            Quindi devono appartenere alla stessa PMU.
        */
        if (ev_l2_cache.type != ev_l2_refill.type ||
            ev_l2_cache.type != ev_bus_access.type ||
            ev_l2_cache.type != ev_bus_cycles.type ||
            ev_l2_cache.type != ev_cpu_cycles.type ||
            ev_l2_cache.type != ev_inst_retired.type)
        {
            std::cerr << "Errore: gli eventi scelti appartengono a PMU diverse.\n";
            std::cerr << "l2d_cache type=" << ev_l2_cache.type << "\n";
            std::cerr << "l2d_cache_refill type=" << ev_l2_refill.type << "\n";
            std::cerr << "bus_access type=" << ev_bus_access.type << "\n";
            std::cerr << "bus_cycles type=" << ev_bus_cycles.type << "\n";
            std::cerr << "cpu_cycles type=" << ev_cpu_cycles.type << "\n";
            std::cerr << "cpu_cycles type=" << ev_inst_retired.type << "\n";
            return false;
        }

        std::cout << "Perf PMU trovata: " << ev_l2_cache.pmu_name << "\n";

        std::cout << "l2d_cache config=0x"
                  << std::hex << ev_l2_cache.config << std::dec << "\n";

        std::cout << "l2d_cache_refill config=0x"
                  << std::hex << ev_l2_refill.config << std::dec << "\n";

        std::cout << "bus_access config=0x"
                  << std::hex << ev_bus_access.config << std::dec << "\n";

        std::cout << "bus_cycles config=0x"
                  << std::hex << ev_bus_cycles.config << std::dec << "\n";

        std::cout << "cpu_cycles config=0x"
                  << std::hex << ev_cpu_cycles.config << std::dec << "\n";

        std::cout << "inst_retired config=0x"
                  << std::hex << ev_inst_retired.config << std::dec << "\n";

        for (int cpu : cpus)
        {
            PerfCpuGroup g;
            g.cpu = cpu;

            /*
                Gruppo per CPU:
                    leader  = l2d_cache
                    sibling = l2d_cache_refill
                    sibling = bus_access
                    sibling = bus_cycles
                    sibling = cpu_cycles

                Tutti questi contatori vengono:
                    - resettati insieme
                    - abilitati insieme
                    - disabilitati insieme
                    - letti insieme

                Così appartengono alla stessa finestra temporale.
            */

            struct perf_event_attr pe;
            memset(&pe, 0, sizeof(pe));

            pe.type = ev_l2_cache.type;
            pe.size = sizeof(pe);
            pe.config = ev_l2_cache.config;
            pe.disabled = 1;
            pe.exclude_user = 0;
            pe.exclude_kernel = 0;
            pe.exclude_hv = 0;
            pe.read_format = PERF_FORMAT_GROUP;

            g.fd_leader_l2_cache = perf_event_open_syscall(&pe, -1, cpu, -1, 0);

            if (g.fd_leader_l2_cache == -1)
            {
                std::cerr << "Errore apertura l2d_cache su CPU" << cpu << ": ";
                perror("perf_event_open");
                close_all();
                return false;
            }

            // Secondo evento del gruppo: l2d_cache_refill
            memset(&pe, 0, sizeof(pe));

            pe.type = ev_l2_refill.type;
            pe.size = sizeof(pe);
            pe.config = ev_l2_refill.config;
            pe.disabled = 0;
            pe.exclude_user = 0;
            pe.exclude_kernel = 0;
            pe.exclude_hv = 0;
            pe.read_format = PERF_FORMAT_GROUP;

            g.fd_l2_refill = perf_event_open_syscall(&pe, -1, cpu, g.fd_leader_l2_cache, 0);

            if (g.fd_l2_refill == -1)
            {
                std::cerr << "Errore apertura l2d_cache_refill su CPU" << cpu << ": ";
                perror("perf_event_open");
                close_all();
                return false;
            }

            // Terzo evento del gruppo: bus_access
            memset(&pe, 0, sizeof(pe));

            pe.type = ev_bus_access.type;
            pe.size = sizeof(pe);
            pe.config = ev_bus_access.config;
            pe.disabled = 0;
            pe.exclude_user = 0;
            pe.exclude_kernel = 0;
            pe.exclude_hv = 0;
            pe.read_format = PERF_FORMAT_GROUP;

            g.fd_bus_access = perf_event_open_syscall(&pe, -1, cpu, g.fd_leader_l2_cache, 0);

            if (g.fd_bus_access == -1)
            {
                std::cerr << "Errore apertura bus_access su CPU" << cpu << ": ";
                perror("perf_event_open");
                close_all();
                return false;
            }

            // Quarto evento del gruppo: bus_cycles
            memset(&pe, 0, sizeof(pe));

            pe.type = ev_bus_cycles.type;
            pe.size = sizeof(pe);
            pe.config = ev_bus_cycles.config;
            pe.disabled = 0;
            pe.exclude_user = 0;
            pe.exclude_kernel = 0;
            pe.exclude_hv = 0;
            pe.read_format = PERF_FORMAT_GROUP;

            g.fd_bus_cycles = perf_event_open_syscall(&pe, -1, cpu, g.fd_leader_l2_cache, 0);

            if (g.fd_bus_cycles == -1)
            {
                std::cerr << "Errore apertura bus_cycles su CPU" << cpu << ": ";
                perror("perf_event_open");
                close_all();
                return false;
            }

            // Quinto evento del gruppo: cpu_cycles
            memset(&pe, 0, sizeof(pe));

            pe.type = ev_cpu_cycles.type;
            pe.size = sizeof(pe);
            pe.config = ev_cpu_cycles.config;
            pe.disabled = 0;
            pe.exclude_user = 0;
            pe.exclude_kernel = 0;
            pe.exclude_hv = 0;
            pe.read_format = PERF_FORMAT_GROUP;

            g.fd_cpu_cycles = perf_event_open_syscall(&pe, -1, cpu, g.fd_leader_l2_cache, 0);

            if (g.fd_cpu_cycles == -1)
            {
                std::cerr << "Errore apertura cpu_cycles su CPU" << cpu << ": ";
                perror("perf_event_open");
                close_all();
                return false;
            }

            // Sesto evento del gruppo: inst_retired
            memset(&pe, 0, sizeof(pe));

            pe.type = ev_inst_retired.type;
            pe.size = sizeof(pe);
            pe.config = ev_inst_retired.config;
            pe.disabled = 0;
            pe.exclude_user = 0;
            pe.exclude_kernel = 0;
            pe.exclude_hv = 0;
            pe.read_format = PERF_FORMAT_GROUP;

            g.fd_inst_retired = perf_event_open_syscall(&pe, -1, cpu, g.fd_leader_l2_cache, 0);

            if (g.fd_inst_retired == -1)
            {
                std::cerr << "Errore apertura inst_retired su CPU" << cpu << ": ";
                perror("perf_event_open");
                close_all();
                return false;
            }

            ioctl(g.fd_leader_l2_cache, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
            ioctl(g.fd_leader_l2_cache, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);

            groups.push_back(g);
        }

        initialized = true;
        return true;
    }

    void start()
    {
        if (!initialized)
            return;

        for (auto& g : groups)
        {
            ioctl(g.fd_leader_l2_cache, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
            ioctl(g.fd_leader_l2_cache, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
        }
    }

    /*
        Legge i contatori aggregati e anche il dettaglio per CPU.
    */
    bool stop_and_read(uint64_t& total_l2d_cache,
                       uint64_t& total_l2d_cache_refill,
                       uint64_t& total_bus_access,
                       uint64_t& total_bus_cycles,
                       uint64_t& total_cpu_cycles,
                       uint64_t& total_inst_retired,
                       std::vector<PerfCpuIterationStats>& per_cpu_stats)
    {
        total_l2d_cache = 0;
        total_l2d_cache_refill = 0;
        total_bus_access = 0;
        total_bus_cycles = 0;
        total_cpu_cycles = 0;
        total_inst_retired = 0;
        per_cpu_stats.clear();

        if (!initialized)
            return false;

        // Prima fermo tutti i gruppi, così congelo la finestra di misura.
        for (auto& g : groups)
        {
            ioctl(g.fd_leader_l2_cache, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        }

        // Poi leggo i valori CPU per CPU.
        for (auto& g : groups)
        {
            struct ReadGroup
            {
                uint64_t nr;
                uint64_t values[6];
            };

            ReadGroup rg;
            memset(&rg, 0, sizeof(rg));

            ssize_t ret = read(g.fd_leader_l2_cache, &rg, sizeof(rg));

            if (ret < 0)
            {
                perror("read perf group");
                return false;
            }

            if (rg.nr < 6)
            {
                std::cerr << "Errore: gruppo perf su CPU" << g.cpu
                          << " ha meno di 5 valori. nr=" << rg.nr << "\n";
                return false;
            }

            /*
                Ordine del gruppo:
                    values[0] = leader  l2d_cache
                    values[1] = sibling l2d_cache_refill
                    values[2] = sibling bus_access
                    values[3] = sibling bus_cycles
                    values[4] = sibling cpu_cycles
            */
            uint64_t cpu_l2d_cache = rg.values[0];
            uint64_t cpu_l2d_cache_refill = rg.values[1];
            uint64_t cpu_bus_access = rg.values[2];
            uint64_t cpu_bus_cycles = rg.values[3];
            uint64_t cpu_cpu_cycles = rg.values[4];
            uint64_t cpu_inst_retired = rg.values[5];

            double cpu_l2_miss_percent = 0.0;
            if (cpu_l2d_cache > 0)
            {
                cpu_l2_miss_percent =
                    ((double)cpu_l2d_cache_refill / (double)cpu_l2d_cache) * 100.0;
            }

            double cpu_bus_access_per_cycle = 0.0;
            if (cpu_bus_cycles > 0)
            {
                cpu_bus_access_per_cycle =
                    (double)cpu_bus_access / (double)cpu_bus_cycles;
            }

            double cpu_bus_cycles_per_access = 0.0;
            if (cpu_bus_access > 0)
            {
                cpu_bus_cycles_per_access =
                    (double)cpu_bus_cycles / (double)cpu_bus_access;
            }

            double cpu_ipc = 0.0;
            if (cpu_cpu_cycles > 0)
            {
                cpu_ipc = (double)cpu_inst_retired / (double)cpu_cpu_cycles;
            }

            double cpu_cpi = 0.0;
            if (cpu_inst_retired > 0)
            {
                cpu_cpi = (double)cpu_cpu_cycles / (double)cpu_inst_retired;
            }

            PerfCpuIterationStats cpu_stats;
            cpu_stats.cpu = g.cpu;

            cpu_stats.l2d_cache = cpu_l2d_cache;
            cpu_stats.l2d_cache_refill = cpu_l2d_cache_refill;
            cpu_stats.l2_miss_percent = cpu_l2_miss_percent;

            cpu_stats.bus_access = cpu_bus_access;
            cpu_stats.bus_cycles = cpu_bus_cycles;
            cpu_stats.bus_access_per_cycle = cpu_bus_access_per_cycle;
            cpu_stats.bus_cycles_per_access = cpu_bus_cycles_per_access;

            cpu_stats.cpu_cycles = cpu_cpu_cycles;
            cpu_stats.inst_retired = cpu_inst_retired;
            cpu_stats.ipc = cpu_ipc;
            cpu_stats.cpi = cpu_cpi;

            per_cpu_stats.push_back(cpu_stats);

            total_l2d_cache += cpu_l2d_cache;
            total_l2d_cache_refill += cpu_l2d_cache_refill;
            total_bus_access += cpu_bus_access;
            total_bus_cycles += cpu_bus_cycles;
            total_cpu_cycles += cpu_cpu_cycles;
            total_inst_retired += cpu_inst_retired;
        }

        return true;
    }

    void close_all()
    {
        for (auto& g : groups)
        {
            if (g.fd_cpu_cycles >= 0)
            {
                close(g.fd_cpu_cycles);
                g.fd_cpu_cycles = -1;
            }

            if (g.fd_bus_cycles >= 0)
            {
                close(g.fd_bus_cycles);
                g.fd_bus_cycles = -1;
            }

            if (g.fd_bus_access >= 0)
            {
                close(g.fd_bus_access);
                g.fd_bus_access = -1;
            }

            if (g.fd_l2_refill >= 0)
            {
                close(g.fd_l2_refill);
                g.fd_l2_refill = -1;
            }

            if (g.fd_leader_l2_cache >= 0)
            {
                close(g.fd_leader_l2_cache);
                g.fd_leader_l2_cache = -1;
            }
        }

        groups.clear();
        initialized = false;
    }

    ~PerfL2Monitor()
    {
        close_all();
    }

private:
    bool initialized = false;
    std::vector<PerfCpuGroup> groups;
};


static PerfL2Monitor g_perf_l2;
static std::vector<PerfIterationStats> g_perf_stats;


static const PerfCpuIterationStats* FindCpuStatsInIteration(const PerfIterationStats& iter_stats,
                                                            int cpu)
{
    for (const auto& c : iter_stats.per_cpu)
    {
        if (c.cpu == cpu)
            return &c;
    }

    return nullptr;
}


static void SavePerfStatsForIteration(int iter, double sleep_delay_us)
{
    if (iter < 0 || iter >= (int)g_perf_stats.size())
        return;

    uint64_t total_l2d_cache = 0;
    uint64_t total_l2d_cache_refill = 0;
    uint64_t total_bus_access = 0;
    uint64_t total_bus_cycles = 0;
    uint64_t total_cpu_cycles = 0;
    uint64_t total_inst_retired = 0;

    std::vector<PerfCpuIterationStats> per_cpu_stats;

    if (!g_perf_l2.stop_and_read(total_l2d_cache,
                                 total_l2d_cache_refill,
                                 total_bus_access,
                                 total_bus_cycles,
                                 total_cpu_cycles,
                                 total_inst_retired,
                                 per_cpu_stats))
    {
        std::cerr << "Errore lettura perf all'iterazione " << iter << "\n";
        return;
    }

    double miss_percent = 0.0;
    if (total_l2d_cache > 0)
    {
        miss_percent =
            ((double)total_l2d_cache_refill / (double)total_l2d_cache) * 100.0;
    }

    double bus_access_per_cycle = 0.0;
    if (total_bus_cycles > 0)
    {
        bus_access_per_cycle =
            (double)total_bus_access / (double)total_bus_cycles;
    }

    double bus_cycles_per_access = 0.0;
    if (total_bus_access > 0)
    {
        bus_cycles_per_access =
            (double)total_bus_cycles / (double)total_bus_access;
    }

    double ipc = 0.0;
    if (total_cpu_cycles > 0)
    {
        ipc = (double)total_inst_retired / (double)total_cpu_cycles;
    }

    double cpi = 0.0;
    if (total_inst_retired > 0)
    {
        cpi = (double)total_cpu_cycles / (double)total_inst_retired;
    }
    g_perf_stats[iter].valid = true;
    g_perf_stats[iter].iter = iter;
    g_perf_stats[iter].sleep_delay_us = sleep_delay_us;

    // Totali globali su tutte le CPU
    g_perf_stats[iter].l2d_cache = total_l2d_cache;
    g_perf_stats[iter].l2d_cache_refill = total_l2d_cache_refill;
    g_perf_stats[iter].l2_miss_percent = miss_percent;

    g_perf_stats[iter].bus_access = total_bus_access;
    g_perf_stats[iter].bus_cycles = total_bus_cycles;
    g_perf_stats[iter].bus_access_per_cycle = bus_access_per_cycle;
    g_perf_stats[iter].bus_cycles_per_access = bus_cycles_per_access;

    g_perf_stats[iter].cpu_cycles = total_cpu_cycles;
    g_perf_stats[iter].inst_retired = total_inst_retired;
    g_perf_stats[iter].ipc = ipc;
    g_perf_stats[iter].cpi = cpi;

    // Dettaglio per CPU
    g_perf_stats[iter].per_cpu = per_cpu_stats;
}


static std::string fmt_double(double value, int precision = 4)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}


static void print_separator_line()
{
    std::cout << "============================================================\n";
}


static void print_subseparator_line()
{
    std::cout << "------------------------------------------------------------\n";
}


static void print_cpu_iteration_block(const std::string& title,
                                      double sleep_delay_us,
                                      int iter,
                                      const PerfCpuIterationStats* s)
{
    std::cout << title << "\n";

    if (!s)
    {
        std::cout << "  Dati non disponibili\n";
        return;
    }

    std::cout << "  Iterazione               : " << iter << "\n";
    std::cout << "  Ritardo sleep            : " << fmt_double(sleep_delay_us, 0) << " us\n";



    std::cout << "  l2d_cache                : " << s->l2d_cache << "\n";
    std::cout << "  l2d_cache_refill         : " << s->l2d_cache_refill << "\n";
    std::cout << "  L2 cache miss            : " << fmt_double(s->l2_miss_percent, 4) << " %\n";

    std::cout << "  bus_access               : " << s->bus_access << "\n";
    std::cout << "  bus_cycles               : " << s->bus_cycles << "\n";
    std::cout << "  bus_access/bus_cycles    : " << fmt_double(s->bus_access_per_cycle, 6) << "\n";
    std::cout << "  bus_cycles/bus_access    : " << fmt_double(s->bus_cycles_per_access, 4) << "\n";

    std::cout << "  cpu_cycles               : " << s->cpu_cycles << "\n";
    std::cout << "  numero_istruzioni        : " << s->inst_retired << "\n";
    std::cout << "  IPC                      : " << fmt_double(s->ipc, 6) << "\n";
    std::cout << "  CPI                      : " << fmt_double(s->cpi, 6) << "\n";
}


static void print_total_iteration_block(const std::string& title,
                                        const PerfIterationStats& s)
{
    std::cout << title << "\n";
    std::cout << "  Iterazione               : " << s.iter << "\n";
    std::cout << "  Ritardo sleep            : " << fmt_double(s.sleep_delay_us, 0) << " us\n";


    std::cout << "  l2d_cache totale         : " << s.l2d_cache << "\n";
    std::cout << "  l2d_cache_refill totale  : " << s.l2d_cache_refill << "\n";
    std::cout << "  L2 cache miss totale     : " << fmt_double(s.l2_miss_percent, 4) << " %\n";

    std::cout << "  bus_access totale        : " << s.bus_access << "\n";
    std::cout << "  bus_cycles totale        : " << s.bus_cycles << "\n";
    std::cout << "  bus_access/bus_cycles    : " << fmt_double(s.bus_access_per_cycle, 6) << "\n";
    std::cout << "  bus_cycles/bus_access    : " << fmt_double(s.bus_cycles_per_access, 4) << "\n";

    std::cout << "  cpu_cycles totale        : " << s.cpu_cycles << "\n";
    std::cout << "  numero_istruzioni totale      : " << s.inst_retired << "\n";
    std::cout << "  IPC totale               : " << fmt_double(s.ipc, 6) << "\n";
    std::cout << "  CPI totale               : " << fmt_double(s.cpi, 6) << "\n";
}


static void PrintFinalPerfStats()
{
    int min_iter = -1;
    int max_iter = -1;

    double min_sleep = 999999999.0;
    double max_sleep = -1.0;

    double sum_miss_percent = 0.0;
    double sum_bus_access_per_cycle = 0.0;
    double sum_bus_cycles_per_access = 0.0;
    double sum_cpu_cycles = 0.0;
    double sum_inst_retired = 0.0;
    double sum_ipc = 0.0;
    double sum_cpi = 0.0;

    int valid_count = 0;

    // Trovo iterazione con ritardo minimo, massimo e medie globali.
    for (const auto& s : g_perf_stats)
    {
        if (!s.valid)
            continue;

        if (s.sleep_delay_us < min_sleep)
        {
            min_sleep = s.sleep_delay_us;
            min_iter = s.iter;
        }

        if (s.sleep_delay_us > max_sleep)
        {
            max_sleep = s.sleep_delay_us;
            max_iter = s.iter;
        }

        sum_miss_percent += s.l2_miss_percent;
        sum_bus_access_per_cycle += s.bus_access_per_cycle;
        sum_bus_cycles_per_access += s.bus_cycles_per_access;
        sum_cpu_cycles += (double)s.cpu_cycles;
        sum_inst_retired += (double)s.inst_retired;
        sum_ipc += s.ipc;
        sum_cpi += s.cpi;

        valid_count++;
    }

    if (valid_count == 0)
    {
        std::cout << "\nNessuna iterazione valida per statistiche perf.\n";
        return;
    }

    double avg_miss_percent = sum_miss_percent / valid_count;
    double avg_bus_access_per_cycle = sum_bus_access_per_cycle / valid_count;
    double avg_bus_cycles_per_access = sum_bus_cycles_per_access / valid_count;
    double avg_cpu_cycles = sum_cpu_cycles / valid_count;
    double avg_inst_retired = sum_inst_retired / valid_count;
    double avg_ipc = sum_ipc / valid_count;
    double avg_cpi = sum_cpi / valid_count;

    const auto& min_s = g_perf_stats[min_iter];
    const auto& max_s = g_perf_stats[max_iter];

    print_separator_line();
    std::cout << "STATISTICHE FINALI PERF TOTALI\n";
    print_separator_line();

    std::cout << "Iterazioni valide          : " << valid_count << "\n";
    std::cout << "Media cpu_cycles totale    : " << fmt_double(avg_cpu_cycles, 0) << "\n";
    std::cout << "Media inst_retired totale  : " << fmt_double(avg_inst_retired, 0) << "\n";
    std::cout << "Media IPC totale           : " << fmt_double(avg_ipc, 6) << "\n";
    std::cout << "Media CPI totale           : " << fmt_double(avg_cpi, 6) << "\n";
    std::cout << "Media cache miss totale    : " << fmt_double(avg_miss_percent, 4) << " %\n";
    std::cout << "Media bus_access/bus_cycles: " << fmt_double(avg_bus_access_per_cycle, 6) << "\n";
    std::cout << "Media bus_cycles/bus_access: " << fmt_double(avg_bus_cycles_per_access, 4) << "\n";
    std::cout << "\n";

    print_total_iteration_block("[MIN SLEEP]", min_s);
    std::cout << "\n";
    print_total_iteration_block("[MAX SLEEP]", max_s);
    std::cout << "\n";

    print_separator_line();
    std::cout << "STATISTICHE FINALI PERF PER CPU\n";
    print_separator_line();

    std::vector<int> cpus_found;

    for (const auto& s : g_perf_stats)
    {
        if (!s.valid)
            continue;

        for (const auto& c : s.per_cpu)
        {
            bool already_present = false;

            for (int existing_cpu : cpus_found)
            {
                if (existing_cpu == c.cpu)
                {
                    already_present = true;
                    break;
                }
            }

            if (!already_present)
                cpus_found.push_back(c.cpu);
        }
    }

    std::sort(cpus_found.begin(), cpus_found.end());

    for (int cpu : cpus_found)
    {
        double sum_cpu_miss_percent = 0.0;
        double sum_cpu_bus_access_per_cycle = 0.0;
        double sum_cpu_bus_cycles_per_access = 0.0;
        double sum_cpu_cpu_cycles = 0.0;
        double sum_cpu_inst_retired = 0.0;
        double sum_cpu_ipc = 0.0;
        double sum_cpu_cpi = 0.0;

        int cpu_valid_count = 0;

        for (const auto& s : g_perf_stats)
        {
            if (!s.valid)
                continue;

            const PerfCpuIterationStats* cpu_s = FindCpuStatsInIteration(s, cpu);
            if (!cpu_s)
                continue;

            sum_cpu_miss_percent += cpu_s->l2_miss_percent;
            sum_cpu_bus_access_per_cycle += cpu_s->bus_access_per_cycle;
            sum_cpu_bus_cycles_per_access += cpu_s->bus_cycles_per_access;
            sum_cpu_cpu_cycles += (double)cpu_s->cpu_cycles;
            sum_cpu_inst_retired += (double)cpu_s->inst_retired;
            sum_cpu_ipc += cpu_s->ipc;
            sum_cpu_cpi += cpu_s->cpi;

            cpu_valid_count++;
        }

        if (cpu_valid_count == 0)
            continue;

        double avg_cpu_miss_percent = sum_cpu_miss_percent / cpu_valid_count;
        double avg_cpu_bus_access_per_cycle = sum_cpu_bus_access_per_cycle / cpu_valid_count;
        double avg_cpu_bus_cycles_per_access = sum_cpu_bus_cycles_per_access / cpu_valid_count;
        double avg_cpu_cpu_cycles = sum_cpu_cpu_cycles / cpu_valid_count;
        double avg_cpu_inst_retired = sum_cpu_inst_retired / cpu_valid_count;
        double avg_cpu_ipc = sum_cpu_ipc / cpu_valid_count;
        double avg_cpu_cpi = sum_cpu_cpi / cpu_valid_count;

        const PerfCpuIterationStats* min_cpu_s = FindCpuStatsInIteration(min_s, cpu);
        const PerfCpuIterationStats* max_cpu_s = FindCpuStatsInIteration(max_s, cpu);

        std::cout << "\n";
        print_subseparator_line();
        std::cout << "CPU" << cpu << "\n";
        print_subseparator_line();

        std::cout << "Iterazioni valide          : " << cpu_valid_count << "\n";
        std::cout << "Media cpu_cycles           : " << fmt_double(avg_cpu_cpu_cycles, 0) << "\n";
        std::cout << "Media inst_retired         : " << fmt_double(avg_cpu_inst_retired, 0) << "\n";
        std::cout << "Media IPC                  : " << fmt_double(avg_cpu_ipc, 6) << "\n";
        std::cout << "Media CPI                  : " << fmt_double(avg_cpu_cpi, 6) << "\n";
        std::cout << "Media cache miss           : " << fmt_double(avg_cpu_miss_percent, 4) << " %\n";
        std::cout << "Media bus_access/bus_cycles: " << fmt_double(avg_cpu_bus_access_per_cycle, 6) << "\n";
        std::cout << "Media bus_cycles/bus_access: " << fmt_double(avg_cpu_bus_cycles_per_access, 4) << "\n";
        std::cout << "\n";

        print_cpu_iteration_block("[MIN SLEEP]", min_s.sleep_delay_us, min_s.iter, min_cpu_s);
        std::cout << "\n";
        print_cpu_iteration_block("[MAX SLEEP]", max_s.sleep_delay_us, max_s.iter, max_cpu_s);
    }

    std::cout << "\n";
    print_separator_line();
}
#endif
```
