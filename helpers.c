/* Two small helper programs, built from this one file:
 *   gcc -DAUDIOBRIDGE ... -lasound -lpthread  -> audiobridge (sound out / mic in)
 *   gcc -DDEVRELAY    ... -ldl                -> devrelay (devices for static mod programs)
 */
#define _GNU_SOURCE
#ifdef AUDIOBRIDGE
/* helpers.c - sound between the MPC program and the phone's Linux sound card, with microphone recording.
 *
 * The phone's (virtual) sound card plays OR records, never both at once (opening one direction while the other
 * is open fails with "busy"), and the MPC program always does both. So the program talks to two pipes instead
 * of the card (see the ALSA setup in ./mpc run), and this program serves them:
 *   ROOT/tmp/mpc-play.fifo   the program's sound: 44.1 kHz, 32-bit, 6 channels  -> played as 16-bit stereo
 *   ROOT/tmp/mpc-mic.fifo    the program's input, in the format it chose (ROOT/tmp/mpc-mic.fmt) <- the microphone, or silence
 * While ROOT/tmp/mpc-mic.on exists (the add-on creates it while SAMPLE RECORD is lit) the card records instead
 * of playing: the program's sound is dropped at the same pace and the microphone goes to its input.
 *
 *   audiobridge ROOT [period frames]          (started by ./mpc run, as root)
 */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RATE 44100
#define CH 6   /* the program's outputs (it opens 6 when the card allows any number) */
static char play_path[512], mic_path[512], flag_path[512];
static unsigned period = 512;
static int test_ramp;
static int gain = 1;   /* microphone boost (MIC_GAIN=2 or more if it is too quiet) */
static volatile int want_mic, card_mic = -1;   /* what the add-on asks for, what the card does now (-1: nothing) */
static snd_pcm_t *pcm;                           /* the card, open in one direction */
static pthread_mutex_t card = PTHREAD_MUTEX_INITIALIZER;

static void note(const char *s) { fprintf(stderr, "audiobridge: %s\n", s); }
static long now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000L + t.tv_nsec / 1000; }

static unsigned cap_rate = RATE;   /* the rate the microphone actually runs at (resampled to 44.1 kHz) */
static snd_pcm_t *open_card(int capture) {
    unsigned latency = (unsigned)(period * 4ULL * 1000000ULL / RATE);
    /* playback: hw first (no conversion). capture: the way the working mic test did it, 48 kHz with a large buffer */
    struct { const char *dev; unsigned rate, latency; } pb[] = { { "hw:0", RATE, latency }, { "plughw:0", RATE, latency } },
        cp[] = { { "plughw:0", 48000, 200000 }, { "hw:0", 48000, 200000 }, { "plughw:0", RATE, 200000 }, { "default", 48000, 500000 } };
    int nt = capture ? 4 : 2;
    for (int tries = 0; tries < 12; tries++) {   /* the other direction may take a moment to let go */
        for (int d = 0; d < nt; d++) {
            const char *dev = capture ? cp[d].dev : pb[d].dev;
            unsigned rate = capture ? cp[d].rate : pb[d].rate, lat = capture ? cp[d].latency : pb[d].latency;
            snd_pcm_t *p;
            int e = snd_pcm_open(&p, dev, capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK, 0);
            if (e == 0 && (e = snd_pcm_set_params(p, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1, lat)) == 0) {
                if (capture) {
                    int16_t probe[2 * 256];   /* make sure it really records */
                    snd_pcm_start(p);
                    snd_pcm_sframes_t r = snd_pcm_readi(p, probe, 256);
                    if (r < 0) { e = (int)r; snd_pcm_close(p); fprintf(stderr, "audiobridge: mic %s %u Hz reads fail: %s\n", dev, rate, snd_strerror(e)); continue; }
                    cap_rate = rate;
                    fprintf(stderr, "audiobridge: mic %s %u Hz\n", dev, rate);
                }
                return p;
            }
            if (e == 0) snd_pcm_close(p);
            if (tries == 0 || tries == 11) fprintf(stderr, "audiobridge: %s %s %u Hz: %s\n", capture ? "mic" : "sound", dev, rate, snd_strerror(e));
        }
        usleep(25000);
    }
    return 0;
}

