/* ===========================================================================
 * stress_mem.c
 *
 * Generatore di interferenza di memoria CONTROLLATA, per verificare se il
 * jitter del thread real-time su CPU3 sia causato dalla cache L2 condivisa
 * del cluster Cortex-A53 (i.MX8M Plus).
 *
 * Il punto: sull'i.MX8MP i 4 core A53 stanno in un unico cluster e
 * CONDIVIDONO la L2. "isolcpus=3" isola lo scheduler, NON la cache.
 * Questo programma genera traffico di memoria su CPU0-2 SENZA alcuna GUI,
 * cosi' da poter attribuire il jitter alla sola interferenza di memoria.
 *
 * Tre modalita', progettate per essere confrontate fra loro:
 *
 *   l2fit   buffer piccolo (64 KB): la memcpy gira INTERAMENTE dentro la
 *           cache. Tanto lavoro, tanta banda verso la L1/L2, ma quasi
 *           nessun accesso alla DRAM e quasi nessuno sfratto delle linee
 *           altrui.  --> CONTROLLO NEGATIVO: non deve produrre jitter.
 *
 *   stream  buffer grande (8 MB): memcpy che sfonda la L2. Massima banda
 *           verso la DRAM E massimo sfratto.
 *           --> TRATTAMENTO: deve riprodurre il jitter osservato con la GUI.
 *
 *   thrash  buffer grande percorso a passo di 64 byte (una lettura per
 *           linea di cache) con una pausa fra una passata e l'altra.
 *           Sfratta l'intera L2 ripetutamente ma con banda BASSA.
 *           --> DISCRIMINANTE: se questo produce jitter, la causa e' lo
 *               SFRATTO DELLA CACHE e non la saturazione della banda.
 *
 * Matrice di interpretazione:
 *
 *   l2fit  stream  thrash | conclusione
 *   -------------------------------------------------------------------
 *    no      si      si   | sfratto della L2 condivisa  <-- ipotesi attesa
 *    no      si      no   | saturazione della banda DDR
 *    si      si      si   | non e' la memoria: e' il carico di CPU in se'
 *    no      no      no   | ipotesi da rifare: la GUI agisce per altra via
 *
 * ---------------------------------------------------------------------------
 * Compilazione (sulla board):
 *     gcc -O2 -o stress_mem stress_mem.c -lpthread
 *
 * Compilazione (cross, SDK Yocto):
 *     source /opt/.../environment-setup-aarch64-poky-linux
 *     $CC -O2 -o stress_mem stress_mem.c -lpthread
 *
 * Uso:
 *     ./stress_mem [stream|l2fit|thrash] [MB] [nthread] [pausa_us]
 *
 * Esempi:
 *     ./stress_mem l2fit                 # controllo negativo
 *     ./stress_mem stream                # trattamento (8 MB, 3 thread)
 *     ./stress_mem thrash                # discriminante (pausa 200 us)
 *     ./stress_mem thrash 8 3 1000       # discriminante a banda ancora piu' bassa
 *
 * Terminare con Ctrl-C o SIGTERM: stampa le statistiche di banda.
 * ===========================================================================
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS     4
#define CACHE_LINE      64

typedef enum {
    MODE_STREAM = 0,
    MODE_L2FIT,
    MODE_THRASH
} stress_mode_t;

static volatile sig_atomic_t g_running = 1;

static stress_mode_t g_mode      = MODE_STREAM;
static size_t        g_bufsz     = 8u << 20;   /* byte */
static int           g_nthreads  = 3;          /* CPU0..CPU2 */
static int           g_pause_us  = 0;

/* volatile: impedisce al compilatore di eliminare il ciclo di lettura */
static volatile uint64_t g_sink  = 0;

static uint64_t g_bytes[MAX_THREADS];

/* --------------------------------------------------------------------- */

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static const char *mode_name(stress_mode_t m)
{
    switch (m) {
        case MODE_L2FIT:  return "l2fit";
        case MODE_THRASH: return "thrash";
        default:          return "stream";
    }
}

static int pin_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        fprintf(stderr, "  [!] affinita' su CPU%d fallita: %s\n",
                cpu, strerror(errno));
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------- */

