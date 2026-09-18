/*
    carico_gui.c — carico GUI riproducibile per le campagne RT
    ---------------------------------------------------------------------------
    PERCHE' ESISTE

    Le campagne RT confrontano due configurazioni (SDL contro DRM, COND_SYNC 0
    contro 1, ...) e richiedono che il carico sia IDENTICO nei due bracci. Finora
    il carico era manuale, e questo ha gia' invalidato tre misure:

      - A/B su PEG_DRM_FULLWIDTH_PCT: inconcludente, la variabilita' fra sessioni
        (616 -> 809 us per blit a parita' di configurazione) superava l'effetto
      - test COND_SYNC di 30 minuti: l'interfaccia era in gran parte ferma
      - confronti a 3 minuti: carico "uguale" solo per dichiarazione

    ---------------------------------------------------------------------------
    TRE MODALITA'

    1) REGISTRA   legge il touchscreen reale e salva i tuoi gesti su file
    2) RIPRODUCI  ricrea quei gesti su un touchscreen virtuale, in ciclo
    3) SINTETICA  esegue una sequenza scritta a mano nel sorgente (ripiego)

    La modalita' 1+2 e' quella giusta: fai TU l'interazione che vuoi misurare,
    una volta, e poi la si ripete identica per ore e in entrambi i bracci
    dell'A/B. Niente coordinate indovinate, niente dialog aperti per sbaglio.

    ---------------------------------------------------------------------------
    ⚠️  SICUREZZA

    Questa e' l'HMI di una pressa piegatrice. Le icone della toolbar cambiano
    stato macchina e, per AUTO e SAUTO, passano da AttivaMenu ->
    ConfigButtonsStartStopPlusMinus: il cablaggio dei tasti fisici Start / Stop /
    Plus / Minus. Vedi il vincolo non negoziabile nel CLAUDE.md del progetto.

    Durante la REGISTRAZIONE tocchi tu, quindi controlli cosa succede: e' il
    momento in cui decidere cosa deve e cosa non deve finire nel ciclo.
    Non lasciare mai una riproduzione in esecuzione su una macchina in pressione.

    ---------------------------------------------------------------------------
    USO

        ./carico_gui --registra sessione.rec
              registra dal touchscreen reale finche' non premi Ctrl+C.
              Il device sorgente si trova da solo; forzalo con --da /dev/input/eventX

        ./carico_gui --riproduci sessione.rec --minuti 60
              ricrea la registrazione in ciclo per un'ora

        ./carico_gui --riproduci sessione.rec
              in ciclo all'infinito, Ctrl+C per fermare

        ./carico_gui --probe 300 320
              un solo tocco, per capire cosa c'e' in un punto

        ./carico_gui --sintetica --minuti 60
              sequenza scritta nel sorgente (vedi g_sequenza[])

        ./carico_gui --lista
              stampa la sequenza sintetica senza eseguirla

    Dopo l'avvio, trovare il device virtuale e passarlo all'HMI:

        grep -A5 -i carico-gui /proc/bus/input/devices | grep -o "event[0-9]*"
        PEGDRM_TOUCH_DEV=/dev/input/eventN ./PegExec

    ⚠️  Il generatore va avviato PRIMA di PegExec: pegdrm_evdev apre il device
    all'inizializzazione.

    ---------------------------------------------------------------------------
    COMPILAZIONE

        gcc -O2 -o carico_gui carico_gui.c                       (sul target)

        bash -c '. /opt/avnet/imx8mp/environment-setup-armv8a-poky-linux; \
                 $CC -O2 -Wall -o carico_gui carico_gui.c'       (cross da WSL)
*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define SCREEN_W      1024
#define SCREEN_H       600
#define TOUCH_SCALE     16          /* 16384/1024 e 9600/600 */
#define TOUCH_MAX_X  (SCREEN_W * TOUCH_SCALE)
#define TOUCH_MAX_Y  (SCREEN_H * TOUCH_SCALE)

/* Pausa massima riprodotta fra due eventi: evita che una pausa lunga in
   registrazione (es. sei andato a prendere un caffe') diventi tempo morto. */
#define PAUSA_MAX_US  (2 * 1000 * 1000)

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