/* switch the card to what the add-on asks for (called by the play thread between blocks) */
static void follow(void) {
    static long failed_at; static int failed_want = -1;
    int w = want_mic;
    if (w == card_mic) return;
    if (card_mic < 0 && w == failed_want && now_us() - failed_at < 2000000) return;   /* no card: retry every 2 s */
    pthread_mutex_lock(&card);
    if (pcm) { snd_pcm_drop(pcm); snd_pcm_close(pcm); pcm = 0; }
    pcm = open_card(w);
    card_mic = pcm ? w : -1;
    if (!pcm) { failed_at = now_us(); failed_want = w; }
    note(pcm ? (w ? "microphone on (sound off while recording)" : "sound on") : "could not open the sound card");
    pthread_mutex_unlock(&card);
}

static int open_fifo(const char *path, int size) {
    unlink(path);
    if (mkfifo(path, 0666) != 0) { perror(path); exit(1); }
    chmod(path, 0666);
    int fd = open(path, O_RDWR);   /* read and write: opening never waits, and the program's end never sees an EOF */
    if (fd < 0) { perror(path); exit(1); }
    fcntl(fd, F_SETPIPE_SZ, size);
    return fd;
}

static int playfd, micfd;

/* the program's sound -> the card (or dropped, at the same pace, while recording) */
static void *play_thread(void *u) {
    (void)u;
    size_t n = period;
    int32_t *in = malloc(n * 4 * CH); int16_t *out = malloc(n * 4);
    long t0 = 0; unsigned long dropped = 0;
    for (;;) {
        size_t got = 0, want = n * 4 * CH;   /* one whole block of 6 x 32-bit frames */
        while (got < want) { ssize_t r = read(playfd, (char *)in + got, want - got); if (r > 0) got += (size_t)r; else if (r < 0 && errno != EINTR) usleep(1000); }
        for (size_t i = 0; i < n; i++) {   /* L = ch1 + ch3 + ch5, R = ch2 + ch4 + ch6 (the MPC uses one of the pairs) */
            const int32_t *f = in + CH * i;
            long l = ((long)f[0] + f[2] + f[4]) >> 16, r = ((long)f[1] + f[3] + f[5]) >> 16;
            out[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            out[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        follow();
        if (card_mic == 0 && pcm) {
            t0 = 0;
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, out, n);
            if (w < 0) snd_pcm_recover(pcm, (int)w, 1);
        } else {   /* recording (or no card): keep the program's pace with the clock */
            long t = now_us();
            if (!t0 || t - t0 > 500000 + (long)(dropped * 1000000ULL / RATE)) { t0 = t; dropped = 0; }
            dropped += n;
            long due = t0 + (long)(dropped * 1000000ULL / RATE);
            if (due > t) usleep((useconds_t)(due - t));
        }
    }
    return 0;
}

/* the program's input format, noted by the add-on in ROOT/tmp/mpc-mic.fmt: ALSA format number, channels, rate */
static char fmt_path[512];
static int in_fmt = 10, in_ch = 6; static unsigned in_rate = RATE;   /* until noted: 32-bit, 6 channels, 44.1 kHz */
static void read_fmt(void) {
    FILE *f = fopen(fmt_path, "r"); if (!f) return;
    int fm; unsigned ch, rate;
    if (fscanf(f, "%d %u %u", &fm, &ch, &rate) == 3 && ch >= 1 && ch <= 32 && rate >= 8000 && rate <= 192000 &&
        (fm == 2 || fm == 10 || fm == 14 || fm == 6 || fm == 32)) {
        if (fm != in_fmt || (int)ch != in_ch || rate != in_rate) {
            char m[96]; snprintf(m, sizeof m, "program input: format %d, %u channels, %u Hz", fm, ch, rate); note(m);
        }
        in_fmt = fm; in_ch = (int)ch; in_rate = rate;
    }
    fclose(f);
}
static int sample_bytes(void) { return in_fmt == 2 ? 2 : in_fmt == 32 ? 3 : 4; }
/* one stereo sample pair -> one frame in the program's format (left on odd channels, right on even ones) */
static void put_frame(unsigned char *o, int l, int r) {
    for (int c = 0; c < in_ch; c++) {
        int v = c & 1 ? r : l;   /* 16-bit value */
        switch (in_fmt) {
        case 2:  o[0] = (unsigned char)v; o[1] = (unsigned char)(v >> 8); o += 2; break;
        case 10: { int32_t x = (int32_t)((uint32_t)v << 16); memcpy(o, &x, 4); o += 4; break; }
        case 6:  { int32_t x = v * 256; memcpy(o, &x, 4); o += 4; break; }
        case 32: { int x = v * 256; o[0] = (unsigned char)x; o[1] = (unsigned char)(x >> 8); o[2] = (unsigned char)(x >> 16); o += 3; break; }
        default: { float x = (float)v / 32768.0f; memcpy(o, &x, 4); o += 4; break; }
        }
    }
}

/* the microphone (or silence) -> the program's input. The pipe is large: the card gives the microphone in bursts,
 * and the program must always get whole reads */
static void *mic_thread(void *u) {
    (void)u;
    size_t n = period;
    int16_t *buf = calloc(n * 8 + 2, 2), *in = buf + 2;   /* card frames (stereo 16-bit); buf[0..1] = the previous block's last */
    int have_prev = 0;
    unsigned char *out = malloc(n * 4 * 32 * 2);   /* up to 2n frames of up to 32 channels x 4 bytes */
    long last = now_us(); int peak = 0; unsigned long frames = 0, taken = 0; int errs = 0, lasterr = 0;
    double pos = 0; unsigned long long sent = 0;   /* bytes written so far */
    read_fmt();
    for (;;) {
        if (now_us() - last >= 1000000) {   /* once a second: the program's format; while recording, the levels */
            read_fmt();
            if (card_mic == 1) {
                char m[200];
                snprintf(m, sizeof m, "mic: %lu frames/s from the card (%u Hz), peak %d (of 32767, x%d), program took %lu frames/s, errors %d %s",
                         frames, cap_rate, peak, gain, taken, errs, errs ? snd_strerror(lasterr) : "");
                note(m);
            }
            last = now_us(); peak = 0; frames = 0; taken = 0; errs = 0;
        }
        int fb = sample_bytes() * in_ch, outn = 0;
        if (card_mic == 1) {
            pthread_mutex_lock(&card);
            if (card_mic == 1 && pcm) {
                size_t want = n * cap_rate / in_rate; if (want < 16) want = 16; if (want > n * 4) want = n * 4;
                snd_pcm_sframes_t r;
                if (test_ramp) {   /* AB_TEST_RAMP=1: a rising test signal at 48 kHz instead of the microphone, real-time pace */
                    static long t0; static unsigned long made; static int val;
                    if (!t0) t0 = now_us();
                    long due = t0 + (long)((made + want) * 1000000ULL / 48000), t = now_us();
                    if (due > t) usleep((useconds_t)(due - t));
                    for (size_t k = 0; k < want; k++) { in[2 * k] = in[2 * k + 1] = (int16_t)(val - 15000); val = (val + 1) % 30000; }
                    made += want; r = (snd_pcm_sframes_t)want;
                } else r = snd_pcm_readi(pcm, in, want);
                if (r < 0) { errs++; lasterr = (int)r; snd_pcm_recover(pcm, (int)r, 1); }
                else if (r > 0) {
                    frames += (unsigned long)r;
                    for (long k = 0; k < r * 2; k++) { int v = in[k] * gain; in[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); if (in[k] > peak) peak = in[k]; }
                    /* linear resampling to the program's rate, continuing across blocks (the previous block's last
                     * frame is frame 0 here) */
                    const int16_t *v = have_prev ? buf : in; long tot = have_prev ? r + 1 : r;
                    double step = (double)cap_rate / in_rate;
                    while (pos < tot - 1 && outn < (int)n * 2) {
                        long i0 = (long)pos; double f = pos - i0;
                        int l0 = v[2 * i0], r0 = v[2 * i0 + 1], l1 = v[2 * i0 + 2], r1 = v[2 * i0 + 3];
                        put_frame(out + (size_t)outn * fb, (int)(l0 + (l1 - l0) * f), (int)(r0 + (r1 - r0) * f));
                        outn++; pos += step;
                    }
                    pos -= (double)(tot - 1); if (pos < 0) pos = 0;
                    buf[0] = in[2 * (r - 1)]; buf[1] = in[2 * (r - 1) + 1]; have_prev = 1;
                }
            }
            pthread_mutex_unlock(&card);
        }
        if (!outn) {   /* silence; at the program's pace while recording fails, faster otherwise (keeps the pipe full) */
            outn = (int)n; memset(out, 0, (size_t)outn * fb);
            if (in_fmt == 14) for (int k = 0; k < outn * in_ch; k++) { float z = 0; memcpy(out + k * 4, &z, 4); }
            usleep((useconds_t)(n * 1000000ULL / in_rate / (card_mic == 1 ? 1 : 2)));
        }
        /* keep the byte stream in whole frames of the current format (it may change once the program sets up) */
        if (sent % (unsigned long long)fb) {
            static const unsigned char z[256];
            size_t pad = fb - (size_t)(sent % (unsigned long long)fb);
            ssize_t w = write(micfd, z, pad); if (w > 0) sent += (unsigned long long)w;
            continue;
        }
        const unsigned char *p = out; size_t left = (size_t)outn * fb;
        while (left) { ssize_t w = write(micfd, p, left); if (w > 0) { p += w; left -= (size_t)w; sent += (unsigned long long)w; taken += (unsigned long)w / (unsigned)fb; } else if (errno != EINTR) break; }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: audiobridge ROOT [period]\n"); return 2; }
    if (argc > 2 && atoi(argv[2]) >= 64) period = (unsigned)atoi(argv[2]);
    if (getenv("MIC_GAIN") && atoi(getenv("MIC_GAIN")) >= 1) gain = atoi(getenv("MIC_GAIN"));
    test_ramp = getenv("AB_TEST_RAMP") != 0;
    snprintf(play_path, sizeof play_path, "%s/tmp/mpc-play.fifo", argv[1]);
    snprintf(mic_path, sizeof mic_path, "%s/tmp/mpc-mic.fifo", argv[1]);
    snprintf(flag_path, sizeof flag_path, "%s/tmp/mpc-mic.on", argv[1]);
    snprintf(fmt_path, sizeof fmt_path, "%s/tmp/mpc-mic.fmt", argv[1]);
    unlink(flag_path);
    playfd = open_fifo(play_path, 8192);    /* small: the program's sound stays in time */
    micfd = open_fifo(mic_path, 262144);     /* large: the card gives the microphone in bursts, and the program must always
                                               get whole reads (a short read leaves old sound in its buffer: clicks) */
    pthread_t a, b;
    pthread_create(&a, 0, play_thread, 0);
    pthread_create(&b, 0, mic_thread, 0);
    note("ready");
    for (;;) {   /* the add-on's request: microphone while SAMPLE RECORD is lit */
        want_mic = access(flag_path, F_OK) == 0;   /* the play thread switches the card between its blocks */
        usleep(50000);
    }
}

#endif /* AUDIOBRIDGE */
#ifdef DEVRELAY
/* helpers.c - the emulated MPC devices for STATIC programs (started by ./mpc run for MPC_BOOT=auto).
 *
 * The add-on (fbshim.so) gives every program the MPC's hardware by hooking libc calls. A statically linked
 * program (SampleBoot's sampleboot-ui, many mod UIs) has no dynamic libc, so ld.so.preload never reaches it:
 * it opens the real paths. On the real MPC those paths are devices; here this helper provides them as files:
 *
 *   /dev/fb0            -> /run/mpc-fb0, a plain 320x240 BGRX frame file. Every change is shown on the
 *                          display with the on-screen panel (panel_fb_present, the same path the add-on uses).
 *   /dev/snd/midiC8D0   -> a raw pseudo-terminal joined to the fake control chip (card 8, via mpcctl): panel
 *                          touches (pads, buttons, the encoder) reach the program as the MPC's own MIDI, and
 *                          what it sends back (lights) lights the panel. (/proc/asound/cards lists card 8 as
 *                          "Surface"; ./mpc run adds that line.)
 *
 * The helper runs inside the chroot with fbshim.so preloaded (that is where the panel lives) and reads the
 * touchscreen through libinput, as the MPC app does. It shows a static program's frames only while the MPC
 * app is NOT running, and hands the display back the moment the app starts (one owner at a time).
 * It never holds the MIDI port open itself, so tools that ask "does any process have the surface port open?"
 * (SampleBoot's check for a running OS) see only real users.
 */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define FRAME (320 * 240 * 4)
#define CARD8 "/dev/snd/midiC8D0"   /* the literal path the add-on fakes (always the socket to mpcctl) */

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }
static long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }
static void say(const char *a, const char *b) { fprintf(stderr, "devrelay: %s%s%s\n", a, b ? " " : "", b ? b : ""); }

/* who has the display? 2 = the stock MPC app (it reads the touchscreen itself), 1 = another program that draws
 * with DRM itself - an app made for the MPC like Melodee's sample-base, found by name or (deep = 1) by an open
 * /dev/dri device: the panel's touches still come from here - 0 = nobody (a static program's /dev/fb0 is shown) */
static int app_running(int deep) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    int found = 0;
    char p[300], c[64], self[16];
    snprintf(self, sizeof self, "%d", (int)getpid());
    while (d && found < 2 && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9' || !strcmp(e->d_name, self)) continue;
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t n = read(fd, c, sizeof c - 1);
        close(fd);
        if (n <= 0) continue;
        c[n] = 0;
        if (c[n - 1] == '\n') c[n - 1] = 0;
        if (!strcmp(c, "MPC Sample")) { found = 2; break; }
        if (!strcmp(c, "sample-base") || !strcmp(c, "mpcsimple")) { found = 1; continue; }
        if (!deep || found) continue;
        char fdd[300];
        struct stat rs, me;   /* only programs in this system (the MPC's chroot), not the phone's own */
        snprintf(fdd, sizeof fdd, "/proc/%s/root", e->d_name);
        if (stat(fdd, &rs) != 0 || stat("/", &me) != 0 || rs.st_dev != me.st_dev || rs.st_ino != me.st_ino) continue;
        snprintf(fdd, sizeof fdd, "/proc/%s/fd", e->d_name);
        DIR *f = opendir(fdd);
        struct dirent *g;
        while (f && (g = readdir(f))) {
            char fp[600], t[64];
            snprintf(fp, sizeof fp, "%s/%s", fdd, g->d_name);
            ssize_t k = readlink(fp, t, sizeof t - 1);
            if (k > 0) { t[k] = 0; if (!strncmp(t, "/dev/dri/", 9)) { found = 1; break; } }
        }
        if (f) closedir(f);
    }
    if (d) closedir(d);
    return found;
}