static void *worker(void *arg)
{
    const int cpu = (int)(intptr_t)arg;
    char *a = NULL;
    char *b = NULL;
    uint64_t bytes = 0;

    pin_to_cpu(cpu);

    if (posix_memalign((void **)&a, CACHE_LINE, g_bufsz) != 0 ||
        posix_memalign((void **)&b, CACHE_LINE, g_bufsz) != 0) {
        fprintf(stderr, "  [!] allocazione fallita su CPU%d\n", cpu);
        free(a);
        return NULL;
    }

    /* Tocca tutte le pagine subito: niente page fault dentro la misura. */
    memset(a, 0xA5, g_bufsz);
    memset(b, 0x5A, g_bufsz);

    while (g_running) {
        if (g_mode == MODE_THRASH) {
            /* Una lettura per linea di cache: sfratta tutto, muove poco. */
            uint64_t sum = 0;
            size_t i;
            for (i = 0; i < g_bufsz; i += CACHE_LINE)
                sum += (uint64_t)(unsigned char)b[i];
            g_sink += sum;
            bytes += (uint64_t)g_bufsz;          /* linee toccate x 64 B */

            if (g_pause_us > 0)
                usleep((useconds_t)g_pause_us);
        } else {
            memcpy(a, b, g_bufsz);
            bytes += 2ull * (uint64_t)g_bufsz;   /* letti + scritti */
        }
    }

    if (cpu >= 0 && cpu < MAX_THREADS)
        g_bytes[cpu] = bytes;

    free(a);
    free(b);
    return NULL;
}

/* --------------------------------------------------------------------- */

static void parse_args(int argc, char **argv)
{
    if (argc > 1) {
        if      (strcmp(argv[1], "l2fit")  == 0) g_mode = MODE_L2FIT;
        else if (strcmp(argv[1], "thrash") == 0) g_mode = MODE_THRASH;
        else if (strcmp(argv[1], "stream") == 0) g_mode = MODE_STREAM;
        else {
            fprintf(stderr, "modalita' sconosciuta: %s\n", argv[1]);
            fprintf(stderr, "uso: %s [stream|l2fit|thrash] [MB] [nthread] [pausa_us]\n",
                    argv[0]);
            exit(1);
        }
    }

    /* Default sensati per ciascuna modalita'. */
    switch (g_mode) {
        case MODE_L2FIT:  g_bufsz = 64u << 10; g_pause_us = 0;   break;
        case MODE_THRASH: g_bufsz =  8u << 20; g_pause_us = 200; break;
        default:          g_bufsz =  8u << 20; g_pause_us = 0;   break;
    }

    if (argc > 2) {
        long mb = strtol(argv[2], NULL, 10);
        if (mb > 0) g_bufsz = (size_t)mb << 20;
    }
    if (argc > 3) {
        long n = strtol(argv[3], NULL, 10);
        if (n >= 1 && n <= MAX_THREADS) g_nthreads = (int)n;
    }
    if (argc > 4) {
        long p = strtol(argv[4], NULL, 10);
        if (p >= 0) g_pause_us = (int)p;
    }
}

int main(int argc, char **argv)
{
    pthread_t th[MAX_THREADS];
    struct sigaction sa;
    double t0, t1, elapsed;
    uint64_t total = 0;
    int i;

    parse_args(argc, argv);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("=====================================================\n");
    printf(" stress_mem  --  interferenza di memoria controllata\n");
    printf("=====================================================\n");
    printf("  modalita'    : %s\n", mode_name(g_mode));
    printf("  buffer       : %zu KB per thread (x2 buffer)\n", g_bufsz >> 10);
    printf("  thread       : %d  (CPU0..CPU%d)\n", g_nthreads, g_nthreads - 1);
    printf("  pausa        : %d us\n", g_pause_us);
    printf("  CPU3         : NON toccata (e' quella del thread real-time)\n");
    printf("-----------------------------------------------------\n");
    printf("  Ctrl-C per fermare e stampare la banda.\n\n");

    t0 = now_seconds();

    for (i = 0; i < g_nthreads; i++) {
        if (pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i) != 0) {
            fprintf(stderr, "pthread_create fallita per CPU%d\n", i);
            g_running = 0;
            break;
        }
    }

    for (i = 0; i < g_nthreads; i++)
        pthread_join(th[i], NULL);

    t1 = now_seconds();
    elapsed = t1 - t0;
    if (elapsed <= 0.0) elapsed = 1e-9;

    for (i = 0; i < MAX_THREADS; i++)
        total += g_bytes[i];

    printf("\n-----------------------------------------------------\n");
    printf("  durata       : %.1f s\n", elapsed);
    printf("  byte mossi   : %.2f GB\n", (double)total / (1024.0*1024.0*1024.0));
    printf("  banda media  : %.0f MB/s\n",
           (double)total / (1024.0*1024.0) / elapsed);
    printf("  (sink = %llu, ignorare: serve solo a non far ottimizzare via il ciclo)\n",
           (unsigned long long)g_sink);
    printf("=====================================================\n");

    return 0;
}