/*
    I segnali vanno installati con sigaction e SENZA SA_RESTART.

    signal() sotto glibc implica SA_RESTART: la read() bloccante sul touchscreen
    verrebbe interrotta dal Ctrl+C, l'handler imposterebbe g_stop... e poi la
    read() ripartirebbe da capo, bloccandosi di nuovo. Il ciclo non tornerebbe
    mai a controllare g_stop e la registrazione non terminerebbe piu'
    (sintomo osservato: Ctrl+C ignorato finche' non si tocca lo schermo).

    Con sa_flags = 0 la read() ritorna -1/EINTR e il ciclo puo' uscire.
*/
static void installa_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* ------------------------------------------------------------------------- */
/* Sequenza sintetica (modalita' di ripiego)                                  */
/* ------------------------------------------------------------------------- */

typedef enum { TAP, DRAG, WAIT } TipoAzione;

typedef struct {
    TipoAzione tipo;
    int x1, y1, x2, y2;
    int ms;
    const char *nota;
} Azione;

static const Azione g_sequenza[] = {
    { DRAG, 150, 250, 450, 400, 700, "pan area contenuto, diagonale" },
    { WAIT,   0,   0,   0,   0, 300, "" },
    { DRAG, 450, 400, 150, 250, 700, "ritorno" },
    { WAIT,   0,   0,   0,   0, 300, "" },
    { DRAG, 300, 480, 300, 180, 600, "scroll verticale" },
    { WAIT,   0,   0,   0,   0, 300, "" },
    { DRAG, 300, 180, 300, 480, 600, "ritorno" },
    { WAIT,   0,   0,   0,   0, 700, "" },
};
static const int g_nAzioni = (int)(sizeof(g_sequenza) / sizeof(g_sequenza[0]));

/* ------------------------------------------------------------------------- */
/* uinput                                                                     */
/* ------------------------------------------------------------------------- */

static void emit(int fd, int type, int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = (unsigned short)type;
    ev.code  = (unsigned short)code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        fprintf(stderr, "carico_gui: write fallita\n");
}

static void sync_report(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

/* Codici che il device virtuale dichiara. La riproduzione scarta gli altri:
   uinput li filtrerebbe comunque, e l'app non li usa. */
static int codice_supportato(int type, int code)
{
    if (type == EV_SYN) return 1;
    if (type == EV_KEY) return code == BTN_TOUCH || code == BTN_TOOL_FINGER;
    if (type == EV_ABS) {
        switch (code) {
        case ABS_X: case ABS_Y:
        case ABS_PRESSURE:
        case ABS_MT_SLOT: case ABS_MT_TRACKING_ID:
        case ABS_MT_POSITION_X: case ABS_MT_POSITION_Y:
        case ABS_MT_TOUCH_MAJOR: case ABS_MT_WIDTH_MAJOR: case ABS_MT_PRESSURE:
            return 1;
        default: return 0;
        }
    }
    return 0;
}

static int crea_device(void)
{
    struct uinput_user_dev ud;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("carico_gui: open /dev/uinput (serve root e il modulo uinput)");
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
    /* INPUT_PROP_DIRECT = touchscreen, non touchpad: senza, le coordinate
       assolute vengono interpretate come relative. */
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_PRESSURE);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_WIDTH_MAJOR);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);

    memset(&ud, 0, sizeof(ud));
    snprintf(ud.name, UINPUT_MAX_NAME_SIZE, "carico-gui-virtual-touch");
    ud.id.bustype = BUS_VIRTUAL;
    ud.id.vendor  = 0x0001;
    ud.id.product = 0x0001;
    ud.id.version = 1;
    ud.absmin[ABS_X] = 0;               ud.absmax[ABS_X] = TOUCH_MAX_X;
    ud.absmin[ABS_Y] = 0;               ud.absmax[ABS_Y] = TOUCH_MAX_Y;
    ud.absmin[ABS_MT_POSITION_X] = 0;   ud.absmax[ABS_MT_POSITION_X] = TOUCH_MAX_X;
    ud.absmin[ABS_MT_POSITION_Y] = 0;   ud.absmax[ABS_MT_POSITION_Y] = TOUCH_MAX_Y;
    ud.absmin[ABS_MT_SLOT] = 0;         ud.absmax[ABS_MT_SLOT] = 9;
    ud.absmin[ABS_MT_TRACKING_ID] = 0;  ud.absmax[ABS_MT_TRACKING_ID] = 65535;
    ud.absmin[ABS_PRESSURE] = 0;        ud.absmax[ABS_PRESSURE] = 255;
    ud.absmin[ABS_MT_PRESSURE] = 0;     ud.absmax[ABS_MT_PRESSURE] = 255;
    ud.absmin[ABS_MT_TOUCH_MAJOR] = 0;  ud.absmax[ABS_MT_TOUCH_MAJOR] = 255;
    ud.absmin[ABS_MT_WIDTH_MAJOR] = 0;  ud.absmax[ABS_MT_WIDTH_MAJOR] = 255;

    if (write(fd, &ud, sizeof(ud)) != (ssize_t)sizeof(ud)) {
        perror("carico_gui: write uinput_user_dev");
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("carico_gui: UI_DEV_CREATE");
        close(fd);
        return -1;
    }
    usleep(300 * 1000);          /* lascia comparire il nodo in /dev/input */
    return fd;
}