/* FIFO mode: does any OTHER process have the port open? (only then is panel MIDI delivered) */
static int fifo_client(dev_t dev, ino_t ino) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    int found = 0;
    char self[16];
    snprintf(self, sizeof self, "%d", (int)getpid());
    while (!found && d && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9' || !strcmp(e->d_name, self)) continue;
        char fdd[300];
        snprintf(fdd, sizeof fdd, "/proc/%s/fd", e->d_name);
        DIR *f = opendir(fdd);
        struct dirent *g;
        while (f && (g = readdir(f))) {
            if (g->d_name[0] == '.') continue;
            char fp[600];
            struct stat st;
            snprintf(fp, sizeof fp, "%s/%s", fdd, g->d_name);
            if (stat(fp, &st) == 0 && S_ISFIFO(st.st_mode) && st.st_ino == ino && st.st_dev == dev) { found = 1; break; }
        }
        if (f) closedir(f);
    }
    if (d) closedir(d);
    return found;
}

/* ---- touchscreen through libinput (the panel's libinput_get_event wrapper turns panel touches into MIDI) ---- */
struct li_iface { int (*open_restricted)(const char *, int, void *); void (*close_restricted)(int, void *); };
static int li_open(const char *path, int flags, void *u) { (void)u; int fd = open(path, flags | O_CLOEXEC); return fd < 0 ? -errno : fd; }
static void li_close(int fd, void *u) { (void)u; close(fd); }
static const struct li_iface iface = { li_open, li_close };
static void *lctx;
static int (*li_dispatch)(void *);
static void *(*li_get_event)(void *);
static void (*li_destroy)(void *);

static int touch_open(void) {
    void *ud = dlopen("libudev.so.1", RTLD_NOW | RTLD_GLOBAL);
    void *li = dlopen("libinput.so.10", RTLD_NOW | RTLD_GLOBAL);
    if (!ud || !li) { say("no libinput/libudev: the panel is shown but can't be touched", dlerror()); return -1; }
    void *(*udev_new)(void) = (void *(*)(void))dlsym(ud, "udev_new");
    void *(*create)(const void *, void *, void *) = (void *(*)(const void *, void *, void *))dlsym(li, "libinput_udev_create_context");
    int (*assign)(void *, const char *) = (int (*)(void *, const char *))dlsym(li, "libinput_udev_assign_seat");
    int (*getfd)(void *) = (int (*)(void *))dlsym(li, "libinput_get_fd");
    li_dispatch = (int (*)(void *))dlsym(li, "libinput_dispatch");
    li_get_event = (void *(*)(void *))dlsym(RTLD_DEFAULT, "libinput_get_event");   /* the panel's wrapper */
    li_destroy = (void (*)(void *))dlsym(li, "libinput_event_destroy");
    if (!udev_new || !create || !assign || !getfd || !li_dispatch || !li_get_event || !li_destroy) { say("libinput: missing functions", 0); return -1; }
    void *u = udev_new();
    lctx = u ? create(&iface, 0, u) : 0;
    if (!lctx || assign(lctx, "seat0") != 0) { say("libinput: no touch seat", 0); lctx = 0; return -1; }
    return getfd(lctx);
}