static void dormi_us(long us)
{
    struct timespec ts;
    if (us <= 0) return;
    ts.tv_sec  = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000L;
    nanosleep(&ts, NULL);
}

static void dormi_ms(int ms) { dormi_us((long)ms * 1000); }

/* ------------------------------------------------------------------------- */
/* Gesti sintetici                                                            */
/* ------------------------------------------------------------------------- */

static int g_tracking = 1;

static void giu(int fd, int sx, int sy)
{
    emit(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, g_tracking++);
    emit(fd, EV_ABS, ABS_MT_POSITION_X, sx * TOUCH_SCALE);
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, sy * TOUCH_SCALE);
    emit(fd, EV_KEY, BTN_TOUCH, 1);
    emit(fd, EV_ABS, ABS_X, sx * TOUCH_SCALE);
    emit(fd, EV_ABS, ABS_Y, sy * TOUCH_SCALE);
    sync_report(fd);
}

static void muovi(int fd, int sx, int sy)
{
    emit(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(fd, EV_ABS, ABS_MT_POSITION_X, sx * TOUCH_SCALE);
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, sy * TOUCH_SCALE);
    emit(fd, EV_ABS, ABS_X, sx * TOUCH_SCALE);
    emit(fd, EV_ABS, ABS_Y, sy * TOUCH_SCALE);
    sync_report(fd);
}

static void su(int fd)
{
    emit(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(fd, EV_KEY, BTN_TOUCH, 0);
    sync_report(fd);
}

static void tap(int fd, int sx, int sy, int ms)
{
    giu(fd, sx, sy);
    dormi_ms(ms > 0 ? ms : 80);
    su(fd);
}

static void drag(int fd, int x1, int y1, int x2, int y2, int ms)
{
    const int passi = (ms / 10) > 2 ? (ms / 10) : 2;
    int i;
    giu(fd, x1, y1);
    for (i = 1; i <= passi && !g_stop; ++i) {
        muovi(fd, x1 + (x2 - x1) * i / passi, y1 + (y2 - y1) * i / passi);
        dormi_ms(10);
    }
    su(fd);
}

/* ------------------------------------------------------------------------- */
/* Registrazione                                                              */
/* ------------------------------------------------------------------------- */

/* Cerca un touchscreen reale: un device con ABS_MT_POSITION_X che non sia il
   nostro virtuale. Stessa logica della scansione in pegdrm_evdev. */
static int trova_touch_reale(char *out, size_t n)
{
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    int trovato = 0;
    if (!d) return 0;
    while (!trovato && (e = readdir(d)) != NULL) {
        char path[sizeof(e->d_name) + 32], nome[256];
        unsigned long bits[ (ABS_MAX/(8*sizeof(long))) + 1 ];
        int fd;
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        memset(nome, 0, sizeof(nome));
        memset(bits, 0, sizeof(bits));
        if (ioctl(fd, EVIOCGNAME(sizeof(nome) - 1), nome) >= 0 &&
            strstr(nome, "carico-gui") == NULL &&
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) >= 0) {
            const int idx = ABS_MT_POSITION_X / (8 * (int)sizeof(long));
            const int off = ABS_MT_POSITION_X % (8 * (int)sizeof(long));
            if (bits[idx] & (1UL << off)) {
                snprintf(out, n, "%s", path);
                printf("carico_gui: sorgente = %s (\"%s\")\n", path, nome);
                trovato = 1;
            }
        }
        close(fd);
    }
    closedir(d);
    return trovato;
}