int main(void) {
    const char *fbpath = getenv("MPC_RELAY_FB") ? getenv("MPC_RELAY_FB") : "/run/mpc-fb0";
    const char *linkpath = getenv("MPC_RELAY_MIDI") ? getenv("MPC_RELAY_MIDI") : CARD8;
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGHUP, on_sig); signal(SIGPIPE, SIG_IGN);

    int (*present)(const unsigned char *) = (int (*)(const unsigned char *))dlsym(RTLD_DEFAULT, "panel_fb_present");
    void (*release)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "panel_fb_release");
    if (!present) say("the add-on (fbshim.so) is not loaded: frames and the control chip are not available", 0);

    /* the control surface's MIDI port for static programs. Best: a raw pseudo-terminal (two-way, every byte
     * passes). The phone's Linux VM may have no pseudo-terminals (sudo's "unable to allocate pty"); ./mpc run then
     * mounts a private devpts instance in the chroot (/dev/pts/ptmx). If there are none at all: a named pipe,
     * fed only while a program has it open (the program also reads back its own writes - harmless for the
     * MPC's protocol: lights and requests are never encoder or push messages). */
    char pty[64] = "";
    int fifo = 0;
    dev_t fdev = 0; ino_t fino = 0;
    int m = getenv("MPC_RELAY_NOPTY") ? -1 : open("/dev/pts/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (m < 0 && !getenv("MPC_RELAY_NOPTY")) m = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (m >= 0 && !grantpt(m) && !unlockpt(m) && ptsname(m)) {
        snprintf(pty, sizeof pty, "%s", ptsname(m));
        int s = open(pty, O_RDWR | O_NOCTTY | O_CLOEXEC);
        struct termios t;
        if (s >= 0 && tcgetattr(s, &t) == 0) { cfmakeraw(&t); tcsetattr(s, TCSANOW, &t); }
        if (s >= 0) close(s);   /* never keep it open ourselves (see the top) */
        say("control surface port: pseudo-terminal", pty);
    } else {
        if (m >= 0) close(m);
        const char *fp = getenv("MPC_RELAY_FIFO") ? getenv("MPC_RELAY_FIFO") : "/run/mpc-midi8";
        unlink(fp);
        struct stat fs;
        if (mkfifo(fp, 0666) != 0 || (m = open(fp, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0 || fstat(m, &fs) != 0) {
            say("cannot create the control surface port", strerror(errno)); return 1;
        }
        snprintf(pty, sizeof pty, "%s", fp);
        fifo = 1; fdev = fs.st_dev; fino = fs.st_ino;
        say("no pseudo-terminals here: control surface port is a named pipe", fp);
    }
    int linked = 0;
    struct stat st;
    if (lstat(linkpath, &st) == 0 && !S_ISLNK(st.st_mode)) say("a real device is already there, left alone:", linkpath);
    else {
        unlink(linkpath);
        if (symlink(pty, linkpath) == 0) { linked = 1; say("control surface MIDI for static programs:", linkpath); }
        else say("cannot create", linkpath);
    }

    /* the frame file static programs get as /dev/fb0 */
    int fr = open(fbpath, O_RDONLY | O_CLOEXEC);
    if (fr < 0) {
        int w = open(fbpath, O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
        if (w >= 0) { if (ftruncate(w, FRAME)) {} close(w); }
        fr = open(fbpath, O_RDONLY | O_CLOEXEC);
    }
    int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fr < 0 || ino < 0 || inotify_add_watch(ino, fbpath, IN_MODIFY | IN_CLOSE_WRITE) < 0) say("cannot watch", fbpath);

    int lfd = present ? touch_open() : -1;
    int c8 = -1;
    long c8_retry = 0, hup_until = 0, app_chk = 0, last_pres = 0, deep_chk = 0;
    int dirty = 1, owned = 0, app = 0, nframes = 0, fifo_open = 0;   /* dirty: take the display (black) at once */
    static unsigned char frame[FRAME];
    say("ready", 0);

    while (!stop) {
        long now = ms();
        if (now - app_chk >= 150) {
            app_chk = now;
            int deep = now - deep_chk >= 1000, was = app;
            if (deep) deep_chk = now;
            app = app_running(deep);
            if (!deep && was == 1 && !app) app = 1;   /* found by its open display: only the deep look can tell it's gone */
            if (app && owned) { if (release) release(); owned = 0; say(app == 2 ? "the MPC app is up: display handed over" : "another program draws the screen: display handed over", 0); }
            if (fifo) {
                int c = fifo_client(fdev, fino);
                if (!c && fifo_open) { unsigned char b[4096]; while (read(m, b, sizeof b) > 0) {} }   /* no stale bytes for the next one */
                fifo_open = c;
            }
        }
        if (c8 < 0 && present && now >= c8_retry) {   /* the fake control chip (mpcctl), card 8 */
            c8 = open(CARD8, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (c8 < 0) c8_retry = now + 1000;
        }
        struct pollfd p[4];
        int n = 0, ip = -1, il = -1, ic = -1, im = -1;
        if (ino >= 0) { p[n] = (struct pollfd){ ino, POLLIN, 0 }; ip = n++; }
        if (lfd >= 0) { p[n] = (struct pollfd){ lfd, POLLIN, 0 }; il = n++; }
        if (c8 >= 0) { p[n] = (struct pollfd){ c8, POLLIN, 0 }; ic = n++; }
        if (!fifo && now >= hup_until) { p[n] = (struct pollfd){ m, POLLIN, 0 }; im = n++; }
        poll(p, (nfds_t)n, dirty ? 8 : 50);

        if (ip >= 0 && (p[ip].revents & POLLIN)) { char b[4096]; while (read(ino, b, sizeof b) > 0) {} dirty = 1; }
        if (il >= 0 && (p[il].revents & POLLIN)) {
            li_dispatch(lctx);
            void *ev;
            while ((ev = li_get_event(lctx))) li_destroy(ev);   /* panel touches were taken by the wrapper */
        }
        /* a static program has the port open when the master side is not hung up */
        int slave_open = fifo ? fifo_open : 0;
        if (im >= 0) {
            if (p[im].revents & POLLHUP) hup_until = ms() + 100;
            else {
                slave_open = 1;
                if (p[im].revents & POLLIN) {   /* what it sends (lights, requests) -> the control chip */
                    unsigned char b[512];
                    ssize_t r = read(m, b, sizeof b);
                    if (r > 0 && c8 >= 0 && write(c8, b, (size_t)r) < 0) {}
                }
            }
        }
        if (ic >= 0 && (p[ic].revents & (POLLIN | POLLHUP | POLLERR))) {   /* the surface's MIDI -> the program */
            unsigned char b[512];
            ssize_t r = read(c8, b, sizeof b);
            if (r <= 0) { close(c8); c8 = -1; c8_retry = ms() + 1000; }
            else if (slave_open && write(m, b, (size_t)r) < 0) {}   /* nobody listening: dropped, never queued */
        }
        if (owned && !dirty && ms() - last_pres >= 1000) dirty = 1;   /* redraw each second: follows a window resize */
        if (dirty && app) dirty = 0;   /* the app owns the screen */
        if (dirty && present && fr >= 0 && ms() - last_pres >= (owned ? 15 : 300)) {
            if (pread(fr, frame, FRAME, 0) == FRAME && present(frame)) {
                if (!owned) say("display taken (black until a static program draws)", 0);
                owned = 1; dirty = 0;
                if (nframes++ == 1) say("showing a static program's /dev/fb0", 0);
            }   /* not shown (something else has the display): keep trying every 0.3 s */
            last_pres = ms();
        }
    }
    if (linked) {
        char cur[64];
        ssize_t k = readlink(linkpath, cur, sizeof cur - 1);
        if (k > 0) { cur[k] = 0; if (!strcmp(cur, pty)) unlink(linkpath); }
    }
    if (fifo) unlink(pty);
    if (owned && release) release();
    say("stopped", 0);
    return 0;
}

#endif /* DEVRELAY */