static int registra(const char *file, const char *dev_forzato)
{
    char dev[320];
    int fd_in, n_ev = 0;
    FILE *out;
    struct input_event ev;

    if (dev_forzato) snprintf(dev, sizeof(dev), "%s", dev_forzato);
    else if (!trova_touch_reale(dev, sizeof(dev))) {
        fprintf(stderr, "carico_gui: nessun touchscreen trovato, usa --da /dev/input/eventX\n");
        return 1;
    }

    fd_in = open(dev, O_RDONLY);
    if (fd_in < 0) { perror("carico_gui: open sorgente"); return 1; }

    out = fopen(file, "wb");
    if (!out) { perror("carico_gui: fopen file"); close(fd_in); return 1; }

    printf("carico_gui: REGISTRAZIONE in corso su '%s'.\n", file);
    printf("            Fai adesso l'interazione che vuoi ripetere.\n");
    printf("            Ctrl+C per terminare.\n");

    while (!g_stop) {
        ssize_t r = read(fd_in, &ev, sizeof(ev));
        if (r < 0) {
            if (errno == EINTR) break;          /* Ctrl+C: uscita pulita */
            perror("carico_gui: read sorgente");
            break;
        }
        if (r != (ssize_t)sizeof(ev)) break;
        if (fwrite(&ev, sizeof(ev), 1, out) != 1) break;
        if (++n_ev % 500 == 0) {
            printf("  %d eventi\r", n_ev);
            fflush(stdout);
            fflush(out);        /* cosi' un kill brutale non azzera il lavoro */
        }
    }

    fclose(out);
    close(fd_in);
    printf("\ncarico_gui: registrati %d eventi in '%s'\n", n_ev, file);
    if (n_ev == 0)
        fprintf(stderr, "carico_gui: ATTENZIONE, nessun evento: device sbagliato?\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Riproduzione                                                               */
/* ------------------------------------------------------------------------- */

static long delta_us(struct timeval a, struct timeval b)   /* a - b */
{
    return (a.tv_sec - b.tv_sec) * 1000000L + (a.tv_usec - b.tv_usec);
}

static int riproduci(int fd, const char *file, int minuti)
{
    long cicli = 0;
    time_t t0 = time(NULL);

    printf("carico_gui: RIPRODUZIONE di '%s'", file);
    if (minuti > 0) printf(" per %d minuti", minuti);
    else            printf(" (infinito, Ctrl+C per fermare)");
    printf("\n");

    while (!g_stop) {
        FILE *in;
        struct input_event ev, prec;
        int primo = 1, scartati = 0, emessi = 0;

        if (minuti > 0 && difftime(time(NULL), t0) >= minuti * 60) break;

        in = fopen(file, "rb");
        if (!in) { perror("carico_gui: fopen registrazione"); return 1; }

        memset(&prec, 0, sizeof(prec));
        while (!g_stop && fread(&ev, sizeof(ev), 1, in) == 1) {
            if (!primo) {
                long d = delta_us(ev.time, prec.time);
                if (d < 0) d = 0;
                if (d > PAUSA_MAX_US) d = PAUSA_MAX_US;
                dormi_us(d);
            }
            prec = ev;
            primo = 0;
            if (!codice_supportato(ev.type, ev.code)) { ++scartati; continue; }
            emit(fd, ev.type, ev.code, ev.value);
            ++emessi;
        }
        fclose(in);

        if (cicli == 0)
            printf("carico_gui: %d eventi emessi, %d scartati per ciclo\n",
                   emessi, scartati);
        ++cicli;
        if ((cicli % 20) == 0)
            printf("carico_gui: %ld cicli, %.0f s\n", cicli, difftime(time(NULL), t0));
    }

    printf("carico_gui: terminato dopo %ld cicli, %.0f s\n",
           cicli, difftime(time(NULL), t0));
    return 0;
}

/* ------------------------------------------------------------------------- */

static void stampa_lista(void)
{
    int i, totale = 0;
    printf("Sequenza sintetica: %d azioni\n", g_nAzioni);
    for (i = 0; i < g_nAzioni; ++i) {
        const Azione *a = &g_sequenza[i];
        totale += a->ms;
        if (a->tipo == TAP)
            printf("  %2d. TAP  (%4d,%4d)            %4d ms  %s\n", i+1, a->x1, a->y1, a->ms, a->nota);
        else if (a->tipo == DRAG)
            printf("  %2d. DRAG (%4d,%4d)->(%4d,%4d) %4d ms  %s\n", i+1, a->x1, a->y1, a->x2, a->y2, a->ms, a->nota);
        else
            printf("  %2d. WAIT                       %4d ms\n", i+1, a->ms);
    }
    printf("Durata di un ciclo: %.1f s\n", totale / 1000.0);
}

static void uso(const char *prog)
{
    fprintf(stderr,
        "uso: %s MODALITA' [opzioni]\n\n"
        "  --registra FILE [--da /dev/input/eventX]\n"
        "        registra i tuoi gesti dal touchscreen reale (Ctrl+C per finire)\n"
        "  --riproduci FILE [--minuti N]\n"
        "        ricrea la registrazione in ciclo\n"
        "  --probe X Y\n"
        "        un solo tocco in (X,Y) pixel schermo\n"
        "  --sintetica [--minuti N]\n"
        "        sequenza scritta nel sorgente\n"
        "  --lista\n"
        "        stampa la sequenza sintetica\n", prog);
}

int main(int argc, char **argv)
{
    const char *file_rec = NULL, *dev_forzato = NULL;
    int minuti = 0, probe_x = -1, probe_y = -1;
    int modo_registra = 0, modo_riproduci = 0, modo_sintetica = 0;
    int fd, i;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--registra") && i + 1 < argc) { modo_registra = 1; file_rec = argv[++i]; }
        else if (!strcmp(argv[i], "--riproduci") && i + 1 < argc) { modo_riproduci = 1; file_rec = argv[++i]; }
        else if (!strcmp(argv[i], "--da") && i + 1 < argc) { dev_forzato = argv[++i]; }
        else if (!strcmp(argv[i], "--minuti") && i + 1 < argc) { minuti = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--probe") && i + 2 < argc) { probe_x = atoi(argv[++i]); probe_y = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--sintetica")) { modo_sintetica = 1; }
        else if (!strcmp(argv[i], "--lista")) { stampa_lista(); return 0; }
        else { uso(argv[0]); return 1; }
    }

    if (!modo_registra && !modo_riproduci && !modo_sintetica && probe_x < 0) {
        uso(argv[0]);
        return 1;
    }

    installa_handler();

    /* La registrazione non crea nessun device virtuale: legge e basta. */
    if (modo_registra)
        return registra(file_rec, dev_forzato);

    fd = crea_device();
    if (fd < 0) return 1;

    if (probe_x >= 0) {
        printf("probe: tocco singolo in (%d,%d). Guarda cosa succede a schermo.\n", probe_x, probe_y);
        tap(fd, probe_x, probe_y, 120);
        dormi_ms(500);
    } else if (modo_riproduci) {
        riproduci(fd, file_rec, minuti);
    } else {
        long cicli = 0;
        time_t t0 = time(NULL);
        printf("carico_gui: sequenza sintetica, %d azioni per ciclo\n", g_nAzioni);
        while (!g_stop) {
            if (minuti > 0 && difftime(time(NULL), t0) >= minuti * 60) break;
            for (i = 0; i < g_nAzioni && !g_stop; ++i) {
                const Azione *a = &g_sequenza[i];
                if (a->tipo == TAP)       tap(fd, a->x1, a->y1, a->ms);
                else if (a->tipo == DRAG) drag(fd, a->x1, a->y1, a->x2, a->y2, a->ms);
                else                      dormi_ms(a->ms);
            }
            if ((++cicli % 50) == 0)
                printf("carico_gui: %ld cicli, %.0f s\n", cicli, difftime(time(NULL), t0));
        }
        printf("carico_gui: terminato dopo %ld cicli\n", cicli);
    }

    su(fd);                      /* non lasciare un dito premuto */
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
