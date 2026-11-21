/* addon.c - the add-on (LD_PRELOAD, /tmp/fbshim.so in the chroot) that lets Akai's MPC Sample program run
 * in the phone's Linux Terminal.
 *
 *  - Display: a fake /dev/fb0 (the program clears one at start-up and crashes without it), and only the
 *    connected display output is shown to the program, so it draws where the Terminal's window can see it.
 *  - Control chip: the program's two internal USB-MIDI devices become fake ALSA cards 8 and 9 (and a fake
 *    sequencer) whose ports are Unix sockets served by mpc.py ctl, which feeds them the panel's MIDI.
 *  - Sound: platform-sound is the phone's card 0; the program's 32-bit 4-channel output is converted to the
 *    16-bit stereo the card plays cleanly; headphone and line-out jacks are reported plugged in.
 *  - microSD card: the phone folder mounted at /media/az01-internal-sd is never unmounted by the program.
 *  - A crash recorder writes where a crash happened to /tmp/fbshim.log.
 *
 * Only syscall() (and errno) are used from the C library, so it works with the firmware's own glibc.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/fb.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sound/asound.h>
#include <signal.h>
void panel_tap_release(void);   /* panel, below */
#include <ucontext.h>

#define FB_W 320
#define FB_H 240
#define BPP 32
#define PAGES 2
#define FBFILE "/tmp/fb0.raw"

long syscall(long number, ...);

static int fbfd = -1;
static struct fb_var_screeninfo var;
static int ready;

static void init_var(void) {
    char *p = (char *)&var;
    for (unsigned i = 0; i < sizeof var; i++) p[i] = 0;
    var.xres = FB_W; var.yres = FB_H; var.xres_virtual = FB_W; var.yres_virtual = FB_H * PAGES;
    var.bits_per_pixel = BPP;
    var.blue.offset = 0;  var.blue.length = 8;
    var.green.offset = 8; var.green.length = 8;
    var.red.offset = 16;  var.red.length = 8;
    var.transp.offset = 24; var.transp.length = 8;
    var.width = 49; var.height = 37;           /* mm, a 2.4" screen */
    var.pixclock = 100000;
    ready = 1;
}

static int is_fb(const char *p) {
    const char *f = "/dev/fb";
    for (int i = 0; f[i]; i++) if (p[i] != f[i]) return 0;
    return p[7] >= '0' && p[7] <= '9';
}

static int real_open(const char *path, int flags, int mode) {
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

/* ---- small helpers ---- */
static int str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void put(char *b, int *n, const char *s) { while (*s && *n < 250) b[(*n)++] = *s++; }
static void putu(char *b, int *n, unsigned v) { char t[12]; int m = 0; do { t[m++] = '0' + v % 10; v /= 10; } while (v); while (m && *n < 250) b[(*n)++] = t[--m]; }
static void logs(const char *a, const char *b2, unsigned v) {
    int fd = real_open("/tmp/fbshim.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    char b[256]; int n = 0; put(b, &n, a); if (b2) { put(b, &n, " "); put(b, &n, b2); } put(b, &n, " "); putu(b, &n, v); b[n++] = '\n';
    syscall(SYS_write, fd, b, n); syscall(SYS_close, fd);
}


/* ---- crash recorder: on a crash, write where it happened to
 * /tmp/fbshim.log, as program offsets (the program is loaded at 0xaaaaaaaa0000), then crash normally. */
#define PBASE 0xaaaaaaaa0000UL
static void hex(char *b, int *n, unsigned long v) {
    const char *h = "0123456789abcdef"; char t[20]; int m = 0;
    do { t[m++] = h[v & 15]; v >>= 4; } while (v);
    b[(*n)++] = '0'; b[(*n)++] = 'x'; while (m) b[(*n)++] = t[--m];
}
static void addr(char *b, int *n, const char *what, unsigned long v) {
    put(b, n, what);
    if (v >= PBASE && v < PBASE + 0x8000000UL) { put(b, n, " program+"); hex(b, n, v - PBASE); } else { put(b, n, " "); hex(b, n, v); }
    b[(*n)++] = '\n';
}
static void on_crash(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx; char b[1500]; int n = 0;
    put(b, &n, "CRASH signal "); putu(b, &n, (unsigned)sig);
    { char tn[17] = {0}; int fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/thread-self/comm", O_RDONLY, 0);
      if (fd >= 0) { long r = syscall(SYS_read, fd, tn, 16); syscall(SYS_close, fd); if (r > 0) { tn[r - 1 < 16 ? r - 1 : 15] = 0; put(b, &n, " thread "); put(b, &n, tn); } } }
    b[n++] = '\n';
#if defined(__aarch64__)
    addr(b, &n, "  fault address", (unsigned long)si->si_addr);
    addr(b, &n, "  pc", uc->uc_mcontext.pc & 0xffffffffffffUL);
    addr(b, &n, "  lr", uc->uc_mcontext.regs[30] & 0xffffffffffffUL);
    unsigned long fp = uc->uc_mcontext.regs[29];
    for (int i = 0; i < 14 && fp && !(fp & 7) && n < 1300; i++) {
        unsigned long *f = (unsigned long *)fp;
        addr(b, &n, "  called from", f[1] & 0xffffffffffffUL);
        if (f[0] <= fp) break;
        fp = f[0];
    }
#else
    (void)uc; addr(b, &n, "  fault address", (unsigned long)si->si_addr);
#endif
    int fd = (int)syscall(SYS_openat, AT_FDCWD, "/tmp/fbshim.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { syscall(SYS_write, fd, b, n); syscall(SYS_close, fd); }
    struct sigaction d = { 0 }; d.sa_handler = SIG_DFL; sigaction(sig, &d, 0);   /* crash normally on return */
}
static void install_crash(void) {
    static int done; if (done) return; done = 1;
    struct sigaction a = { 0 }; a.sa_sigaction = on_crash; a.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &a, 0); sigaction(SIGBUS, &a, 0); sigaction(SIGILL, &a, 0); sigaction(SIGABRT, &a, 0); sigaction(SIGFPE, &a, 0);
}
__attribute__((constructor)) static void shim_init(void) { install_crash(); }

/* ---- fake ALSA MIDI cards for the control chip ---- */
enum { F_NONE, F_CTL, F_MIDI };
static struct { int fd, kind, card, peer; unsigned mode; } fk[64];
static int fake_find(int fd) { for (int i = 0; i < 64; i++) if (fk[i].kind && fk[i].fd == fd) return i; return -1; }
static int fake_add(int fd, int kind, int card, int peer) {
    for (int i = 0; i < 64; i++) if (!fk[i].kind) { fk[i].fd = fd; fk[i].kind = kind; fk[i].card = card; fk[i].peer = peer; fk[i].mode = 0; return fd; }
    return fd;
}
static int fake_card_of(const char *p) {
    if (str_eq(p, "/dev/snd/by-path/platform-ff500000.usb-usb-0:1.5:1.0") || str_eq(p, "/dev/snd/controlC8")) return 8;
    if (str_eq(p, "/dev/snd/by-path/platform-ff500000.usb-usb-0:1.6:1.0") || str_eq(p, "/dev/snd/controlC9")) return 9;
    return -1;
}
static int fake_midi_of(const char *p) {
    if (str_eq(p, "/dev/snd/midiC8D0")) return 8;
    if (str_eq(p, "/dev/snd/midiC9D0")) return 9;
    return -1;
}
static int open_ctl(int card, int flags) {
    int sv[2];
    if (syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM | ((flags & O_CLOEXEC) ? SOCK_CLOEXEC : 0), 0, sv) != 0) return -1;
    logs("ctl open card", 0, card);
    return fake_add(sv[0], F_CTL, card, sv[1]);
}
static int open_midi(int card, int flags) {
    int fd = (int)syscall(SYS_socket, AF_UNIX, SOCK_STREAM | ((flags & O_CLOEXEC) ? SOCK_CLOEXEC : 0), 0);
    if (fd < 0) return -1;
    struct sockaddr_un a; char *z = (char *)&a; for (unsigned i = 0; i < sizeof a; i++) z[i] = 0;
    a.sun_family = AF_UNIX;
    const char *base = "/tmp/mpcctl-"; int n = 0; while (base[n]) { a.sun_path[n] = base[n]; n++; }
    a.sun_path[n++] = '0' + card; const char *ext = ".sock"; for (int i = 0; ext[i]; i++) a.sun_path[n++] = ext[i];
    if (syscall(SYS_connect, fd, &a, sizeof a) != 0) { logs("midi open: no mpcctl helper for card", 0, card); syscall(SYS_close, fd); errno = ENODEV; return -1; }
    if (flags & O_NONBLOCK) syscall(SYS_fcntl, fd, F_SETFL, O_NONBLOCK);
    logs("midi open card", 0, card);
    return fake_add(fd, F_MIDI, card, -1);
}

/* ---- fake ALSA sequencer (/dev/snd/seq) ----
 * JUCE reads MIDI devices through the ALSA sequencer, which the Pixel/Android Linux Terminal's kernel does not have.
 * Each open of /dev/snd/seq becomes a SOCK_SEQPACKET connection to mpc.py ctl (/tmp/mpcseq.sock).
 * Records on that socket are [card, MIDI bytes...]. Clients, ports and subscriptions are kept here;
 * read()/write() convert between sequencer events and MIDI bytes. Fake cards 8 and 9 appear as
 * sequencer clients 48 and 52 ("MPC Sample Controls A/B"), each with one MIDI port 0.
 */
#include <sound/asequencer.h>
#define SEQ_CARD_CLIENT(c) (16 + 4 * (c))
static const int seq_cards[2] = { 8, 9 };
struct seqport { int used; int port; char name[64]; unsigned caps, type; };
struct seqst { int fd; int client; char name[64]; struct seqport ports[8]; int nports;
               unsigned char in_sub[16]; unsigned char out_sub[16]; unsigned char in_port[16]; };
static struct seqst sq[8];
static int seq_next_client = 128;
static struct seqst *seq_of(int fd) { for (int i = 0; i < 8; i++) if (sq[i].fd == fd && sq[i].client) return &sq[i]; return 0; }
static void cpy(char *d, const char *s, int n) { int i = 0; for (; s[i] && i < n - 1; i++) d[i] = s[i]; d[i] = 0; }
static void zero(void *p, unsigned n) { char *z = p; for (unsigned i = 0; i < n; i++) z[i] = 0; }
static int card_of_client(int c) { for (int i = 0; i < 2; i++) if (SEQ_CARD_CLIENT(seq_cards[i]) == c) return seq_cards[i]; return -1; }

static int open_seq(int flags) {
    int fd = (int)syscall(SYS_socket, AF_UNIX, SOCK_SEQPACKET | ((flags & O_CLOEXEC) ? SOCK_CLOEXEC : 0), 0);
    if (fd < 0) return -1;
    struct sockaddr_un a; zero(&a, sizeof a); a.sun_family = AF_UNIX; cpy(a.sun_path, "/tmp/mpcseq.sock", sizeof a.sun_path);
    if (syscall(SYS_connect, fd, &a, sizeof a) != 0) { logs("seq open: no mpcctl helper", 0, 0); syscall(SYS_close, fd); errno = ENOENT; return -1; }
    if (flags & O_NONBLOCK) syscall(SYS_fcntl, fd, F_SETFL, O_NONBLOCK);
    for (int i = 0; i < 8; i++) if (!sq[i].client) {
        zero(&sq[i], sizeof sq[i]); sq[i].fd = fd; sq[i].client = seq_next_client++; cpy(sq[i].name, "Client", 64);
        logs("seq open client", 0, sq[i].client); return fd;
    }
    syscall(SYS_close, fd); errno = EMFILE; return -1;
}

static void fill_client(struct snd_seq_client_info *ci, int c, struct seqst *me) {
    int want = c; zero(ci, sizeof *ci); ci->client = want; ci->card = -1; ci->pid = -1;
    if (want == 0) { ci->type = KERNEL_CLIENT; cpy(ci->name, "System", 64); ci->num_ports = 2; return; }
    int card = card_of_client(want);
    if (card >= 0) { ci->type = KERNEL_CLIENT; cpy(ci->name, card == 8 ? "Control Surface" : "MPC Sample Controls B", 64); ci->num_ports = 1; ci->card = card; return; }
    for (int i = 0; i < 8; i++) if (sq[i].client == want) { ci->type = USER_CLIENT; cpy(ci->name, sq[i].name, 64); ci->num_ports = sq[i].nports; ci->pid = (int)syscall(SYS_getpid); return; }
    (void)me;
}
static int client_exists(int c) { if (c == 0 || card_of_client(c) >= 0) return 1; for (int i = 0; i < 8; i++) if (sq[i].client == c) return 1; return 0; }

static int fill_port(struct snd_seq_port_info *pi, int c, int p) {
    struct snd_seq_addr ad = { (unsigned char)c, (unsigned char)p };
    zero(pi, sizeof *pi); pi->addr = ad;
    if (c == 0) {
        if (p > 1) return -1;
        cpy(pi->name, p == 0 ? "Timer" : "Announce", 64);
        pi->capability = SNDRV_SEQ_PORT_CAP_READ | SNDRV_SEQ_PORT_CAP_SUBS_READ | (p == 0 ? SNDRV_SEQ_PORT_CAP_WRITE | SNDRV_SEQ_PORT_CAP_SUBS_WRITE : 0);
        pi->type = 0; return 0;
    }
    int card = card_of_client(c);
    if (card >= 0) {
        if (p != 0) return -1;
        cpy(pi->name, card == 8 ? "Control Surface MIDI 1" : "MPC Sample Controls B MIDI 1", 64);
        pi->capability = SNDRV_SEQ_PORT_CAP_READ | SNDRV_SEQ_PORT_CAP_WRITE | SNDRV_SEQ_PORT_CAP_SYNC_READ | SNDRV_SEQ_PORT_CAP_SYNC_WRITE |
                         SNDRV_SEQ_PORT_CAP_DUPLEX | SNDRV_SEQ_PORT_CAP_SUBS_READ | SNDRV_SEQ_PORT_CAP_SUBS_WRITE;
        pi->type = SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC | SNDRV_SEQ_PORT_TYPE_HARDWARE | SNDRV_SEQ_PORT_TYPE_PORT;
        pi->midi_channels = 16; return 0;
    }
    for (int i = 0; i < 8; i++) if (sq[i].client == c)
        for (int k = 0; k < sq[i].nports; k++) if (sq[i].ports[k].port == p) {
            cpy(pi->name, sq[i].ports[k].name, 64); pi->capability = sq[i].ports[k].caps; pi->type = sq[i].ports[k].type; return 0; }
    return -1;
}
static int next_port(int c, int after) {
    if (c == 0) return after < 0 ? 0 : (after == 0 ? 1 : -1);
    if (card_of_client(c) >= 0) return after < 0 ? 0 : -1;
    int best = -1;
    for (int i = 0; i < 8; i++) if (sq[i].client == c)
        for (int k = 0; k < sq[i].nports; k++) { int p = sq[i].ports[k].port; if (p > after && (best < 0 || p < best)) best = p; }
    return best;
}

static int seq_ioctl(struct seqst *s, unsigned long req, void *arg) {
    switch (req) {
    case SNDRV_SEQ_IOCTL_PVERSION: *(int *)arg = SNDRV_SEQ_VERSION; return 0;
    case SNDRV_SEQ_IOCTL_USER_PVERSION: return 0;
    case SNDRV_SEQ_IOCTL_CLIENT_ID: *(int *)arg = s->client; return 0;
    case SNDRV_SEQ_IOCTL_RUNNING_MODE: return 0;
    case SNDRV_SEQ_IOCTL_SYSTEM_INFO: { struct snd_seq_system_info *si = arg; zero(si, sizeof *si);
        si->queues = 32; si->clients = 192; si->ports = 254; si->channels = 256; si->cur_clients = 3; si->cur_queues = 0; return 0; }
    case SNDRV_SEQ_IOCTL_GET_CLIENT_INFO: { struct snd_seq_client_info *ci = arg; int c = ci->client;
        if (!client_exists(c)) { errno = ENOENT; return -1; } fill_client(ci, c, s); return 0; }
    case SNDRV_SEQ_IOCTL_SET_CLIENT_INFO: { struct snd_seq_client_info *ci = arg; cpy(s->name, ci->name, 64); logs("seq client name", s->name, s->client); return 0; }
    case SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT: { struct snd_seq_client_info *ci = arg; int after = ci->client, best = -1;
        int list[12], n = 0; list[n++] = 0; list[n++] = SEQ_CARD_CLIENT(8); list[n++] = SEQ_CARD_CLIENT(9);
        for (int i = 0; i < 8; i++) if (sq[i].client) list[n++] = sq[i].client;
        for (int i = 0; i < n; i++) if (list[i] > after && (best < 0 || list[i] < best)) best = list[i];
        if (best < 0) { errno = ENOENT; return -1; } fill_client(ci, best, s); return 0; }
    case SNDRV_SEQ_IOCTL_CREATE_PORT: { struct snd_seq_port_info *pi = arg;
        if (s->nports >= 8) { errno = ENOMEM; return -1; }
        struct seqport *p = &s->ports[s->nports++]; p->used = 1;
        p->port = (pi->flags & SNDRV_SEQ_PORT_FLG_GIVEN_PORT) ? pi->addr.port : s->nports - 1;
        cpy(p->name, pi->name, 64); p->caps = pi->capability; p->type = pi->type;
        pi->addr.client = s->client; pi->addr.port = p->port; logs("seq create port", p->name, p->port); return 0; }
    case SNDRV_SEQ_IOCTL_DELETE_PORT: case SNDRV_SEQ_IOCTL_SET_PORT_INFO: return 0;
    case SNDRV_SEQ_IOCTL_GET_PORT_INFO: { struct snd_seq_port_info *pi = arg; int c = pi->addr.client, p = pi->addr.port;
        if (fill_port(pi, c, p) != 0) { errno = ENOENT; return -1; } return 0; }
    case SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT: { struct snd_seq_port_info *pi = arg; int c = pi->addr.client;
        int after = pi->addr.port; if (after == 255) after = -1;   /* alsa-lib starts with port -1 stored in an unsigned char */
        int p = next_port(c, after); if (p < 0 || fill_port(pi, c, p) != 0) { errno = ENOENT; return -1; } return 0; }
    case SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT: case SNDRV_SEQ_IOCTL_UNSUBSCRIBE_PORT: {
        struct snd_seq_port_subscribe *sb = arg; int on = req == SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT;
        int sc = card_of_client(sb->sender.client), dc = card_of_client(sb->dest.client);
        if (sc >= 0 && sb->dest.client == s->client) { s->in_sub[sc] = on; s->in_port[sc] = sb->dest.port; logs(on ? "seq subscribe input from card" : "seq unsubscribe input card", 0, sc); }
        if (dc >= 0 && sb->sender.client == s->client) { s->out_sub[dc] = on; logs(on ? "seq subscribe output to card" : "seq unsubscribe output card", 0, dc); }
        return 0; }
    case SNDRV_SEQ_IOCTL_QUERY_SUBS: case SNDRV_SEQ_IOCTL_GET_SUBSCRIPTION: errno = ENOENT; return -1;
    case SNDRV_SEQ_IOCTL_CREATE_QUEUE: { struct snd_seq_queue_info *q = arg; q->queue = 0; return 0; }
    default: return 0;   /* pools, queues, timers, event removal: accept */
    }
}

/* MIDI bytes -> one sequencer event (returns bytes written into out, 0 if not convertible) */
static int midi_to_event(const unsigned char *m, int n, int card, struct seqst *s, char *out, int room) {
    if (n < 1) return 0;
    struct snd_seq_event e; zero(&e, sizeof e);
    e.queue = SNDRV_SEQ_QUEUE_DIRECT; e.source.client = SEQ_CARD_CLIENT(card); e.source.port = 0;
    e.dest.client = s->client; e.dest.port = s->in_port[card & 15];
    unsigned st = m[0] & 0xF0, ch = m[0] & 0x0F;
    if (m[0] == 0xF0) {
        int pad = ((n + (int)sizeof e - 1) / (int)sizeof e) * (int)sizeof e;
        if ((int)sizeof e + pad > room) return 0;
        e.type = SNDRV_SEQ_EVENT_SYSEX; e.flags = SNDRV_SEQ_EVENT_LENGTH_VARIABLE; e.data.ext.len = n; e.data.ext.ptr = 0;
        char *d = out; char *src = (char *)&e; for (unsigned i = 0; i < sizeof e; i++) d[i] = src[i];
        for (int i = 0; i < pad; i++) d[sizeof e + i] = i < n ? m[i] : 0;
        return (int)sizeof e + pad;
    }
    if ((st == 0x90 || st == 0x80 || st == 0xA0) && n >= 3) {
        e.type = st == 0xA0 ? SNDRV_SEQ_EVENT_KEYPRESS : (st == 0x90 && m[2] ? SNDRV_SEQ_EVENT_NOTEON : SNDRV_SEQ_EVENT_NOTEOFF);
        e.data.note.channel = ch; e.data.note.note = m[1]; e.data.note.velocity = m[2];
    } else if (st == 0xB0 && n >= 3) { e.type = SNDRV_SEQ_EVENT_CONTROLLER; e.data.control.channel = ch; e.data.control.param = m[1]; e.data.control.value = m[2]; }
    else if (st == 0xC0 && n >= 2) { e.type = SNDRV_SEQ_EVENT_PGMCHANGE; e.data.control.channel = ch; e.data.control.value = m[1]; }
    else if (st == 0xD0 && n >= 2) { e.type = SNDRV_SEQ_EVENT_CHANPRESS; e.data.control.channel = ch; e.data.control.value = m[1]; }
    else if (st == 0xE0 && n >= 3) { e.type = SNDRV_SEQ_EVENT_PITCHBEND; e.data.control.channel = ch; e.data.control.value = (m[1] | (m[2] << 7)) - 8192; }
    else return 0;
    if ((int)sizeof e > room) return 0;
    char *src = (char *)&e; for (unsigned i = 0; i < sizeof e; i++) out[i] = src[i];
    return (int)sizeof e;
}

/* The helper's end of a sequencer connection closed: connect again on the same fd number. */
static void seq_reconnect(struct seqst *s) {
    long fl = syscall(SYS_fcntl, s->fd, F_GETFL, 0), fdfl = syscall(SYS_fcntl, s->fd, F_GETFD, 0);
    int nfd = (int)syscall(SYS_socket, AF_UNIX, SOCK_SEQPACKET, 0);
    if (nfd < 0) return;
    struct sockaddr_un a; zero(&a, sizeof a); a.sun_family = AF_UNIX; cpy(a.sun_path, "/tmp/mpcseq.sock", sizeof a.sun_path);
    if (syscall(SYS_connect, nfd, &a, sizeof a) == 0) {
        syscall(SYS_dup3, nfd, s->fd, (fdfl > 0 && (fdfl & FD_CLOEXEC)) ? O_CLOEXEC : 0);
        if (fl > 0) syscall(SYS_fcntl, s->fd, F_SETFL, fl);
        logs("seq: helper connection lost, reconnected client", 0, s->client);
    } else {
        struct timespec ts = { 0, 200000000 }; syscall(SYS_nanosleep, &ts, 0);   /* helper gone: don't spin */
    }
    syscall(SYS_close, nfd);
}

static long seq_read(struct seqst *s, void *buf, unsigned long len) {
    unsigned char rec[1100];
    for (;;) {
        long r = syscall(SYS_recvfrom, s->fd, rec, sizeof rec, 0, 0, 0);
        /* A real sequencer never returns 0 bytes, and ALSA crashes on it: report "no data" instead. */
        if (r == 0) { seq_reconnect(s); errno = EAGAIN; return -1; }
        if (r < 0) return r;
        int card = rec[0];
        if (card < 0 || card > 15 || !s->in_sub[card]) continue;            /* not subscribed: dropped, like the kernel */
        int w = midi_to_event(rec + 1, (int)r - 1, card, s, buf, (int)len);
        if (w > 0) return w;
    }
}

static long seq_write(struct seqst *s, const void *buf, unsigned long len) {
    const char *p = buf; unsigned long off = 0;
    while (off + sizeof(struct snd_seq_event) <= len) {
        struct snd_seq_event e; char *d = (char *)&e; for (unsigned i = 0; i < sizeof e; i++) d[i] = p[off + i];
        unsigned long elen = sizeof e; unsigned extlen = 0;
        if ((e.flags & SNDRV_SEQ_EVENT_LENGTH_MASK) == SNDRV_SEQ_EVENT_LENGTH_VARIABLE) { extlen = e.data.ext.len & 0x3fffffffu; elen += extlen; }
        unsigned char m[1100]; int n = 0; unsigned ch = e.data.note.channel & 15;
        switch (e.type) {
        case SNDRV_SEQ_EVENT_NOTEON: m[0] = 0x90 | ch; m[1] = e.data.note.note; m[2] = e.data.note.velocity; n = 3; break;
        case SNDRV_SEQ_EVENT_NOTEOFF: m[0] = 0x80 | ch; m[1] = e.data.note.note; m[2] = e.data.note.velocity; n = 3; break;
        case SNDRV_SEQ_EVENT_KEYPRESS: m[0] = 0xA0 | ch; m[1] = e.data.note.note; m[2] = e.data.note.velocity; n = 3; break;
        case SNDRV_SEQ_EVENT_CONTROLLER: m[0] = 0xB0 | (e.data.control.channel & 15); m[1] = e.data.control.param & 127; m[2] = e.data.control.value & 127; n = 3; break;
        case SNDRV_SEQ_EVENT_PGMCHANGE: m[0] = 0xC0 | (e.data.control.channel & 15); m[1] = e.data.control.value & 127; n = 2; break;
        case SNDRV_SEQ_EVENT_CHANPRESS: m[0] = 0xD0 | (e.data.control.channel & 15); m[1] = e.data.control.value & 127; n = 2; break;
        case SNDRV_SEQ_EVENT_PITCHBEND: { int v = e.data.control.value + 8192; m[0] = 0xE0 | (e.data.control.channel & 15); m[1] = v & 127; m[2] = (v >> 7) & 127; n = 3; break; }
        case SNDRV_SEQ_EVENT_SYSEX: if (extlen && extlen < sizeof m) { for (unsigned i = 0; i < extlen && off + sizeof e + i < len; i++) m[i] = p[off + sizeof e + i]; n = extlen; } break;
        }
        if (n) {
            int targets[2], nt = 0, dc = card_of_client(e.dest.client);
            if (dc >= 0) targets[nt++] = dc;
            else if (e.dest.client == SNDRV_SEQ_ADDRESS_SUBSCRIBERS) { for (int i = 0; i < 2; i++) if (s->out_sub[seq_cards[i]]) targets[nt++] = seq_cards[i]; }
            for (int t = 0; t < nt; t++) {
                unsigned char rec[1101]; rec[0] = targets[t]; for (int i = 0; i < n; i++) rec[1 + i] = m[i];
                syscall(SYS_sendto, s->fd, rec, n + 1, MSG_DONTWAIT, 0, 0);
            }
        }
        off += elen;
    }
    return (long)len;
}

static long midi_tread(int k, void *buf, size_t len) {
    const size_t FS = sizeof(struct snd_rawmidi_framing_tstamp);
    if (len < FS) { errno = EINVAL; return -1; }
    size_t nf = len / FS; if (nf > 64) nf = 64;
    unsigned char raw[64 * 16];
    long r = syscall(SYS_recvfrom, fk[k].fd, raw, nf * 16, 0, 0, 0);
    if (r <= 0) return r;
    int clk = 0; unsigned c = fk[k].mode & SNDRV_RAWMIDI_MODE_CLOCK_MASK;
    if (c == SNDRV_RAWMIDI_MODE_CLOCK_MONOTONIC) clk = 1; else if (c == SNDRV_RAWMIDI_MODE_CLOCK_MONOTONIC_RAW) clk = 4;
    struct { long sec, nsec; } ts = {0, 0};
    if (c != SNDRV_RAWMIDI_MODE_CLOCK_NONE) syscall(SYS_clock_gettime, clk, &ts);
    for (long i = 0; i + 2 < r; i++)   /* TAP TEMPO (note 53) released: tell the panel to hold the screen briefly */
        if (raw[i + 1] == 53 && ((raw[i] == 0x80) || (raw[i] == 0x90 && raw[i + 2] == 0))) panel_tap_release();
    char *out = buf; long off = 0, w = 0;
    while (off < r) {
        struct snd_rawmidi_framing_tstamp f; zero(&f, sizeof f);
        int n = (int)(r - off); if (n > 16) n = 16;
        f.frame_type = 0; f.length = n; f.tv_sec = ts.sec; f.tv_nsec = ts.nsec;
        for (int i = 0; i < n; i++) f.data[i] = raw[off + i];
        char *src = (char *)&f; for (size_t i = 0; i < FS; i++) out[w + i] = src[i];
        off += n; w += FS;
    }
    return w;
}

ssize_t read(int fd, void *buf, size_t len) {
    struct seqst *s = seq_of(fd);
    if (s) return seq_read(s, buf, len);
    { int k = fake_find(fd); if (k >= 0 && fk[k].kind == F_MIDI && (fk[k].mode & SNDRV_RAWMIDI_MODE_FRAMING_MASK) == SNDRV_RAWMIDI_MODE_FRAMING_TSTAMP) return midi_tread(k, buf, len); }
    return syscall(SYS_read, fd, buf, len);
}
ssize_t __read_chk(int fd, void *buf, size_t len, size_t blen) { (void)blen; return read(fd, buf, len); }
void panel_midi_out(const unsigned char *b, unsigned long n);   /* panel: pad and button lights */
int panel_fb_present(const unsigned char *frame);               /* panel: show a /dev/fb0 frame on the display */
ssize_t write(int fd, const void *buf, size_t len) {
    struct seqst *s = seq_of(fd);
    if (s) return seq_write(s, buf, len);
    { int k = fake_find(fd); if (k >= 0 && fk[k].kind == F_MIDI && fk[k].card == 8) panel_midi_out(buf, len); }
    ssize_t r = syscall(SYS_write, fd, buf, len);
    if (fd == fbfd && r > 0) {   /* a program drawing straight to /dev/fb0 (SampleBoot, SampQOL splash) */
        static long acc; acc += r;
        if (acc >= (long)FB_W * FB_H * (BPP / 8)) {   /* one full 320x240 BGRX frame has been written */
            static unsigned char fbframe[FB_W * FB_H * (BPP / 8)];
            acc = 0;
            if (syscall(SYS_pread64, fd, fbframe, (long)sizeof fbframe, 0) == (long)sizeof fbframe)
                panel_fb_present(fbframe);
        }
    }
    return r;
}

/* ---- sound output: the phone can't keep up with the MPC's 128-sample (3 ms) blocks, so the add-on
 * raises the smallest block size the sound card offers. MPC_AUDIO_PERIOD (default 512) sets it; 128 = off. */
static int pcmp[16]; static int npcmp;
static unsigned min_period = 0;
static int is_play_pcm(const char *p) {
    const char *pre = "/dev/snd/pcmC"; int i = 0;
    for (; pre[i]; i++) if (p[i] != pre[i]) return 0;
    while (p[i]) i++;
    return p[i - 1] == 'p';
}
static int is_pcmp(int fd) { for (int i = 0; i < npcmp; i++) if (pcmp[i] == fd) return 1; return 0; }
static void pcm_constrain(struct snd_pcm_hw_params *hp, int final) {
    if (!min_period) {
        const char *e = getenv("MPC_AUDIO_PERIOD"); unsigned v = 0;
        if (e) for (; *e >= '0' && *e <= '9'; e++) v = v * 10 + (*e - '0');
        min_period = v ? v : 512;
    }
    if (min_period <= 128) return;
    struct snd_interval *ps = &hp->intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    struct snd_interval *bs = &hp->intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    if (ps->min < min_period) { ps->min = min_period; ps->openmin = 0; hp->rmask |= 1u << SNDRV_PCM_HW_PARAM_PERIOD_SIZE; }
    if (bs->min < 4 * min_period) { bs->min = 4 * min_period; bs->openmin = 0; hp->rmask |= 1u << SNDRV_PCM_HW_PARAM_BUFFER_SIZE; }
    if (final) logs("audio output block size at least", 0, min_period);
}

/* ---- 16-bit output: the phone's sound card plays 32-bit audio as noise (tonetest: 16-bit clean, 32-bit
 * noise). The program keeps seeing the 32-bit, 4-channel card it expects; the real card is set to 16-bit
 * stereo, and every block the program writes is converted on the way (channels 1-2, top 16 bits).
 * Frame counts are the same on both sides, so positions, delays and timing need no translation.
 * MPC_AUDIO16=0 turns this off. */
#define IV(hp, p) (&(hp)->intervals[(p) - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL])
#define MK(hp, p) (&(hp)->masks[(p) - SNDRV_PCM_HW_PARAM_FIRST_MASK])
static int a16 = -1;
static int audio16(void) {
    if (a16 < 0) { const char *e = getenv("MPC_AUDIO16"); a16 = !(e && e[0] == '0'); }
    return a16;
}
static int mask_has(struct snd_mask *m, unsigned b) { return (m->bits[b >> 5] >> (b & 31)) & 1; }
static void mask_only(struct snd_mask *m, unsigned b) { for (int i = 0; i < 8; i++) m->bits[i] = 0; m->bits[b >> 5] = 1u << (b & 31); }
static void iv_set(struct snd_interval *i, unsigned v) { i->min = i->max = v; i->openmin = i->openmax = 0; i->integer = 1; i->empty = 0; }
static void iv_scale(struct snd_interval *i, int up) {   /* bytes: program side = 4 x card side */
    if (up) { if (i->min <= 0x3fffffffu) i->min *= 4; else i->min = 0xffffffffu; if (i->max <= 0x3fffffffu) i->max *= 4; else i->max = 0xffffffffu; }
    else { i->min /= 4; if (i->max != 0xffffffffu) i->max = (i->max + 3) / 4; }
}
/* program's view -> card's view. Returns 1 if this request is converted. */
static int a16_down(struct snd_pcm_hw_params *hp) {
    if (!audio16() || !mask_has(MK(hp, SNDRV_PCM_HW_PARAM_FORMAT), SNDRV_PCM_FORMAT_S32_LE)) return 0;
    struct snd_interval *ch = IV(hp, SNDRV_PCM_HW_PARAM_CHANNELS);
    if (ch->min != 4 || ch->max != 4) return 0;     /* only the program's final 4-channel 32-bit setting */
    struct snd_mask *fm = MK(hp, SNDRV_PCM_HW_PARAM_FORMAT);
    for (int i = 0; i < 8; i++) if (fm->bits[i] != (i == (SNDRV_PCM_FORMAT_S32_LE >> 5) ? 1u << (SNDRV_PCM_FORMAT_S32_LE & 31) : 0u)) return 0;
    mask_only(MK(hp, SNDRV_PCM_HW_PARAM_FORMAT), SNDRV_PCM_FORMAT_S16_LE);
    iv_set(ch, 2);
    iv_set(IV(hp, SNDRV_PCM_HW_PARAM_SAMPLE_BITS), 16);
    iv_set(IV(hp, SNDRV_PCM_HW_PARAM_FRAME_BITS), 32);
    iv_scale(IV(hp, SNDRV_PCM_HW_PARAM_PERIOD_BYTES), 0);
    iv_scale(IV(hp, SNDRV_PCM_HW_PARAM_BUFFER_BYTES), 0);
    hp->rmask |= (1u << SNDRV_PCM_HW_PARAM_FORMAT) | (1u << SNDRV_PCM_HW_PARAM_CHANNELS) | (1u << SNDRV_PCM_HW_PARAM_SAMPLE_BITS)
               | (1u << SNDRV_PCM_HW_PARAM_FRAME_BITS) | (1u << SNDRV_PCM_HW_PARAM_PERIOD_BYTES) | (1u << SNDRV_PCM_HW_PARAM_BUFFER_BYTES);
    return 1;
}
/* card's view -> program's view */
static void a16_up(struct snd_pcm_hw_params *hp) {
    struct snd_mask *f = MK(hp, SNDRV_PCM_HW_PARAM_FORMAT);
    if (mask_has(f, SNDRV_PCM_FORMAT_S16_LE)) mask_only(f, SNDRV_PCM_FORMAT_S32_LE);
    iv_set(IV(hp, SNDRV_PCM_HW_PARAM_CHANNELS), 4);
    iv_set(IV(hp, SNDRV_PCM_HW_PARAM_SAMPLE_BITS), 32);
    iv_set(IV(hp, SNDRV_PCM_HW_PARAM_FRAME_BITS), 128);
    iv_scale(IV(hp, SNDRV_PCM_HW_PARAM_PERIOD_BYTES), 1);
    iv_scale(IV(hp, SNDRV_PCM_HW_PARAM_BUFFER_BYTES), 1);
    if (hp->msbits == 16) hp->msbits = 32;
}
static int conv_fd[16]; static int nconv;
static int is_conv(int fd) { for (int i = 0; i < nconv; i++) if (conv_fd[i] == fd) return 1; return 0; }
static void set_conv(int fd, int on) {
    for (int i = 0; i < nconv; i++) if (conv_fd[i] == fd) { if (!on) conv_fd[i] = -1; return; }
    if (on) { for (int i = 0; i < nconv; i++) if (conv_fd[i] < 0) { conv_fd[i] = fd; return; } if (nconv < 16) conv_fd[nconv++] = fd; }
}
static int16_t *cbuf; static unsigned long cframes;
/* ---- recorder for checking the sound: audiorec.sh creates /tmp/mpc-rec; the next 10 s the program writes
 * (4 x 32-bit, before conversion) go to /tmp/mpc-rec.raw, then the flag file is removed. */
static unsigned audio_rate;
static int rec_fd = -1; static unsigned long rec_left; static int rec_tick;
static void rec_block(const int32_t *in, unsigned long frames) {
    if (rec_fd < 0) {
        if (++rec_tick < 20) return;   /* look for the flag about 5 times a second */
        rec_tick = 0;
        struct stat st; if (syscall(SYS_newfstatat, AT_FDCWD, "/tmp/mpc-rec", &st, 0) != 0) return;
        rec_fd = (int)syscall(SYS_openat, AT_FDCWD, "/tmp/mpc-rec.raw", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (rec_fd < 0) return;
        uint32_t hdr[2] = { 0x43455243 /* "CREC" */, audio_rate ? audio_rate : 48000 };
        syscall(SYS_write, rec_fd, hdr, sizeof hdr);
        rec_left = (unsigned long)(audio_rate ? audio_rate : 48000) * 10;
        logs("audio: recording 10 s", 0, audio_rate);
    }
    unsigned long n = frames < rec_left ? frames : rec_left;
    syscall(SYS_write, rec_fd, in, n * 16);
    rec_left -= n;
    if (!rec_left) { syscall(SYS_close, rec_fd); rec_fd = -1; syscall(SYS_unlinkat, AT_FDCWD, "/tmp/mpc-rec", 0); logs("audio: recording done", 0, 0); }
}
/* WRITEI_FRAMES from the program: 4 x S32 per frame -> 2 x S16 per frame */
static long a16_write(int fd, struct snd_xferi *x) {
    const int32_t *in = x->buf; unsigned long frames = x->frames, done = 0;
    rec_block(in, frames);
    if (!cbuf) { cframes = 16384; long p = syscall(SYS_mmap, 0, cframes * 4, 3 /*RW*/, 0x22 /*PRIVATE|ANON*/, -1, 0); if (p < 0 && p > -4096) return -1; cbuf = (int16_t *)p; }
    while (done < frames) {
        unsigned long n = frames - done; if (n > cframes) n = cframes;
        const int32_t *s = in + done * 4;
        for (unsigned long i = 0; i < n; i++) {   /* L = ch1 + ch3, R = ch2 + ch4 (the MPC may use either pair) */
            long l = ((long)s[4 * i] + s[4 * i + 2]) >> 16, r = ((long)s[4 * i + 1] + s[4 * i + 3]) >> 16;
            cbuf[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            cbuf[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        struct snd_xferi k = { 0, cbuf, (snd_pcm_uframes_t)n };
        long r = syscall(SYS_ioctl, fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &k);
        if (r < 0) { if (done) break; x->result = 0; return r; }
        done += (unsigned long)k.result;
        if ((unsigned long)k.result < n) break;
    }
    x->result = (snd_pcm_sframes_t)done;
    return 0;
}

/* ---- output jacks: the MPC plays through its built-in speaker (its speaker channels, with the speaker's
 * sound) unless PHONES or AUDIO OUT is plugged in. It learns that from the sound card's "Headphones Jack" and
 * "Line Out Jack" switches, which the phone's card does not have, so they are added here, plugged in.
 * MPC_JACKS=0 leaves them out (speaker sound). */
#include <sound/asound.h>
static int jack_fds[16], njack;
static int jacks_on = -1;
static int jacks(void) { if (jacks_on < 0) { const char *e = getenv("MPC_JACKS"); jacks_on = !(e && e[0] == '0'); } return jacks_on; }
static void jack_track(int fd) {
    if (!jacks()) return;
    for (int i = 0; i < njack; i++) if (jack_fds[i] == fd || jack_fds[i] < 0) { jack_fds[i] = fd; return; }
    if (njack < 16) jack_fds[njack++] = fd;
}
static int is_jackfd(int fd) { for (int i = 0; i < njack; i++) if (jack_fds[i] == fd) return 1; return 0; }
#define NJ 3
static const struct { const char *name; int iface, rw; long val; } JK[NJ] = {
    { "Headphones Jack", SNDRV_CTL_ELEM_IFACE_CARD, 0, 1 },
    { "Line Out Jack", SNDRV_CTL_ELEM_IFACE_CARD, 0, 1 },
    { "Builtin Speaker Switch", SNDRV_CTL_ELEM_IFACE_MIXER, 1, 0 },
};
static long jack_val[NJ] = { 1, 1, 0 };
#define JNUM0 0x7000u
static int jack_of(const struct snd_ctl_elem_id *id) {
    if (id->numid) return id->numid >= JNUM0 && id->numid < JNUM0 + NJ ? (int)(id->numid - JNUM0) : -1;
    for (int j = 0; j < NJ; j++) if (str_eq((const char *)id->name, JK[j].name)) return j;
    return -1;
}
static void jack_id(struct snd_ctl_elem_id *id, int j) {
    for (unsigned i = 0; i < sizeof id->name; i++) id->name[i] = 0;
    for (int i = 0; JK[j].name[i]; i++) id->name[i] = (unsigned char)JK[j].name[i];
    id->numid = JNUM0 + j; id->iface = JK[j].iface; id->device = 0; id->subdevice = 0; id->index = 0;
}
/* returns 1 when handled (result in *ret) */
static int jack_ioctl(int fd, unsigned long req, void *arg, long *ret) {
    if (!arg || !is_jackfd(fd)) return 0;
    if (req == SNDRV_CTL_IOCTL_ELEM_LIST) {
        struct snd_ctl_elem_list *l = arg;
        long r = syscall(SYS_ioctl, fd, req, arg);
        if (r < 0) { *ret = r; return 1; }
        unsigned real = l->count;
        for (int j = 0; j < NJ; j++) {
            unsigned pos = real + (unsigned)j;
            if (l->pids && pos >= l->offset && pos - l->offset == l->used && l->used < l->space) jack_id(&l->pids[l->used++], j);
        }
        l->count = real + NJ;
        *ret = 0; return 1;
    }
    if (req == SNDRV_CTL_IOCTL_ELEM_INFO) {
        struct snd_ctl_elem_info *in = arg; int j = jack_of(&in->id);
        if (j < 0) return 0;
        for (unsigned i = 0; i < sizeof *in; i++) ((char *)in)[i] = 0;
        jack_id(&in->id, j);
        in->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
        in->access = SNDRV_CTL_ELEM_ACCESS_READ | (JK[j].rw ? SNDRV_CTL_ELEM_ACCESS_WRITE : SNDRV_CTL_ELEM_ACCESS_VOLATILE);
        in->count = 1; in->value.integer.min = 0; in->value.integer.max = 1; in->value.integer.step = 1;
        *ret = 0; return 1;
    }
    if (req == SNDRV_CTL_IOCTL_ELEM_READ) {
        struct snd_ctl_elem_value *v = arg; int j = jack_of(&v->id);
        if (j < 0) return 0;
        jack_id(&v->id, j); v->value.integer.value[0] = jack_val[j];
        *ret = 0; return 1;
    }
    if (req == SNDRV_CTL_IOCTL_ELEM_WRITE) {
        struct snd_ctl_elem_value *v = arg; int j = jack_of(&v->id);
        if (j < 0) return 0;
        if (JK[j].rw) { jack_val[j] = v->value.integer.value[0] ? 1 : 0; logs("audio: speaker switch set to", 0, (unsigned)jack_val[j]); *ret = 0; }
        else { errno = EPERM; *ret = -1; }
        return 1;
    }
    if (req == SNDRV_CTL_IOCTL_TLV_READ || req == SNDRV_CTL_IOCTL_ELEM_LOCK || req == SNDRV_CTL_IOCTL_ELEM_UNLOCK) {
        struct snd_ctl_elem_id *id = req == SNDRV_CTL_IOCTL_TLV_READ ? (struct snd_ctl_elem_id *)0 : arg;
        if (id && jack_of(id) >= 0) { *ret = 0; return 1; }
        if (!id) {   /* TLV: the element id is inside snd_ctl_tlv.numid */
            struct snd_ctl_tlv *t = arg;
            if (t->numid >= JNUM0 && t->numid < JNUM0 + NJ) { errno = ENXIO; *ret = -1; return 1; }
        }
    }
    return 0;
}

static int do_open(const char *path, int flags, int mode) {
    if (path && path[0] == '/' && path[1] == 'd') {
        int c = fake_card_of(path);
        if (c >= 0) return open_ctl(c, flags);
        c = fake_midi_of(path);
        if (c >= 0) return open_midi(c, flags);
        if (str_eq(path, "/dev/snd/seq")) return open_seq(flags);
        if (is_play_pcm(path)) {
            int fd = real_open(path, flags, mode);
            if (fd >= 0) { int i = 0; while (i < npcmp && pcmp[i] >= 0) i++; if (i < 16) { pcmp[i] = fd; if (i == npcmp) npcmp++; } }
            return fd;
        }
        if (str_eq(path, "/dev/snd/by-path/platform-sound") || str_eq(path, "/dev/snd/controlC0")) {
            if (path[9] == 'b') logs("audio: platform-sound -> controlC0", 0, 0);
            int fd = real_open("/dev/snd/controlC0", flags, mode);
            if (fd >= 0) jack_track(fd);
            return fd;
        }
    }
    if (path && is_fb(path)) {
        if (!ready) init_var();
        int fd = real_open(FBFILE, O_RDWR | O_CREAT | (flags & O_CLOEXEC), 0644);
        if (fd >= 0) {
            syscall(SYS_ftruncate, fd, (long)FB_W * FB_H * (BPP / 8) * PAGES);
            fbfd = fd;
            return fd;
        }
    }
    return real_open(path, flags, mode);
}

static void fill_rawmidi_info(struct snd_rawmidi_info *in, int card) {
    char *z = (char *)in; unsigned dev = in->device, sub = in->subdevice; int st = in->stream;
    for (unsigned i = 0; i < sizeof *in; i++) z[i] = 0;
    in->device = dev; in->subdevice = sub; in->stream = st; in->card = card;
    in->flags = SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_DUPLEX;
    const char *nm = card == 8 ? "Control Surface" : "MPC Sample Ctl B";
    for (int i = 0; nm[i]; i++) { in->id[i] = nm[i]; in->name[i] = nm[i]; in->subname[i] = nm[i]; }
    in->subdevices_count = 1; in->subdevices_avail = 1;
}

void shim_log(const char *a, const char *b, unsigned v) { logs(a, b, v); }
int panel_ioctl(int fd, unsigned long req, void *arg, long *ret);

static unsigned long seen[64]; static int nseen;
static int fake_ioctl(int k, unsigned long req, void *arg) {
    int card = fk[k].card;
    { int dup = 0; for (int i = 0; i < nseen; i++) if (seen[i] == (req ^ ((unsigned long)card << 40))) dup = 1;
      if (!dup && nseen < 64) { seen[nseen++] = req ^ ((unsigned long)card << 40);
        char h[20]; int n = 0; const char *hx = "0123456789abcdef"; for (int sh = 28; sh >= 0; sh -= 4) h[n++] = hx[(req >> sh) & 15]; h[n] = 0;
        logs(fk[k].kind == F_CTL ? "first ctl ioctl" : "first midi ioctl", h, card); } }
    if (fk[k].kind == F_CTL) {
        if (req == SNDRV_CTL_IOCTL_PVERSION) { *(int *)arg = SNDRV_PROTOCOL_VERSION(2, 0, 7); return 0; }
        if (req == SNDRV_CTL_IOCTL_CARD_INFO) {
            struct snd_ctl_card_info *ci = arg; char *z = arg; for (unsigned i = 0; i < sizeof *ci; i++) z[i] = 0;
            ci->card = card;
            const char *id = card == 8 ? "ControlSurface" : "MPCCtlB", *nm = card == 8 ? "Control Surface" : "MPC Sample Controls B";
            const char *drv = card == 8 ? "SerialMIDI" : "USB-Audio";
            for (int i = 0; id[i]; i++) ci->id[i] = id[i];
            for (int i = 0; drv[i]; i++) ci->driver[i] = drv[i];
            for (int i = 0; nm[i]; i++) { ci->name[i] = nm[i]; ci->longname[i] = nm[i]; }
            return 0;
        }
        if (req == SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE) { int *d = arg; *d = (*d < 0) ? 0 : -1; return 0; }
        if (req == SNDRV_CTL_IOCTL_RAWMIDI_INFO) {
            struct snd_rawmidi_info *in = arg;
            if (in->device != 0 || in->subdevice > 0) { errno = ENXIO; return -1; }
            fill_rawmidi_info(in, card); return 0;
        }
        if (req == SNDRV_CTL_IOCTL_RAWMIDI_PREFER_SUBDEVICE || req == SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS) return 0;
        if (req == SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE || req == SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE
#ifdef SNDRV_CTL_IOCTL_UMP_NEXT_DEVICE
            || req == SNDRV_CTL_IOCTL_UMP_NEXT_DEVICE
#endif
        ) { *(int *)arg = -1; return 0; }
        if (req == SNDRV_CTL_IOCTL_ELEM_LIST) { struct snd_ctl_elem_list *l = arg; l->used = 0; l->count = 0; return 0; }
        logs("ctl ioctl not handled card", 0, card);
        errno = ENOTTY; return -1;
    }
    if (req == SNDRV_RAWMIDI_IOCTL_PVERSION) { *(int *)arg = SNDRV_PROTOCOL_VERSION(2, 0, 4); return 0; }
    if (req == SNDRV_RAWMIDI_IOCTL_INFO) { struct snd_rawmidi_info *in = arg; in->device = 0; in->subdevice = 0; fill_rawmidi_info(in, card); return 0; }
    if (req == SNDRV_RAWMIDI_IOCTL_STATUS) { struct snd_rawmidi_status *st = arg; int sd = st->stream; char *z = arg; for (unsigned i = 0; i < sizeof *st; i++) z[i] = 0; st->stream = sd; st->avail = 4096; return 0; }
    if (req == SNDRV_RAWMIDI_IOCTL_PARAMS) { struct snd_rawmidi_params *pp = arg; if (pp->stream == SNDRV_RAWMIDI_STREAM_INPUT) { fk[k].mode = pp->mode; logs("midi input mode card", 0, card * 1000 + pp->mode); } return 0; }
    if (req == SNDRV_RAWMIDI_IOCTL_DROP || req == SNDRV_RAWMIDI_IOCTL_DRAIN) return 0;
#ifdef SNDRV_RAWMIDI_IOCTL_USER_PVERSION
    if (req == SNDRV_RAWMIDI_IOCTL_USER_PVERSION) return 0;
#endif
    logs("midi ioctl not handled card", 0, card);
    errno = ENOTTY; return -1;
}

static long tlog(const char *op, const char *path, long r);   /* the card trace (end of file) */
int open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return (int)tlog("open", path, do_open(path, flags, mode));
}
int open64(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return (int)tlog("open", path, do_open(path, flags, mode));
}

int __open_2(const char *path, int flags) { return do_open(path, flags, 0); }
int __open64_2(const char *path, int flags) { return do_open(path, flags, 0); }
int openat(int dirfd, const char *path, int flags, ...) {
    int mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if (path && path[0] == '/') return (int)tlog("open", path, do_open(path, flags, mode));
    return (int)syscall(SYS_openat, dirfd, path, flags, mode);
}
int openat64(int dirfd, const char *path, int flags, ...) {
    int mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if (path && path[0] == '/') return do_open(path, flags, mode);
    return (int)syscall(SYS_openat, dirfd, path, flags, mode);
}

int close(int fd) {
    if (fd == fbfd && fd >= 0) fbfd = -1;
    for (int i = 0; i < npcmp; i++) if (pcmp[i] == fd) pcmp[i] = -1;
    for (int i = 0; i < nconv; i++) if (conv_fd[i] == fd) conv_fd[i] = -1;
    { struct seqst *sqs = seq_of(fd); if (sqs) { logs("seq close client", 0, sqs->client); sqs->client = 0; sqs->fd = -1; } }
    int k = fake_find(fd);
    if (k >= 0) { if (fk[k].peer >= 0) syscall(SYS_close, fk[k].peer); fk[k].kind = F_NONE; }
    return (int)syscall(SYS_close, fd);
}

/* ---- DRM: show only connected outputs ---- */
#include <drm/drm.h>
#include <drm/drm_mode.h>
struct card_res { uint64_t fb_ptr, crtc_ptr, conn_ptr, enc_ptr; uint32_t n_fb, n_crtc, n_conn, n_enc, minw, maxw, minh, maxh; };
struct get_conn { uint64_t enc_ptr, modes_ptr, props_ptr, vals_ptr;
                  uint32_t n_modes, n_props, n_enc, encoder_id, connector_id, type, type_id, connection, mmw, mmh, subpixel, pad; };
#define IOC_GETRESOURCES 0xC04064A0UL
#define IOC_GETCONNECTOR 0xC05064A7UL
static int logged;

static void logline(const char *a, unsigned v1, unsigned v2) {
    int fd = real_open("/tmp/fbshim.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    char b[160]; int n = 0;
    for (const char *p = a; *p && n < 120; p++) b[n++] = *p;
    unsigned vs[2] = { v1, v2 };
    for (int k = 0; k < 2; k++) {
        char t[12]; int m = 0; unsigned v = vs[k];
        b[n++] = ' '; do { t[m++] = '0' + v % 10; v /= 10; } while (v);
        while (m) b[n++] = t[--m];
    }
    b[n++] = '\n';
    syscall(SYS_write, fd, b, n); syscall(SYS_close, fd);
}

static int connected(int fd, uint32_t id) {
    struct get_conn c; char *p = (char *)&c;
    for (unsigned i = 0; i < sizeof c; i++) p[i] = 0;
    c.connector_id = id;
    if (syscall(SYS_ioctl, fd, IOC_GETCONNECTOR, &c) != 0) return 0;
    return c.connection == 1;
}

static int getresources(int fd, struct card_res *user) {
    uint32_t all[64], keep[64];
    struct card_res r = *user;
    r.conn_ptr = (uint64_t)(uintptr_t)all; r.n_conn = 64;
    long rc = syscall(SYS_ioctl, fd, IOC_GETRESOURCES, &r);
    if (rc != 0 || r.n_conn > 64) return (int)syscall(SYS_ioctl, fd, IOC_GETRESOURCES, user);
    uint32_t n = 0;
    for (uint32_t i = 0; i < r.n_conn; i++) if (connected(fd, all[i])) keep[n++] = all[i];
    if (n == 0) { for (uint32_t i = 0; i < r.n_conn; i++) keep[i] = all[i]; n = r.n_conn; }
    uint64_t up = user->conn_ptr; uint32_t ucount = user->n_conn;
    *user = r; user->conn_ptr = up; user->n_conn = n;
    if (up && ucount >= n) { uint32_t *dst = (uint32_t *)(uintptr_t)up; for (uint32_t i = 0; i < n; i++) dst[i] = keep[i]; }
    if (!logged && up) { logged = 1; logline("outputs: total, shown first id", r.n_conn, keep[0]); }
    return 0;
}

/* Apps written for the MPC itself (Melodee's sample-base, ...) look for the MPC's 320 x 240 panel among the display's modes.
 * The phone's display has only its window size, so other programs than the stock app also see a 320 x 240 mode; the
 * panel turns their picture of that size into the MPC screen as it does the stock app's (the stock app is left as it is). */
static int panel_mode_wanted(void) {
    static int w = -1;
    if (w < 0) {
        extern char *program_invocation_short_name;
        const char *e = getenv("MPC_PANEL");
        w = !(e && e[0] == '0') && !str_eq(program_invocation_short_name, "MPC Sample");
    }
    return w;
}
static int getconnector(int fd, struct get_conn *c) {
    uint32_t room = c->n_modes; uint64_t up = c->modes_ptr;
    long r = syscall(SYS_ioctl, fd, IOC_GETCONNECTOR, c);
    if (r != 0 || c->connection != 1 || !panel_mode_wanted()) return (int)r;
    uint32_t n = c->n_modes;
    if (up && room >= n + 1) {
        struct drm_mode_modeinfo m; char *z = (char *)&m; for (unsigned i = 0; i < sizeof m; i++) z[i] = 0;
        m.clock = 6400; m.hdisplay = 320; m.hsync_start = 336; m.hsync_end = 352; m.htotal = 400;
        m.vdisplay = 240; m.vsync_start = 244; m.vsync_end = 248; m.vtotal = 266; m.vrefresh = 60;
        m.type = DRM_MODE_TYPE_DRIVER; cpy(m.name, "320x240", sizeof m.name);
        ((struct drm_mode_modeinfo *)(uintptr_t)up)[n] = m;
    }
    c->n_modes = n + 1;
    return 0;
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    { struct seqst *sqs = seq_of(fd); if (sqs) return seq_ioctl(sqs, req, arg); }
    int k = fake_find(fd);
    if (k >= 0) return fake_ioctl(k, req, arg);
    if (req == IOC_GETRESOURCES && arg) return getresources(fd, arg);
    if (req == IOC_GETCONNECTOR && arg) return getconnector(fd, arg);
    if (req == DRM_IO(0x1e)) {   /* SET_MASTER: the boot-menu helper (devrelay) may still be handing the display over */
        long r;
        for (int i = 0; (r = syscall(SYS_ioctl, fd, req, arg)) != 0 && i < 60; i++) { struct timespec t = { 0, 50000000 }; syscall(SYS_nanosleep, &t, 0); }
        return (int)r;
    }
    if (((req >> 8) & 0xff) == 'd') { long r; if (panel_ioctl(fd, req, arg, &r)) return (int)r; }
    if (((req >> 8) & 0xff) == 'U') { long r; if (jack_ioctl(fd, req, arg, &r)) return (int)r; }
    if ((req == SNDRV_PCM_IOCTL_HW_REFINE || req == SNDRV_PCM_IOCTL_HW_PARAMS) && arg && is_pcmp(fd)) {
        /* convert only the final settings: the program's capability questions (HW_REFINE) see the real card */
        int conv = req == SNDRV_PCM_IOCTL_HW_PARAMS ? a16_down(arg) : 0;
        pcm_constrain(arg, req == SNDRV_PCM_IOCTL_HW_PARAMS);
        long r = syscall(SYS_ioctl, fd, req, arg);
        if (conv) { int e = errno; a16_up(arg); errno = e; }
        if (req == SNDRV_PCM_IOCTL_HW_PARAMS && r >= 0) {
            set_conv(fd, conv); if (conv) logs("audio: program 32-bit 4ch -> card 16-bit stereo", 0, 0);
            struct snd_interval *rt = IV((struct snd_pcm_hw_params *)arg, SNDRV_PCM_HW_PARAM_RATE);
            audio_rate = rt->min; logs("audio: sample rate", 0, rt->min);
        }
        return (int)r;
    }
    if (req == SNDRV_PCM_IOCTL_WRITEI_FRAMES && arg && is_conv(fd)) return (int)a16_write(fd, arg);
    if (req == SNDRV_PCM_IOCTL_HW_FREE && is_conv(fd)) set_conv(fd, 0);
    if (fd != fbfd || fd < 0) return (int)syscall(SYS_ioctl, fd, req, arg);
    if (req == FBIOGET_VSCREENINFO) {
        char *d = arg, *s = (char *)&var;
        for (unsigned i = 0; i < sizeof var; i++) d[i] = s[i];
        return 0;
    }
    if (req == FBIOPUT_VSCREENINFO || req == FBIOPAN_DISPLAY) {
        struct fb_var_screeninfo *v = arg;
        if (v->yoffset <= (unsigned)FB_H * (PAGES - 1)) { var.xoffset = 0; var.yoffset = v->yoffset; }
        if (req == FBIOPUT_VSCREENINFO) {
            char *d = arg, *s = (char *)&var;
            for (unsigned i = 0; i < sizeof var; i++) d[i] = s[i];
        }
        return 0;
    }
    if (req == FBIOGET_FSCREENINFO) {
        struct fb_fix_screeninfo *f = arg;
        char *d = arg;
        for (unsigned i = 0; i < sizeof *f; i++) d[i] = 0;
        const char id[] = "kit-fb";
        for (unsigned i = 0; i < sizeof id && i < sizeof f->id; i++) f->id[i] = id[i];
        f->smem_len = FB_W * FB_H * (BPP / 8) * PAGES;
        f->type = FB_TYPE_PACKED_PIXELS;
        f->visual = FB_VISUAL_TRUECOLOR;
        f->ypanstep = 1;
        f->line_length = FB_W * (BPP / 8);
        return 0;
    }
    return 0;  /* FBIOBLANK, FBIO_WAITFORVSYNC and anything else: pretend it worked */
}

/* ---------- external drive ----------
 * ./mpc run mounts the phone's MPC-Sample folder at /media/az01-internal-sd, the MPC's SD card place. The program
 * unmounts and removes drives there in some modes (SD Card Access); keep the phone folder mounted instead. */
static int is_sd(const char *p) {
    static const char k[] = "az01-internal-sd";
    if (!p) return 0;
    for (const char *s = p; *s; s++) {
        int i = 0; while (k[i] && s[i] == k[i]) i++;
        if (!k[i] && (s[i] == 0 || (s[i] == '/' && s[i + 1] == 0))) return 1;
    }
    return 0;
}
int umount2(const char *t, int f) {
    if (is_sd(t)) { logs("external drive: kept mounted (umount2)", t, 0); return 0; }
    return (int)syscall(SYS_umount2, t, f);
}
int umount(const char *t) {
    if (is_sd(t)) { logs("external drive: kept mounted (umount)", t, 0); return 0; }
    return (int)syscall(SYS_umount2, t, 0);
}
int remove(const char *p) {
    if (is_sd(p)) { logs("external drive: not removed", p, 0); return 0; }
    if (syscall(SYS_unlinkat, AT_FDCWD, p, 0) == 0) return 0;   /* what libc's remove() does */
    if (errno != EISDIR && errno != EPERM) return -1;
    return (int)syscall(SYS_unlinkat, AT_FDCWD, p, AT_REMOVEDIR);
}

/* ---- the microSD card's filesystem type: report FAT, like a real card ----
 * The kit's card root is a tmpfs with your phone folders mapped onto it (see ./mpc run). The MPC asks statfs()
 * what kind of disk a path is on: JUCE counts FAT as removable media and anything else as a hard disk, so a
 * card that isn't FAT is not offered as the SD card (the Save page says "No SD card"). Paths on the card
 * report FAT (MSDOS_SUPER_MAGIC); the first calls are logged with the real type, to see what the MPC asks. */
#include <sys/vfs.h>
static const char CARDP[] = "/media/az01-internal-sd";
static int on_card(const char *p) {
    unsigned n = 0;
    if (!p) return 0;
    while (CARDP[n]) { if (p[n] != CARDP[n]) return 0; n++; }
    return p[n] == 0 || p[n] == '/';
}
static void card_fs(const char *p, struct statfs *b) {
    static int nlog;
    if (nlog < 40) { nlog++; shim_log("statfs on the card (real type -> reported FAT)", p, (unsigned)b->f_type); }
    b->f_type = 0x4d44;   /* MSDOS_SUPER_MAGIC */
}
int statfs(const char *p, struct statfs *b) {
    int r = (int)syscall(SYS_statfs, p, b);
    if (r == 0 && on_card(p)) card_fs(p, b);
    return r;
}
int fstatfs(int fd, struct statfs *b) {
    int r = (int)syscall(SYS_fstatfs, fd, b);
    if (r == 0 && fd >= 0) {
        char l[32] = "/proc/self/fd/", t[512];
        int i = 14, d = fd, n = 0; char rev[12];
        do { rev[n++] = (char)('0' + d % 10); d /= 10; } while (d && n < 11);
        while (n) l[i++] = rev[--n];
        l[i] = 0;
        long k = syscall(SYS_readlinkat, AT_FDCWD, l, t, sizeof t - 1);
        if (k > 0) { t[k] = 0; if (on_card(t)) card_fs(t, b); }
    }
    return r;
}
/* the 64-bit names: the same struct layout on ARM64 */
int statfs64(const char *p, struct statfs64 *b) { return statfs(p, (struct statfs *)b); }
int fstatfs64(int fd, struct statfs64 *b) { return fstatfs(fd, (struct statfs *)b); }

/* ---- card trace: what does the MPC check on the card? ----
 * Off unless /tmp/cardtrace exists in the MPC's system (touch it, open the page in question, remove it).
 * Logs every check of a path under /media/ - open, stat, lstat, access, statvfs, opendir, mkdir - with its
 * result (0 or a descriptor = ok, 1000+errno = failed) to /tmp/fbshim.log, as "trace <call> <path> <result>". */
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
static int tracing(void) {
    static long next; static int on;
    struct timespec ts; syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    long now = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
    if (now >= next) { struct stat st; on = syscall(SYS_newfstatat, AT_FDCWD, "/tmp/cardtrace", &st, 0) == 0; next = now + 500; }
    return on;
}
static long tlog(const char *op, const char *path, long r) {
    if (path && path[0] == '/' && path[1] == 'm' && path[2] == 'e' && path[3] == 'd' && path[4] == 'i' && path[5] == 'a' && tracing()) {
        int e = errno; char o[48] = "trace "; int n = 6; while (*op && n < 46) o[n++] = *op++; o[n] = 0;
        logs(o, path, r < 0 ? (unsigned)(1000 + e) : (unsigned)r); errno = e;
    }
    return r;
}
int stat(const char *p, struct stat *b) { return (int)tlog("stat", p, syscall(SYS_newfstatat, AT_FDCWD, p, b, 0)); }
int lstat(const char *p, struct stat *b) { return (int)tlog("lstat", p, syscall(SYS_newfstatat, AT_FDCWD, p, b, AT_SYMLINK_NOFOLLOW)); }
int stat64(const char *p, struct stat64 *b) { return stat(p, (struct stat *)b); }
int lstat64(const char *p, struct stat64 *b) { return lstat(p, (struct stat *)b); }
int fstatat(int d, const char *p, struct stat *b, int f) { long r = syscall(SYS_newfstatat, d, p, b, f); return (int)(d == AT_FDCWD ? tlog("stat", p, r) : r); }
int fstatat64(int d, const char *p, struct stat64 *b, int f) { return fstatat(d, p, (struct stat *)b, f); }
int access(const char *p, int m) { return (int)tlog(m & W_OK ? "access-write" : "access", p, syscall(SYS_faccessat, AT_FDCWD, p, m)); }
int mkdir(const char *p, mode_t m) { return (int)tlog("mkdir", p, syscall(SYS_mkdirat, AT_FDCWD, p, m)); }
int statvfs(const char *p, struct statvfs *v) {
    struct statfs b;
    long r = syscall(SYS_statfs, p, &b);
    if (r == 0) {
        v->f_bsize = b.f_bsize; v->f_frsize = b.f_frsize ? b.f_frsize : b.f_bsize;
        v->f_blocks = b.f_blocks; v->f_bfree = b.f_bfree; v->f_bavail = b.f_bavail;
        v->f_files = b.f_files; v->f_ffree = b.f_ffree; v->f_favail = b.f_ffree;
        v->f_fsid = (unsigned long)(unsigned)b.f_fsid.__val[0] | ((unsigned long)(unsigned)b.f_fsid.__val[1] << 32);
        v->f_flag = (b.f_flags & 0x20) ? (unsigned long)(b.f_flags & ~0x20) : 0;   /* ST_VALID: the mount flags */
        v->f_namemax = b.f_namelen;
        tlog("statvfs-freeMB", p, (long)(b.f_bavail * (unsigned long)v->f_frsize >> 20));
    } else tlog("statvfs", p, r);
    return (int)r;
}
int statvfs64(const char *p, struct statvfs64 *v) { return statvfs(p, (struct statvfs *)v); }
DIR *opendir(const char *p) {
    static DIR *(*real)(const char *);
    if (!real) real = (DIR *(*)(const char *))dlsym(RTLD_NEXT, "opendir");
    DIR *d = real ? real(p) : 0;
    tlog("opendir", p, d ? 0 : -1);
    return d;
}

/* ==================== panel ==================== */
/* ==================== the on-screen panel ====================
 *
 * On-screen MPC: every frame the MPC program shows goes through ioctl() (page flip, set crtc or dirty
 * framebuffer). Here that frame's 320x240 MPC screen is
 * enlarged into the add-on's own framebuffer, a control panel (pads, buttons, knobs) is drawn next to it,
 * and that framebuffer is shown instead. Touches on the panel are taken out of the program's touch
 * events (libinput) and sent to the program as control-chip MIDI through the fake control chip;
 * touches on the screen part are scaled back to the MPC's own 320x240 coordinates.
 * MPC_PANEL=0 turns it all off.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <pthread.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

/* 8x14 bitmap font, ASCII 32-126, generated from DejaVu Sans Mono Bold */
static const unsigned char FONT[95][14] = {
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
  {0x00,0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, /* '!' */
  {0x00,0x00,0x00,0x64,0x64,0x64,0x64,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '"' */
  {0x00,0x00,0x00,0x00,0x12,0x16,0x7f,0x24,0x2c,0xfe,0x68,0x48,0x00,0x00}, /* '#' */
  {0x00,0x00,0x10,0x10,0x3c,0x78,0x70,0x3c,0x1e,0x16,0x7e,0x3c,0x10,0x10}, /* '$' */
  {0x00,0x00,0x00,0x70,0xd0,0xd0,0x72,0x18,0x4e,0x0b,0x0b,0x0e,0x00,0x00}, /* '%' */
  {0x00,0x00,0x00,0x38,0x64,0x30,0x30,0x7b,0xcf,0xce,0x6e,0x3f,0x00,0x00}, /* '&' */
  {0x00,0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* "'" */
  {0x00,0x0c,0x18,0x18,0x18,0x10,0x30,0x30,0x10,0x18,0x18,0x18,0x0c,0x00}, /* '(' */
  {0x00,0x30,0x10,0x18,0x18,0x18,0x08,0x08,0x18,0x18,0x18,0x10,0x30,0x00}, /* ')' */
  {0x00,0x00,0x00,0x18,0x5a,0x3c,0x3c,0x5a,0x18,0x00,0x00,0x00,0x00,0x00}, /* '*' */
  {0x00,0x00,0x00,0x00,0x18,0x18,0x18,0xfe,0xfe,0x18,0x18,0x18,0x00,0x00}, /* '+' */
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30}, /* ',' */
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x3c,0x00,0x00,0x00,0x00,0x00}, /* '-' */
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00}, /* '.' */
  {0x00,0x00,0x00,0x06,0x04,0x0c,0x0c,0x08,0x18,0x10,0x30,0x20,0x60,0x40}, /* '/' */
  {0x00,0x00,0x00,0x3c,0x66,0x66,0x7e,0x7e,0x66,0x66,0x66,0x3c,0x00,0x00}, /* '0' */
  {0x00,0x00,0x00,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00}, /* '1' */
  {0x00,0x00,0x00,0x3c,0x4c,0x06,0x0e,0x0c,0x18,0x30,0x60,0x7e,0x00,0x00}, /* '2' */
  {0x00,0x00,0x00,0x3c,0x46,0x06,0x3c,0x06,0x06,0x06,0x46,0x3c,0x00,0x00}, /* '3' */
  {0x00,0x00,0x00,0x0c,0x1c,0x3c,0x6c,0x4c,0x7e,0x0c,0x0c,0x0c,0x00,0x00}, /* '4' */
  {0x00,0x00,0x00,0x7c,0x60,0x60,0x7c,0x4e,0x06,0x06,0x4e,0x3c,0x00,0x00}, /* '5' */
  {0x00,0x00,0x00,0x3c,0x60,0x60,0x7c,0x66,0x66,0x66,0x66,0x3c,0x00,0x00}, /* '6' */
  {0x00,0x00,0x00,0x7e,0x06,0x0c,0x0c,0x1c,0x18,0x18,0x30,0x30,0x00,0x00}, /* '7' */
  {0x00,0x00,0x00,0x3c,0x66,0x66,0x3c,0x66,0x66,0x66,0x66,0x3c,0x00,0x00}, /* '8' */
  {0x00,0x00,0x00,0x3c,0x6e,0x66,0x66,0x6e,0x3e,0x06,0x4c,0x38,0x00,0x00}, /* '9' */
  {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00}, /* ':' */
  {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x18,0x30}, /* ';' */
  {0x00,0x00,0x00,0x00,0x00,0x02,0x1e,0x78,0xe0,0x78,0x1e,0x02,0x00,0x00}, /* '<' */
  {0x00,0x00,0x00,0x00,0x00,0x00,0xfe,0xfe,0x00,0xfe,0xfe,0x00,0x00,0x00}, /* '=' */
  {0x00,0x00,0x00,0x00,0x00,0x40,0x70,0x1e,0x06,0x1e,0x70,0x40,0x00,0x00}, /* '>' */
  {0x00,0x00,0x00,0x3c,0x06,0x06,0x0c,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, /* '?' */
  {0x00,0x00,0x00,0x3c,0x62,0x5e,0xd2,0xb2,0xb2,0xb2,0xd2,0x5e,0x62,0x1e}, /* '@' */
  {0x00,0x00,0x00,0x18,0x3c,0x3c,0x2c,0x64,0x7e,0x66,0x46,0xc3,0x00,0x00}, /* 'A' */
  {0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x7c,0x66,0x66,0x66,0x7c,0x00,0x00}, /* 'B' */
  {0x00,0x00,0x00,0x1c,0x32,0x60,0x60,0x60,0x60,0x60,0x32,0x1c,0x00,0x00}, /* 'C' */
  {0x00,0x00,0x00,0x7c,0x6e,0x66,0x66,0x66,0x66,0x66,0x6e,0x7c,0x00,0x00}, /* 'D' */
  {0x00,0x00,0x00,0x7e,0x60,0x60,0x60,0x7e,0x60,0x60,0x60,0x7e,0x00,0x00}, /* 'E' */
  {0x00,0x00,0x00,0x7e,0x60,0x60,0x60,0x7e,0x60,0x60,0x60,0x60,0x00,0x00}, /* 'F' */
  {0x00,0x00,0x00,0x1c,0x72,0x60,0x60,0x6e,0x66,0x66,0x36,0x3e,0x00,0x00}, /* 'G' */
  {0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x7e,0x66,0x66,0x66,0x66,0x00,0x00}, /* 'H' */
  {0x00,0x00,0x00,0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00}, /* 'I' */
  {0x00,0x00,0x00,0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x4c,0x7c,0x00,0x00}, /* 'J' */
  {0x00,0x00,0x00,0x66,0x6c,0x7c,0x78,0x78,0x6c,0x6c,0x66,0x67,0x00,0x00}, /* 'K' */
  {0x00,0x00,0x00,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x7e,0x00,0x00}, /* 'L' */
  {0x00,0x00,0x00,0xe6,0xe6,0xfe,0xfe,0xda,0xda,0xc2,0xc2,0xc2,0x00,0x00}, /* 'M' */
  {0x00,0x00,0x00,0x66,0x66,0x76,0x76,0x5e,0x4e,0x4e,0x4e,0x46,0x00,0x00}, /* 'N' */
  {0x00,0x00,0x00,0x3c,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x00,0x00}, /* 'O' */
  {0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x7c,0x60,0x60,0x60,0x00,0x00}, /* 'P' */
  {0x00,0x00,0x00,0x3c,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x0e,0x04}, /* 'Q' */
  {0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x7c,0x6c,0x66,0x67,0x00,0x00}, /* 'R' */
  {0x00,0x00,0x00,0x3c,0x62,0x60,0x70,0x3c,0x0e,0x06,0x46,0x3c,0x00,0x00}, /* 'S' */
  {0x00,0x00,0x00,0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00}, /* 'T' */
  {0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x00,0x00}, /* 'U' */
  {0x00,0x00,0x00,0xc6,0x66,0x66,0x66,0x64,0x3c,0x3c,0x3c,0x38,0x00,0x00}, /* 'V' */
  {0x00,0x00,0x00,0xc3,0xc3,0xdb,0xda,0x5a,0x7e,0x6e,0x66,0x66,0x00,0x00}, /* 'W' */
  {0x00,0x00,0x00,0xe6,0x66,0x3c,0x3c,0x18,0x3c,0x3c,0x66,0xc6,0x00,0x00}, /* 'X' */
  {0x00,0x00,0x00,0xc7,0x66,0x6e,0x3c,0x38,0x18,0x18,0x18,0x18,0x00,0x00}, /* 'Y' */
  {0x00,0x00,0x00,0x7e,0x06,0x0e,0x1c,0x18,0x38,0x70,0x60,0x7e,0x00,0x00}, /* 'Z' */
  {0x00,0x1c,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x1c,0x00}, /* '[' */
  {0x00,0x00,0x00,0x40,0x60,0x20,0x30,0x10,0x18,0x08,0x0c,0x0c,0x04,0x06}, /* '\\' */
  {0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x38,0x00}, /* ']' */
  {0x00,0x00,0x00,0x18,0x3c,0x6c,0x46,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '^' */
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '_' */
  {0x00,0x00,0x30,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '`' */
  {0x00,0x00,0x00,0x00,0x00,0x3c,0x46,0x06,0x7e,0x66,0x66,0x7e,0x00,0x00}, /* 'a' */
  {0x00,0x60,0x60,0x60,0x60,0x7c,0x66,0x66,0x66,0x66,0x66,0x7c,0x00,0x00}, /* 'b' */
  {0x00,0x00,0x00,0x00,0x00,0x1c,0x32,0x60,0x60,0x60,0x32,0x1c,0x00,0x00}, /* 'c' */
  {0x00,0x06,0x06,0x06,0x06,0x3e,0x6e,0x66,0x66,0x66,0x6e,0x3e,0x00,0x00}, /* 'd' */
  {0x00,0x00,0x00,0x00,0x00,0x3c,0x66,0x66,0x7e,0x60,0x62,0x3c,0x00,0x00}, /* 'e' */
  {0x00,0x0e,0x18,0x18,0x18,0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00}, /* 'f' */
  {0x00,0x00,0x00,0x00,0x00,0x3e,0x6e,0x66,0x66,0x66,0x6e,0x3e,0x06,0x46}, /* 'g' */
  {0x00,0x60,0x60,0x60,0x60,0x7c,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00}, /* 'h' */
  {0x00,0x18,0x18,0x00,0x00,0x78,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00}, /* 'i' */
  {0x00,0x18,0x18,0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* 'j' */
  {0x00,0x60,0x60,0x60,0x60,0x66,0x6c,0x78,0x78,0x6c,0x66,0x66,0x00,0x00}, /* 'k' */
  {0x00,0x70,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x1e,0x00,0x00}, /* 'l' */
  {0x00,0x00,0x00,0x00,0x00,0xfe,0xda,0xda,0xda,0xda,0xda,0xda,0x00,0x00}, /* 'm' */
  {0x00,0x00,0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00}, /* 'n' */
  {0x00,0x00,0x00,0x00,0x00,0x3c,0x66,0x66,0x66,0x66,0x66,0x3c,0x00,0x00}, /* 'o' */
  {0x00,0x00,0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x66,0x7c,0x60,0x60}, /* 'p' */
  {0x00,0x00,0x00,0x00,0x00,0x3e,0x6e,0x66,0x66,0x66,0x6e,0x3e,0x06,0x06}, /* 'q' */
  {0x00,0x00,0x00,0x00,0x00,0x3e,0x38,0x30,0x30,0x30,0x30,0x30,0x00,0x00}, /* 'r' */
  {0x00,0x00,0x00,0x00,0x00,0x3c,0x64,0x70,0x3c,0x0e,0x46,0x3c,0x00,0x00}, /* 's' */
  {0x00,0x00,0x00,0x30,0x30,0x7e,0x30,0x30,0x30,0x30,0x18,0x1e,0x00,0x00}, /* 't' */
  {0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x6e,0x3e,0x00,0x00}, /* 'u' */
  {0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x2c,0x3c,0x3c,0x18,0x00,0x00}, /* 'v' */
  {0x00,0x00,0x00,0x00,0x00,0xc3,0xc3,0xda,0x5a,0x7e,0x6e,0x66,0x00,0x00}, /* 'w' */
  {0x00,0x00,0x00,0x00,0x00,0x66,0x3c,0x3c,0x18,0x3c,0x6c,0x66,0x00,0x00}, /* 'x' */
  {0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x3c,0x3c,0x3c,0x18,0x18,0x38}, /* 'y' */
  {0x00,0x00,0x00,0x00,0x00,0x7e,0x0e,0x1c,0x18,0x30,0x70,0x7e,0x00,0x00}, /* 'z' */
  {0x00,0x0e,0x18,0x18,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0e,0x00}, /* '{' */
  {0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* '|' */
  {0x00,0x70,0x18,0x18,0x18,0x18,0x18,0x0e,0x18,0x18,0x18,0x18,0x70,0x00}, /* '}' */
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x70,0x0e,0x00,0x00,0x00,0x00,0x00}, /* '~' */
};

/* 6x10 small bitmap font, ASCII 32-126 */
static const unsigned char SFONT[95][10] = {
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x20,0x20,0x20,0x20,0x20,0x00,0x20,0x00,0x00},
  {0x00,0x50,0x50,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x28,0x28,0xfc,0x50,0xf8,0x50,0xf0,0x00,0x00},
  {0x00,0x20,0x70,0x60,0x70,0x38,0x38,0x70,0x20,0x00},
  {0x00,0xe0,0xa0,0xe8,0x20,0x58,0x38,0x18,0x00,0x00},
  {0x00,0x70,0x40,0x60,0x60,0xf8,0xd8,0x78,0x00,0x00},
  {0x00,0x20,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x10,0x20,0x20,0x20,0x20,0x20,0x20,0x10,0x00,0x00},
  {0x60,0x20,0x30,0x30,0x30,0x30,0x20,0x60,0x00,0x00},
  {0x00,0x20,0x78,0x78,0x20,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x20,0x20,0xf8,0x20,0x20,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x20,0x20,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x70,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00,0x00},
  {0x00,0x08,0x10,0x10,0x20,0x20,0x40,0x40,0x00,0x00},
  {0x00,0x70,0x58,0x58,0xf8,0x58,0x58,0x70,0x00,0x00},
  {0x00,0x70,0x30,0x30,0x30,0x30,0x30,0x78,0x00,0x00},
  {0x00,0x70,0x18,0x18,0x10,0x20,0x40,0xf8,0x00,0x00},
  {0x00,0x70,0x18,0x18,0x30,0x18,0x18,0xf0,0x00,0x00},
  {0x00,0x30,0x30,0x50,0x50,0xf8,0x10,0x10,0x00,0x00},
  {0x00,0x78,0x40,0x40,0x70,0x18,0x18,0x70,0x00,0x00},
  {0x00,0x38,0x40,0x40,0xf0,0x58,0x58,0x70,0x00,0x00},
  {0x00,0xf8,0x18,0x10,0x30,0x30,0x20,0x60,0x00,0x00},
  {0x00,0x70,0x58,0x58,0x70,0x58,0x58,0x70,0x00,0x00},
  {0x00,0x70,0xd8,0xd8,0x78,0x18,0x18,0x70,0x00,0x00},
  {0x00,0x00,0x00,0x30,0x30,0x00,0x30,0x30,0x00,0x00},
  {0x00,0x00,0x00,0x30,0x30,0x00,0x30,0x20,0x20,0x00},
  {0x00,0x00,0x00,0x08,0x70,0xc0,0x70,0x08,0x00,0x00},
  {0x00,0x00,0x00,0x00,0xf8,0x00,0xf8,0x00,0x00,0x00},
  {0x00,0x00,0x00,0xc0,0x70,0x18,0x70,0xc0,0x00,0x00},
  {0x00,0x70,0x58,0x18,0x30,0x20,0x00,0x20,0x00,0x00},
  {0x00,0x00,0x70,0x48,0xb8,0xa8,0xa8,0xb8,0x40,0x78},
  {0x00,0x30,0x70,0x70,0x50,0x78,0xc8,0xc8,0x00,0x00},
  {0x00,0xf0,0xd8,0xd8,0xf0,0xd8,0xc8,0xf8,0x00,0x00},
  {0x00,0x30,0x68,0x40,0x40,0x40,0x68,0x30,0x00,0x00},
  {0x00,0x70,0x58,0x58,0x48,0x58,0x58,0x70,0x00,0x00},
  {0x00,0x78,0x40,0x40,0x78,0x40,0x40,0x78,0x00,0x00},
  {0x00,0x78,0x40,0x40,0x78,0x40,0x40,0x40,0x00,0x00},
  {0x00,0x30,0x68,0x40,0xc0,0x58,0x68,0x38,0x00,0x00},
  {0x00,0x58,0x58,0x58,0x78,0x58,0x58,0x58,0x00,0x00},
  {0x00,0x78,0x20,0x20,0x20,0x20,0x20,0x78,0x00,0x00},
  {0x00,0x78,0x18,0x18,0x18,0x18,0x10,0x70,0x00,0x00},
  {0x00,0xd8,0xd0,0xf0,0xf0,0xd0,0xd8,0xc8,0x00,0x00},
  {0x00,0x40,0x40,0x40,0x40,0x40,0x40,0x78,0x00,0x00},
  {0x00,0xd8,0xd8,0xf8,0xf8,0xf8,0xc8,0xc8,0x00,0x00},
  {0x00,0xc8,0xe8,0xe8,0xf8,0xf8,0xd8,0xd8,0x00,0x00},
  {0x00,0x70,0x58,0xc8,0xc8,0xc8,0x58,0x70,0x00,0x00},
  {0x00,0x70,0x58,0x58,0x70,0x40,0x40,0x40,0x00,0x00},
  {0x00,0x70,0x58,0xc8,0xc8,0xc8,0x58,0x70,0x18,0x00},
  {0x00,0xf0,0xd8,0xd8,0xf0,0xd0,0xd8,0xc8,0x00,0x00},
  {0x00,0x70,0x48,0x40,0x70,0x18,0x18,0x70,0x00,0x00},
  {0x00,0xf8,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00},
  {0x00,0xc8,0xc8,0xc8,0xc8,0xc8,0x58,0x70,0x00,0x00},
  {0x00,0xc8,0xd8,0x58,0x50,0x70,0x70,0x30,0x00,0x00},
  {0x00,0x88,0x88,0xa8,0xf8,0xf8,0x58,0x58,0x00,0x00},
  {0x00,0xc8,0x58,0x70,0x30,0x70,0x58,0xc8,0x00,0x00},
  {0x00,0xc8,0x58,0x70,0x30,0x20,0x20,0x20,0x00,0x00},
  {0x00,0x78,0x18,0x30,0x30,0x60,0x40,0xf8,0x00,0x00},
  {0x30,0x20,0x20,0x20,0x20,0x20,0x20,0x30,0x00,0x00},
  {0x00,0x40,0x40,0x20,0x20,0x10,0x10,0x08,0x00,0x00},
  {0x70,0x30,0x30,0x30,0x30,0x30,0x30,0x70,0x00,0x00},
  {0x00,0x30,0x58,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xfc},
  {0x60,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x70,0x18,0x78,0xd8,0x78,0x00,0x00},
  {0x40,0x40,0x40,0x70,0x58,0x48,0x58,0x70,0x00,0x00},
  {0x00,0x00,0x00,0x38,0x60,0x40,0x60,0x38,0x00,0x00},
  {0x18,0x18,0x18,0x78,0xd8,0xd8,0xd8,0x78,0x00,0x00},
  {0x00,0x00,0x00,0x70,0x58,0xf8,0xc0,0x78,0x00,0x00},
  {0x38,0x20,0x20,0x78,0x20,0x20,0x20,0x20,0x00,0x00},
  {0x00,0x00,0x00,0x78,0xd8,0xd8,0xd8,0x78,0x18,0x70},
  {0x40,0x40,0x40,0x70,0x58,0x58,0x58,0x58,0x00,0x00},
  {0x30,0x30,0x00,0x70,0x30,0x30,0x30,0x78,0x00,0x00},
  {0x30,0x30,0x00,0x70,0x30,0x30,0x30,0x30,0x30,0x60},
  {0x40,0x40,0x40,0x58,0x70,0x70,0x50,0x58,0x00,0x00},
  {0xe0,0x20,0x20,0x20,0x20,0x20,0x20,0x38,0x00,0x00},
  {0x00,0x00,0x00,0xf8,0xe8,0xe8,0xe8,0xe8,0x00,0x00},
  {0x00,0x00,0x00,0x70,0x58,0x58,0x58,0x58,0x00,0x00},
  {0x00,0x00,0x00,0x70,0x58,0xc8,0x58,0x70,0x00,0x00},
  {0x00,0x00,0x00,0x70,0x58,0x48,0x58,0x70,0x40,0x40},
  {0x00,0x00,0x00,0x78,0xd8,0xd8,0xd8,0x78,0x18,0x18},
  {0x00,0x00,0x00,0x78,0x60,0x60,0x60,0x60,0x00,0x00},
  {0x00,0x00,0x00,0x70,0x60,0x70,0x18,0x70,0x00,0x00},
  {0x00,0x00,0x20,0xf8,0x20,0x20,0x20,0x38,0x00,0x00},
  {0x00,0x00,0x00,0x58,0x58,0x58,0x58,0x78,0x00,0x00},
  {0x00,0x00,0x00,0xc8,0x58,0x50,0x70,0x30,0x00,0x00},
  {0x00,0x00,0x00,0x88,0xa8,0xf8,0x78,0x58,0x00,0x00},
  {0x00,0x00,0x00,0x58,0x70,0x30,0x70,0xd8,0x00,0x00},
  {0x00,0x00,0x00,0xc8,0x58,0x70,0x70,0x30,0x20,0xe0},
  {0x00,0x00,0x00,0x78,0x10,0x30,0x60,0x78,0x00,0x00},
  {0x38,0x30,0x20,0x20,0x60,0x20,0x30,0x38,0x00,0x00},
  {0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x00},
  {0x60,0x20,0x20,0x30,0x38,0x30,0x20,0x60,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x68,0x18,0x00,0x00,0x00,0x00},
};

#define MPC_W 320
#define MPC_H 240

void shim_log(const char *a, const char *b, unsigned v);   /* in the add-on */

static int enabled = -1;
static int panel_on(void) {
    if (enabled < 0) { const char *e = getenv("MPC_PANEL"); enabled = !(e && e[0] == '0'); }
    return enabled;
}

static long now_ms(void);
static unsigned pver = 1;            /* bumped whenever the panel picture changes */
static unsigned drawn_ver[2];        /* panel version already drawn into each of our buffers */

/* ---------- framebuffers ---------- */
struct srcfb { uint32_t id, w, h, pitch; uint8_t *px; };
static struct srcfb src[8];
struct ourfb { uint32_t id, handle, pitch, size; uint8_t *px; };
static struct ourfb our[2];
static int drmfd = -1, nour, front = -1;
static uint32_t DW, DH, last_src;
static volatile int busy;

static long dio(int fd, unsigned long req, void *arg) {
    long r;
    do r = syscall(SYS_ioctl, fd, req, arg); while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r;
}
static void *map(int fd, uint32_t handle, uint32_t size, int rw) {
    struct drm_mode_map_dumb m = { .handle = handle };
    if (dio(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0) return 0;
    void *p = (void *)syscall(SYS_mmap, 0, (long)size, rw ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, (long)m.offset);
    return (long)p < 0 && (long)p > -4096 ? 0 : p;
}
static struct srcfb *get_src(int fd, uint32_t id) {
    for (int i = 0; i < 8; i++) if (src[i].id == id && src[i].px) return &src[i];
    struct drm_mode_fb_cmd c = { .fb_id = id };
    if (dio(fd, DRM_IOCTL_MODE_GETFB, &c) < 0 || !c.handle || c.bpp != 32) return 0;
    uint8_t *px = map(fd, c.handle, c.pitch * c.height, 0);
    if (!px) return 0;
    int k = 0; for (int i = 0; i < 8; i++) if (!src[i].px) { k = i; break; }
    if (src[k].px) syscall(SYS_munmap, src[k].px, (long)src[k].pitch * src[k].h);
    src[k] = (struct srcfb){ id, c.width, c.height, c.pitch, px };
    shim_log("panel: MPC frame", 0, c.width * 10000 + c.height);
    return &src[k];
}
static struct ourfb old[2]; static int nold;
static int is_ours(uint32_t id) {
    for (int i = 0; i < nour; i++) if (our[i].id == id) return 1;
    for (int i = 0; i < nold; i++) if (old[i].id == id) return 1;
    return 0;
}
static void drop(struct ourfb *b) {
    if (b->px) syscall(SYS_munmap, b->px, (long)b->size);
    uint32_t id = b->id; dio(drmfd, DRM_IOCTL_MODE_RMFB, &id);
    struct drm_mode_destroy_dumb d = { .handle = b->handle }; dio(drmfd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    b->px = 0; b->id = 0;
}
static void drop_old(void) { for (int i = 0; i < nold; i++) drop(&old[i]); nold = 0; }
static int nit;
static int make_ours(int fd, uint32_t w, uint32_t h) {
    if (nour && w == DW && h == DH) return 1;
    if (nour) {   /* the display changed size: keep the old pair until a new one is on screen */
        drop_old();
        old[0] = our[0]; old[1] = our[1]; nold = 2; nour = 0; front = -1;
    }
    for (int i = 0; i < 2; i++) {
        struct drm_mode_create_dumb c = { .height = h, .width = w, .bpp = 32 };
        if (dio(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0) return 0;
        struct drm_mode_fb_cmd f = { .width = w, .height = h, .pitch = c.pitch, .bpp = 32, .depth = 24, .handle = c.handle };
        if (dio(fd, DRM_IOCTL_MODE_ADDFB, &f) < 0) return 0;
        uint8_t *px = map(fd, c.handle, (uint32_t)c.size, 1);
        if (!px) return 0;
        our[i] = (struct ourfb){ f.fb_id, c.handle, c.pitch, (uint32_t)c.size, px };
    }
    nour = 2; drmfd = fd; DW = w; DH = h; nit = 0;
    shim_log("panel: on, display", 0, w * 10000 + h);
    return 1;
}

/* ---------- layout: the controls in the same places as on the MPC Sample's top panel ----------
 * (positions and names from the MPC Sample User Guide, "Features"; neutral styling of our own) */
struct rect { int x, y, w, h; };
static struct rect scr, strip, body, scrbtn, kbar;   /* kbar: landscape knob bar under the screen */
enum { PAD, BTN, KNOB, FADER, ENC, BANK, METER };   /* BANK, METER: lights only, not touchable */
struct item { int kind, num; const char *label, *sub; uint32_t color; struct rect r; int down, val, sx, sy, sv; };
static const int PADNOTE[16] = { 37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53 };
static const char *PADSHIFT[16] = { "FULL LEVEL", "HALF SEQ", "DOUBLE SEQ", "COUNT-IN", "COMPRESSOR", "HALF SPEED", "DOUBLE SPEED", "MIDI CONFIG",
                                    "FADER", "REC QUANTIZE", "RESAMPLE", "SONG", "TRIM SAMPLE", "TIME CORRECT", "WARP", "PROJECT" };
/* button notes: confirmed on screen = SAMPLE 6, SEQ 7, KNOB FX 0, SAMPLE SELECT 36, SAMPLE RECORD 73, SEQ RECORD 80,
 * STOP 81, PLAY 82, B2 13, B3 14, CHOP 75, LOOP 74, - 55, + 54 (tested by hand). The others are the remaining notes the program accepts, matched by best guess. */
enum { N_SAMPLE = 6, N_SEQ = 7, N_PADFX = 1, N_KNOBFX = 0, N_CHOP = 75, N_MUTE = 43, N_LOOP = 74, N_16LEV = 40,
       N_SHIFT = 49, N_PADBANK = 35, N_ERASE = 9, N_NOTEREP = 11, N_SAMPSEL = 36, N_TAP = 53, N_MINUS = 55, N_PLUS = 54,
       N_SAMPREC = 73, N_SEQREC = 80, N_STOP = 81, N_PLAY = 82, N_B1 = 12, N_B2 = 13, N_B3 = 14, N_ENCPUSH = 111 };
/* lights without a button: pad bank A-H = CC 57-64 (127 = this bank; C = 59, F = 62 checked on the phone), level meters = CC 83 (left)
 * and 84 (right), value = lit segments as bits (1, 3, 7, ...) */
enum { L_BANK_A = 57, L_METER_L = 83, L_METER_R = 84 };
/* CC numbers: K1-K3 = 16-18, FADER = 20 (found with faderscan.py; 19 moves the filter), encoder 100.
 * MAIN VOLUME is an analog knob on the real unit: the phone's volume buttons do that job. */
#define C_BTN 0x4a4e57
#define C_MODE 0x2f6f73
#define C_FX 0xb4532a
#define C_PLAY 0x3d5aa8
#define C_SHIFT 0x2b2e35
#define C_REC 0x8a2a2a
static struct item it[64];
static int nit;
static int SC = 1;   /* scale for the big font */
static int SS = 1;   /* scale for the small font and spacing */

static struct item *add(int kind, int num, const char *label, const char *sub, uint32_t color, struct rect r) {
    if (nit >= 64) return &it[63];
    it[nit] = (struct item){ kind, num, label, sub, color, r, 0, 64, 0, 0, 0 };
    return &it[nit++];
}
static struct rect R(int x, int y, int w, int h) { return (struct rect){ x, y, w, h }; }
struct bdef { int note; const char *label, *sub; uint32_t color; };
static int button_row(int x, int y, int w, int h, int g, int n, const struct bdef *d) {
    int gg = n == 6 ? g * 3 / 2 : g;              /* 6 buttons = three pairs: a slightly wider gap between the pairs */
    int bw = n == 6 ? (w - 3 * g - 2 * gg) / 6 : 0;
    for (int i = 0; i < n; i++) {
        int x0 = x + (w + g) * i / n, x1 = x + (w + g) * (i + 1) / n - g;
        if (n == 6) { x0 = x + i * (bw + g) + (i / 2) * (gg - g); x1 = x0 + bw; }
        add(BTN, d[i].note, d[i].label, d[i].sub, d[i].color, R(x0, y, x1 - x0, h));
    }
    return y + h + 12 * SS + g;
}
/* pad bank lights at the left, K1-K3 and the encoder, level meters at the right; kd = knob size */
static void knob_row(int x, int y, int W, int g, int kd) {
    static const char *kl[3] = { "K1", "K2", "K3" };
    int side = W * 8 / 100, kx = x + side, kw = W - 2 * side;
    if (kd > kw / 4 - g) kd = kw / 4 - g;
    for (int i = 0; i < 4; i++) {
        struct rect r = R(kx + kw * i / 4 + (kw / 4 - kd) / 2, y, kd, kd);
        if (i < 3) add(KNOB, 16 + i, kl[i], 0, 0, r); else add(ENC, 100, "", 0, 0, r);
    }
    add(BANK, 0, "BANK", 0, 0, R(x, y, side, kd));
    add(METER, 0, "LEVEL", 0, 0, R(x + W - side, y, side, kd));
}
/* top: knobs, encoder and the other buttons in rows; bottom: SHIFT / PAD BANK, fader, ERASE / NOTE REPEAT
 * and the pads (the part of the MPC you play with) */
static int layout_body(struct rect b) {
    int g = b.w / 90 + 3, W = b.w - 2 * g, x = b.x + g, y = b.y + g;
    int bh = b.h / 18; if (bh < 24 * SS) bh = 24 * SS; if (bh > 44 * SS) bh = 44 * SS;
    /* the button rows (and transport row) are 30% taller; the knobs give up the same height, so the
     * screen and pads don't move */
    int bh0 = bh; bh = bh * 13 / 10;
    /* row 1: K1-K3 and the encoder (in landscape it sits under the MPC screen instead) */
    if (DH >= DW) {
        int kd = W / 4 - 3 * g; if (kd > b.h / 7) kd = b.h / 7;
        kd -= 3 * (bh - bh0); if (kd < 28 * SS) kd = 28 * SS;
        knob_row(x, y, W, g, kd);
        y += kd + 12 * SS + g;
    }
    /* three 2x2 groups, like the MPC Sample: MODE (green over orange) | PAD PLAY (blue) | grey */
    static const struct bdef r2[6] = {
        { N_SAMPLE, "SAMPLE", "INPUT CONFIG", C_MODE }, { N_SEQ, "SEQ", "STEP EDIT", C_MODE },
        { N_CHOP, "CHOP", "NOTE ON", C_PLAY }, { N_MUTE, "MUTE", "UNMUTE ALL", C_PLAY },
        { N_SAMPSEL, "SAMPLE\nSELECT", "SAVE SAMPLE", C_BTN }, { N_TAP, "TAP\nTEMPO", "METRO", C_BTN } };
    static const struct bdef r3[6] = {
        { N_PADFX, "PAD\nFX", "FLEX BEAT", C_FX }, { N_KNOBFX, "KNOB\nFX", "FX SELECT", C_FX },
        { N_LOOP, "LOOP", "REVERSE", C_PLAY }, { N_16LEV, "16\nLEVELS", "TYPE", C_PLAY },
        { N_MINUS, "-", "UNDO", C_BTN }, { N_PLUS, "+", "REDO", C_BTN } };
    static const struct bdef r4[4] = {
        { N_SAMPREC, "SAMPLE\nRECORD", "RECALL", C_BTN }, { N_SEQREC, "SEQ\nRECORD", "RECALL", C_BTN },
        { N_STOP, "STOP", 0, C_BTN }, { N_PLAY, "PLAY", "CONTINUE", C_BTN } };
    y = button_row(x, y, W, bh, g, 6, r2);
    y = button_row(x, y, W, bh, g, 6, r3);
    /* transport row at the very bottom, on the left: the window's own screen icon sits at the bottom right */
    int ty = b.y + b.h - g - bh - 12 * SS;
    button_row(x, ty, W * 80 / 100, bh, g, 4, r4);   /* room for the window's screen icon at the right, in both sizes */
    scrbtn = R(x + W * 80 / 100 + g, ty, W - W * 80 / 100 - g, bh);   /* a button face under that icon */
    b.h = ty - g - b.y;   /* pads end right above it */
    /* bottom: one narrow column (SHIFT, PAD BANK, fader, ERASE, NOTE REPEAT stacked) + pads */
    int sc = W * 16 / 100, lx = x, cx = x + sc + g, cw = W - sc - g;
    int ph = b.y + b.h - y - g;
    int pw = (cw - 3 * g) / 4, pdh = (ph - 3 * g) / 4;
    int spare = 0;
    if (pdh > pw) { spare = 4 * (pdh - pw); pdh = pw; }   /* square pads; the rest can go to the screen */
    else if (pw > pdh) { cx += 2 * (pw - pdh); pw = pdh; }   /* short window: narrower square pads, pushed right */
    int padsh = 4 * pdh + 3 * g;
    y = b.y + b.h - g - padsh;   /* pads sit at the bottom */
    for (int i = 0; i < 16; i++) {   /* pad 1 bottom-left ... pad 16 top-right */
        int row = 3 - i / 4, col = i % 4;
        static char nums[16][3];
        nums[i][0] = i + 1 >= 10 ? '1' : '0' + i + 1; nums[i][1] = i + 1 >= 10 ? '0' + (i + 1) % 10 : 0;
        add(PAD, PADNOTE[i], nums[i], PADSHIFT[i], 0, R(cx + col * (pw + g), y + row * (pdh + g), pw, pdh));
    }
    int sbh = bh0 * 9 / 10;   /* left column keeps its size */
    int lab = 12 * SS;
    /* the gap under SHIFT matches the one under ERASE: ERASE has its "COPY" label (lab) before NOTE REPEAT,
     * so SHIFT gets the same lab of space before PAD BANK */
    add(BTN, N_SHIFT, "SHIFT", 0, C_SHIFT, R(lx, y, sc, sbh));
    add(BTN, N_PADBANK, "PAD\nBANK", 0, C_BTN, R(lx, y + sbh + lab + g, sc, sbh));
    int ey = y + padsh - 2 * (sbh + lab) - g;          /* ERASE, NOTE REPEAT at the bottom, each with its small label */
    add(BTN, N_ERASE, "ERASE", "COPY", C_BTN, R(lx, ey, sc, sbh));
    add(BTN, N_NOTEREP, "NOTE\nREPEAT", "TRIPLET", C_BTN, R(lx, ey + sbh + lab + g, sc, sbh));
    int fy = y + 2 * (sbh + g) + g + lab, fh = ey - fy - lab - 2 * g, fw = sc * 5 / 10;
    add(FADER, 20, "FADER", 0, 0, R(lx + (sc - fw) / 2, fy, fw, fh > 40 ? fh : 40));
    return spare;
}

static void layout(void) {
    pver++;
    nit = 0; scrbtn = (struct rect){ 0, 0, 0, 0 }; kbar = scrbtn;
    if (DH >= DW) {   /* portrait: screen on top, controls below */
        /* full screen (tall window, e.g. 540x1212): a blank gap at the top so the phone's camera doesn't cover the screen */
        int top = DH * 10 >= DW * 22 ? (int)DH * 6 / 100 : 0;
        scr = R(0, top, (int)DW, (int)(DW * MPC_H / MPC_W));
        body = R(0, scr.y + scr.h, (int)DW, (int)DH - scr.y - scr.h);
    } else {          /* landscape: screen at the top left, the knob bar under it, controls on the right */
        int half = (int)DW / 2, kh = (int)DH * 16 / 100;   /* knob bar height */
        int sw = half; if (sw * MPC_H / MPC_W > (int)DH - kh) sw = ((int)DH - kh) * MPC_W / MPC_H;
        scr = R((half - sw) / 2, 0, sw, sw * MPC_H / MPC_W);
        kbar = R(0, scr.h, half, (int)DH - scr.h);
        body = R(half, 0, (int)DW - half, (int)DH);
    }
    strip = R(0, 0, 0, 0);
    SC = DW >= 700 ? 2 : 1;
    SS = (DW >= 900 && DH >= 900) ? 2 : 1;
    int spare = layout_body(body);
    if (kbar.h > 0) {   /* landscape: the knob bar under the screen */
        int g = kbar.w / 90 + 3, kd = kbar.h - 12 * SS - 3 * g;
        knob_row(kbar.x + g, kbar.y + g, kbar.w - 2 * g, g, kd);
    }
    if (spare > 4 && DH >= DW) {   /* portrait: give the spare height to the screen (stretched up to 25% taller) */
        int add_h = spare < scr.h / 4 ? spare : scr.h / 4;
        scr.h += add_h; body.y += add_h; body.h -= add_h;
        nit = 0;
        layout_body(body);
    }
    /* B1-B3: the boxes along the top of the MPC screen (tabs, BPM, Rec Quant...) are the function buttons */
    int band = scr.h * 16 / 100;
    for (int i = 0; i < 3; i++) add(BTN, N_B1 + i, 0, 0, 0, R(scr.x + scr.w * i / 3, scr.y, scr.w / 3, band));
}

/* ---------- drawing ---------- */
static double msin(double a) {   /* small sine, no maths library needed */
    const double P = 3.14159265358979, T = 6.28318530717959;
    while (a > P) a -= T; while (a < -P) a += T;
    double s = 1; if (a > P / 2) a = P - a; else if (a < -P / 2) a = -P - a;
    double a2 = a * a; return s * a * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42 * (1 - a2 / 72))));
}
static double mcos(double a) { return msin(a + 1.5707963267949); }
static void fill(uint8_t *px, uint32_t pitch, struct rect r, uint32_t c) {
    for (int y = r.y; y < r.y + r.h; y++) {
        if (y < 0 || y >= (int)DH) continue;
        uint32_t *row = (uint32_t *)(px + (size_t)y * pitch);
        for (int x = r.x; x < r.x + r.w; x++) if (x >= 0 && x < (int)DW) row[x] = c;
    }
}
static void box(uint8_t *px, uint32_t pitch, struct rect r, uint32_t c) {   /* filled, with cut corners */
    for (int y = r.y; y < r.y + r.h; y++) {
        if (y < 0 || y >= (int)DH) continue;
        int cut = (y == r.y || y == r.y + r.h - 1) ? 2 : (y == r.y + 1 || y == r.y + r.h - 2) ? 1 : 0;
        uint32_t *row = (uint32_t *)(px + (size_t)y * pitch);
        for (int x = r.x + cut; x < r.x + r.w - cut; x++) if (x >= 0 && x < (int)DW) row[x] = c;
    }
}
static void disc(uint8_t *px, uint32_t pitch, int cx, int cy, int rad, uint32_t c) {
    for (int y = cy - rad; y <= cy + rad; y++) {
        if (y < 0 || y >= (int)DH) continue;
        uint32_t *row = (uint32_t *)(px + (size_t)y * pitch);
        for (int x = cx - rad; x <= cx + rad; x++) if (x >= 0 && x < (int)DW && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad) row[x] = c;
    }
}
static void glyphs(uint8_t *px, uint32_t pitch, int x0, int y0, const char *s, int n, uint32_t c, int small, int sc) {
    int gw = small ? 6 : 8, gh = small ? 10 : 14;
    for (int i = 0; i < n; i++) {
        int ch = (unsigned char)s[i]; if (ch < 32 || ch > 126) ch = '?';
        for (int gy = 0; gy < gh * sc; gy++) {
            int y = y0 + gy; if (y < 0 || y >= (int)DH) continue;
            uint32_t *row = (uint32_t *)(px + (size_t)y * pitch);
            unsigned bits = small ? SFONT[ch - 32][gy / sc] : FONT[ch - 32][gy / sc];
            for (int gx = 0; gx < gw * sc; gx++) {
                int x = x0 + i * gw * sc + gx;
                if (x >= 0 && x < (int)DW && (bits & (0x80 >> (gx / sc)))) row[x] = c;
            }
        }
    }
}
/* centred text, lines split at '\n'; uses the small font when the big one doesn't fit */
static void text(uint8_t *px, uint32_t pitch, struct rect r, const char *s, uint32_t c, int minlen) {
    int lines = 1, longest = minlen, cur = 0;
    for (const char *p = s; *p; p++) { if (*p == '\n') { lines++; cur = 0; } else if (++cur > longest) longest = cur; }
    int small = 0, sc = SC;
    if (longest * 8 * sc > r.w - 2 || lines * 14 * sc > r.h) {   /* too big: the small font at its size, then size 1 */
        small = 1; sc = SS;
        if (longest * 6 * sc > r.w - 2 || lines * 10 * sc > r.h) { small = 0; sc = 1; if (longest * 8 > r.w - 2 || lines * 14 > r.h) small = 1; }
    }
    int gw = (small ? 6 : 8) * sc, gh = (small ? 10 : 14) * sc;
    int y = r.y + (r.h - lines * gh) / 2;
    const char *p = s;
    while (1) {
        int n = 0; while (p[n] && p[n] != '\n') n++;
        int vis = n; if (vis * gw > r.w) vis = r.w / gw;
        glyphs(px, pitch, r.x + (r.w - vis * gw) / 2, y, p, vis, c, small, sc);
        if (!p[n]) break;
        p += n + 1; y += gh;
    }
}
static void small_text(uint8_t *px, uint32_t pitch, int cx, int y, int maxw, const char *s, uint32_t c) {
    int n = 0, w = 6 * SS; while (s[n]) n++;
    if (n * w > maxw) n = maxw / w;
    glyphs(px, pitch, cx - n * w / 2, y, s, n, c, 1, SS);
}
/* bottom-aligned, wrapped onto two lines at a space if it doesn't fit */
static void small_wrap(uint8_t *px, uint32_t pitch, int cx, int ybottom, int maxw, const char *s, uint32_t c) {
    int n = 0, sp = -1, w = 6 * SS, h = 10 * SS; while (s[n]) { if (s[n] == ' ') sp = n; n++; }
    if (n * w <= maxw || sp < 0) { small_text(px, pitch, cx, ybottom - h, maxw, s, c); return; }
    int n1 = sp * w > maxw ? maxw / w : sp, n2 = n - sp - 1; if (n2 * w > maxw) n2 = maxw / w;
    glyphs(px, pitch, cx - n1 * w / 2, ybottom - 2 * h - 1, s, n1, c, 1, SS);
    glyphs(px, pitch, cx - n2 * w / 2, ybottom - h, s + sp + 1, n2, c, 1, SS);
}
/* ---------- lights, as the MPC program sets them (its MIDI to the control chip, via the add-on) ----------
 *  buttons: CC on channel 1, number = the button's note, value 0 off, 1 dim, 2 on, 3 on (second colour)
 *  pads:    F0 47 7F 5C 65 00 04 <pad 0-15> <r> <g> <b> F7 (7-bit colour); pad 16 = the fader's light */
static volatile signed char btn_led[128];          /* -1 = never set: drawn as before (the displayed, peak-held level) */
static signed char btn_raw[128];                    /* the level the program last asked for */
static signed char btn_pre[128];                    /* each button's real level just before SHIFT was pressed */
static signed char btn_shift_target[128];           /* level to reveal when the SHIFT light-up delay elapses */
static volatile long shift_show_at;                  /* time to reveal the SHIFT lights (all at once) */
static volatile int shift_shown;                     /* 0 during the delay, 1 once the lights have been revealed */
static volatile int pad_rgb[17];                    /* 0xRRGGBB, -1 = never set */
static int lights_init;
/* While SHIFT is held, the program pulses the shift-function LEDs (bright/dim/off) and never settles. So while
 * SHIFT is down, latch each button at its brightest level (steady lit, no flicker); on release, restore every
 * button to the state it had just before SHIFT (press() snapshots it, release() restores and redraws at once —
 * not deferred, because the program may go idle and send no more LED data). Set on the on-screen SHIFT. */
static volatile int shift_held;
static int led_instant(int n) { return (n >= L_BANK_A && n < L_BANK_A + 8) || n == L_METER_L || n == L_METER_R; }
/* momentary buttons: they fire an action and have no lasting on-state, so their light must follow the
 * finger (lit only while physically held) and never keep a level the program left behind. PLAY/record
 * stay out of this set so playback and record-arm keep their program-driven lit/blink states. */
static int btn_momentary(int n) { return n == N_SHIFT || n == N_ERASE || n == N_TAP || n == N_MINUS || n == N_PLUS || n == N_STOP; }
#define SHIFT_REVEAL_MS 70
static void lights_reset(void) { if (lights_init) return; lights_init = 1; for (int i = 0; i < 128; i++) { btn_led[i] = -1; btn_raw[i] = -1; btn_pre[i] = -1; btn_shift_target[i] = -1; } for (int i = 0; i < 17; i++) pad_rgb[i] = -1; }
static void redraw(struct item *t);
/* an LED at a low level looks much brighter than its level: 7-bit level -> 8-bit screen colour, square root curve */
static int led8(int v) { int t = v * 255 * 255 / 127, r = 0; while ((r + 1) * (r + 1) <= t) r++; return r; }
static const int PADNOTE_OF[16] = { 37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53 };
static void light_changed(int kind, int num) {
    for (int i = 0; i < nit; i++) if (it[i].kind == kind && it[i].num == num && (kind != BTN || it[i].label)) redraw(&it[i]);
}
/* microphone: while SAMPLE RECORD is lit, ask audiobridge (outside the chroot) to switch the sound card to
 * recording, through the file /tmp/mpc-mic.on (the card can't play and record at once) */
static void tap_clock_start(void);
static volatile long rec_lit_at;   /* when the SAMPLE RECORD light was last on */
static void mic_set(int on) {
    static int last = -1;
    if (on == last) return;
    last = on;
    if (on) { long fd = syscall(SYS_openat, AT_FDCWD, "/tmp/mpc-mic.on", O_WRONLY | O_CREAT | O_CLOEXEC, 0644); if (fd >= 0) syscall(SYS_close, fd); }
    else syscall(SYS_unlinkat, AT_FDCWD, "/tmp/mpc-mic.on", 0);
    shim_log(on ? "mic: recording (sound off)" : "mic: off (sound on)", 0, 0);
}
/* the light blinks while the MPC waits for a signal: mic on at once, off only after 1.5 s without light */
static void mic_request(int on) {
    if (on) { rec_lit_at = now_ms(); mic_set(1); }
    tap_clock_start();   /* its clock turns the mic off */
}
static void mic_check(void) {
    if (!rec_lit_at) return;
    if (btn_led[N_SAMPREC] >= 2) rec_lit_at = now_ms();   /* still lit */
    else if (now_ms() - rec_lit_at > 1500) { rec_lit_at = 0; mic_set(0); }
}
/* the SHIFT light-up delay has elapsed: reveal every gathered light at once. Driven by the 50 ms tap_clock so it
 * fires even if the program sent no more LED data after SHIFT was pressed (otherwise the lights would never show). */
static void shift_reveal(void) {
    if (!(lights_init && shift_held && !shift_shown && now_ms() >= shift_show_at)) return;
    shift_shown = 1;
    for (int j = 0; j < 128; j++)
        if (!led_instant(j) && btn_led[j] != btn_shift_target[j]) { btn_led[j] = btn_shift_target[j]; light_changed(BTN, j); }
}
/* called by the add-on with every block the program writes to the control chip (card 8) */
void panel_midi_out(const unsigned char *b, unsigned long n) {
    static unsigned char m[16]; static int k, sysex, run;
    lights_reset();
    shift_reveal();
    for (unsigned long i = 0; i < n; i++) {
        unsigned char c = b[i];
        if (c == 0xF0) { sysex = 1; k = 0; continue; }
        if (sysex) {
            if (c == 0xF7) {
                sysex = 0;
                {   /* log every Akai control-chip SysEx (sub-command m5, index m6, top data byte) to find knob feedback */
                    static int sxlog = -1;
                    if (sxlog < 0) { const char *e = getenv("MPC_LOG"); sxlog = e && e[0] == '1'; }
                    if (sxlog && k >= 7 && m[0] == 0x47) {
                        int dv = 0; for (int j2 = 7; j2 < k && j2 < 13; j2++) if (m[j2] > dv) dv = m[j2];
                        shim_log("sysex m5*1e6+m6*1e3+val", 0, (unsigned)m[5] * 1000000u + (unsigned)m[6] * 1000u + (unsigned)dv);
                    }
                }
                if (k == 10 && m[0] == 0x47 && m[1] == 0x7F && m[2] == 0x5C && m[3] == 0x65 && m[5] == 0x04 && m[6] <= 16) {
                    int rgb = (led8(m[7]) << 16) | (led8(m[8]) << 8) | led8(m[9]);
                    if (pad_rgb[m[6]] != rgb) {
                        pad_rgb[m[6]] = rgb;   /* pad 16 = the fader's LED only: like the hardware, the fader itself
                                                * is not motorized - its cap stays where your finger left it */
                        light_changed(m[6] < 16 ? PAD : FADER, m[6] < 16 ? PADNOTE_OF[m[6]] : 20);
                    }
                }
            } else if (k < 16) m[k++] = c;
            continue;
        }
        if (c >= 0xF8) continue;
        if (c & 0x80) { run = c; k = 0; continue; }
        if (!run) continue;
        if (k < 2) m[k++] = c;
        if (k == 2) {
            k = 0;
            if (run == 0xB0) {
                static int ledlog = -1;
                if (ledlog < 0) { const char *e = getenv("MPC_LOG"); ledlog = e && e[0] == '1'; }
                if (ledlog) shim_log("btnled note*1000+val", 0, (unsigned)m[0] * 1000u + (unsigned)m[1]);
            }
            if (run == 0xB0) {
                int note = m[0], val = (signed char)m[1];
                btn_raw[note] = val;
                if (note == N_SAMPREC) mic_request(val >= 2);   /* the mic follows the real level, not the held one */
                if (led_instant(note)) {                        /* meters, bank lights: show at once, never held */
                    if (btn_led[note] != val) {
                        btn_led[note] = val;
                        if (note >= L_BANK_A && note < L_BANK_A + 8) light_changed(BANK, 0);
                        else light_changed(METER, 0);
                    }
                } else if (shift_held && !shift_shown) {
                    /* during the reveal delay: gather the brightest level silently, keep the display on the
                     * pre-shift look, so every light pops on together when the delay elapses */
                    if (val > btn_shift_target[note]) btn_shift_target[note] = val;
                } else if (shift_held) {
                    /* after the reveal: latch each button at its brightest level so it stays steadily lit,
                     * never following the program's dim/off pulse phases (btn_raw above keeps the real level) */
                    if (val > btn_led[note]) { btn_led[note] = val; light_changed(BTN, note); }
                } else if (btn_led[note] != val) {              /* not shifted: show the program's real level directly,
                                                                 * so TAP TEMPO's metronome and the record-arm LEDs blink as they should */
                    btn_led[note] = val; light_changed(BTN, note);
                }
            }
        }
    }
}
static uint32_t scale(uint32_t c, int num, int den) {
    return ((((c >> 16) & 255) * num / den) << 16) | ((((c >> 8) & 255) * num / den) << 8) | ((c & 255) * num / den);
}
/* multiply brightness and clamp — keeps the LED colours vivid, not washed out */
static uint32_t brighten(uint32_t c, int num, int den) {
    int r = (int)((c >> 16) & 255) * num / den, g = (int)((c >> 8) & 255) * num / den, b = (int)(c & 255) * num / den;
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static int isqrt_i(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }
/* filled rounded rectangle for the buttons/pads (the terminal drew cut corners before) */
static void roundbox(uint8_t *px, uint32_t pitch, struct rect r, uint32_t c, int rad) {
    if (rad < 1) { box(px, pitch, r, c); return; }
    if (rad * 2 > r.h) rad = r.h / 2;
    if (rad * 2 > r.w) rad = r.w / 2;
    for (int y = r.y; y < r.y + r.h; y++) {
        if (y < 0 || y >= (int)DH) continue;
        int top = (r.y + rad) - y, bot = y - (r.y + r.h - 1 - rad);
        int dcy = top > 0 ? top : bot > 0 ? bot : 0;
        int inset = (dcy > 0 && dcy <= rad) ? rad - isqrt_i(rad * rad - dcy * dcy) : 0;
        uint32_t *row = (uint32_t *)(px + (size_t)y * pitch);
        for (int x = r.x + inset; x < r.x + r.w - inset; x++) if (x >= 0 && x < (int)DW) row[x] = c;
    }
}
static int g_label_room(struct rect r) { return r.w < 40 ? 8 : 4; }
#define BG_BODY 0x1b1d22
#define BG_STRIP 0x0c0d10
#define C_LABEL 0x9aa0aa
static void draw_item(uint8_t *px, uint32_t pitch, struct item *t) {
    if (t->kind == BTN && !t->label) return;   /* touch area over the MPC screen: nothing to draw */
    struct rect r = t->r;
    uint32_t bg = (t->r.y < body.y && DH >= DW) || (DH < DW && t->r.y < body.y) ? BG_STRIP : BG_BODY;
    switch (t->kind) {
    case PAD: {
        int pi = 0; while (pi < 16 && PADNOTE_OF[pi] != t->num) pi++;
        int lit = lights_init && pi < 16 ? pad_rgb[pi] : -1;     /* the MPC's own pad colour */
        int glow = lit > 0 && (((lit >> 16) & 255) + ((lit >> 8) & 255) + (lit & 255)) > 48;   /* empty pads: off */
        int mx = 0, tint = 0;
        if (glow) {   /* lift dim LEDs so their colour shows, but keep brighter ones brighter (idle vs active) */
            mx = ((lit >> 16) & 255) > ((lit >> 8) & 255) ? (lit >> 16) & 255 : (lit >> 8) & 255; if ((lit & 255) > mx) mx = lit & 255;
            lit = (int)scale((uint32_t)lit, 70 + mx * 72 / 100, mx);   /* brightest channel: 70 + 0.72 x level */
            tint = mx * 45 / 255;                                     /* % of the colour in the pad face */
        }
        uint32_t face = glow ? scale((uint32_t)lit, tint, 100) + scale(0x30333a, 100 - tint, 100) : 0x30333a;
        int bright = 0, e = glow ? 5 * SS : 3;   /* the light shows as a glowing edge, as on the real pads */
        int prad = r.w / 10;
        struct rect in = r; roundbox(px, pitch, r, t->down ? 0xe8e8e8 : glow ? (uint32_t)lit : 0x3a3d45, prad);
        in.x += e; in.y += e; in.w -= 2 * e; in.h -= 2 * e;
        roundbox(px, pitch, in, face, prad > e ? prad - e : 1);
        glyphs(px, pitch, in.x + 2, in.y + 2, t->label, t->label[1] ? 2 : 1, bright ? 0x000000 : 0xb0b4bc, SS == 2 ? 0 : 1, SS == 2 ? 1 : 1);
        small_wrap(px, pitch, r.x + r.w / 2, in.y + in.h - 3, in.w - 4, t->sub, bright ? 0x202020 : 0x8c919a);
        break; }
    case BTN: {
        int led = lights_init && t->num >= 0 && t->num < 128 ? btn_led[t->num] : -1;
        if (btn_momentary(t->num) && !t->down && !shift_held) led = -1;   /* dark unless the finger is on it */
        if (t->num == N_SAMPREC || t->num == N_SEQREC || t->num == N_STOP || t->num == N_PLAY) {   /* transport */
            uint32_t sc = t->num == N_PLAY ? 0x2fd65a : t->num == N_STOP ? 0xf2f2f2 : 0xe8322c;
            roundbox(px, pitch, r, t->down ? 0x6a6e78 : C_BTN, r.h / 8);
            int sh = r.h / 7 > 3 ? r.h / 7 : 3;
            struct rect st = R(r.x + r.w / 8, r.y + r.h - sh - r.h / 9, r.w * 6 / 8, sh), up = R(r.x, r.y, r.w, st.y - r.y);
            fill(px, pitch, st, led >= 2 ? sc : led == 1 ? scale(sc, 1, 2) : scale(sc, 1, 5));
            int s = up.h * 5 / 10, cx = up.x + up.w / 2, cy = up.y + up.h / 2 + 1;
            if (t->num == N_STOP) fill(px, pitch, R(cx - s / 2, cy - s / 2, s, s), 0xf2f2f2);
            else if (t->num == N_PLAY) for (int y = 0; y < s; y++) { int w = (y < s / 2 ? y : s - 1 - y) * 8 / 5; fill(px, pitch, R(cx - s / 3, cy - s / 2 + y, w + 1, 1), 0xf2f2f2); }
            else text(px, pitch, up, t->label, 0xf2f2f2, 6);
            if (t->sub) { fill(px, pitch, R(r.x - 2, r.y + r.h + 1, r.w + 4, 11 * SS), bg); small_text(px, pitch, r.x + r.w / 2, r.y + r.h + 2, r.w + 6, t->sub, C_LABEL); }
            break;
        }
        uint32_t c = led < 0 ? t->color : led == 0 ? brighten(t->color, 1, 2) : led == 1 ? brighten(t->color, 85, 100) : led == 2 ? brighten(t->color, 150, 100) : brighten(t->color, 190, 100);
        if (t->down) c = brighten(c, 190, 100);
        roundbox(px, pitch, r, c, r.h / 8);
        if (led >= 2) roundbox(px, pitch, R(r.x + r.w / 5, r.y + 2 * SS, r.w * 3 / 5, 2 * SS), 0xffffff, SS);   /* the light strip at the top */
        text(px, pitch, r, t->label, led >= 2 || t->down ? 0x000000 : led == 0 ? 0x9a9da4 : 0xf2f2f2, 6);   /* same size on all buttons */
        if (t->sub) { fill(px, pitch, R(r.x - 2, r.y + r.h + 1, r.w + 4, 11 * SS), bg); small_text(px, pitch, r.x + r.w / 2, r.y + r.h + 2, r.w + 6, t->sub, C_LABEL); }
        break; }
    case KNOB: {   /* round knob, pointer from 7 o'clock (0) to 5 o'clock (127) */
        int rad = r.w / 2, cx = r.x + rad, cy = r.y + rad;
        fill(px, pitch, R(r.x - 1, r.y - 1, r.w + 2, r.h + 2 + (t->num == 20 ? 0 : 12 * SS)), bg);
        disc(px, pitch, cx, cy, rad, t->down ? 0x5a5f69 : 0x3a3d45);
        disc(px, pitch, cx, cy, rad * 7 / 10, 0x23252b);
        double a = (-225.0 + 270.0 * t->val / 127.0) * 3.14159265 / 180.0;
        for (int k = rad * 3 / 10; k < rad - 1; k++) {
            int x = cx + (int)(k * mcos(a)), y = cy + (int)(k * msin(a));
            disc(px, pitch, x, y, rad > 20 ? 2 : 1, 0xf0f0f0);
        }
        if (t->num == 20) small_text(px, pitch, r.x + r.w + 4 + 9 * SS, cy - 5 * SS, 30 * SS, "VOL", C_LABEL);
        else small_text(px, pitch, cx, r.y + r.h + 2, r.w + 10, t->label, C_LABEL);
        break; }
    case FADER: {
        fill(px, pitch, R(r.x - 4, r.y - 2, r.w + 8 + 17 * SS, r.h + 4 + 12 * SS), bg);   /* also under its light */
        int tw = r.w / 5 > 3 ? r.w / 5 : 3;
        fill(px, pitch, R(r.x + (r.w - tw) / 2, r.y, tw, r.h), 0x0a0a0c);
        int th = r.w * 6 / 10; if (th < 14) th = 14;
        int ty = r.y + (r.h - th) * (127 - t->val) / 127;
        box(px, pitch, R(r.x, ty, r.w, th), t->down ? 0xd0d3d8 : 0x8d929b);
        {   /* the fader's light, beside it: the MPC sets its brightness (brighter = higher value) */
            int fl = lights_init ? pad_rgb[16] : -1, lv = fl > 0 ? (fl >> 8) & 255 : 0;
            int lw = 9 * SS, lh = 5 * SS, lx = r.x + r.w + 5 * SS, ly = r.y + r.h / 2 - lh / 2;
            uint32_t lc = lv ? (uint32_t)((lv << 16) | (lv << 8) | lv) : 0x2a2c31;
            if (lv) fill(px, pitch, R(lx - 2, ly - 2, lw + 4, lh + 4), scale(lc, 1, 4));   /* a little glow */
            fill(px, pitch, R(lx, ly, lw, lh), lc);
        }
        fill(px, pitch, R(r.x + 2, ty + th / 2, r.w - 4, 2), 0x1b1d22);
        small_text(px, pitch, r.x + r.w / 2, r.y + r.h + 3, 40 * SS, "FADER", C_LABEL);
        break; }
    case BANK: {   /* A-H in two columns, the current bank lit; just the letter if there's no room */
        fill(px, pitch, R(r.x, r.y, r.w, r.h + 12 * SS), bg);
        int cur = -1;
        if (lights_init) for (int i = 0; i < 8; i++) if (btn_led[L_BANK_A + i] > 0) { cur = i; break; }
        int big = 4 * 14 * SS <= r.h && 2 * 8 * SS + 5 * SS <= r.w;   /* the big font when it fits */
        int cw = (big ? 8 : 6) * SS, ch = (big ? 14 : 10) * SS, gx = 5 * SS, gy = (r.h - 4 * ch) / 5;
        if (2 * cw + gx <= r.w && gy >= 0) {
            int x0 = r.x + (r.w - 2 * cw - gx) / 2;
            for (int i = 0; i < 8; i++) {
                char l = (char)('A' + i);
                int lx = x0 + (i / 4) * (cw + gx), ly = r.y + gy + (i % 4) * (ch + gy);
                int on = lights_init && btn_led[L_BANK_A + i] > 0;   /* every lit one, as the MPC sets them */
                if (on) fill(px, pitch, R(lx - SS, ly - SS, cw + 2 * SS, ch + 2 * SS), 0xe8a33a);
                glyphs(px, pitch, lx, ly, &l, 1, on ? 0x000000 : 0x6a6f79, !big, SS);
            }
        } else if (cur >= 0) {
            char l = (char)('A' + cur);
            glyphs(px, pitch, r.x + (r.w - cw) / 2, r.y + (r.h - ch) / 2, &l, 1, 0xe8a33a, 1, SS);
        }
        small_text(px, pitch, r.x + r.w / 2, r.y + r.h + 2, r.w + 2 * g_label_room(r), t->label, C_LABEL);
        break; }
    case METER: {   /* left and right, 5 segments each: green, green, green, yellow, red (bottom to top) */
        fill(px, pitch, R(r.x, r.y, r.w, r.h + 12 * SS), bg);
        static const uint32_t segc[5] = { 0x2fd65a, 0x2fd65a, 0x2fd65a, 0xe8c22c, 0xe8322c };
        int bw = r.w * 3 / 10; if (bw < 3) bw = 3;
        int sg = 2 * SS, sh = (r.h - 4 * sg) / 5;
        for (int ch = 0; ch < 2; ch++) {
            int v = lights_init ? btn_led[ch ? L_METER_R : L_METER_L] : 0, n = 0;
            for (; v > 0; v >>= 1) n += v & 1;
            int bx = r.x + r.w / 2 + (ch ? bw / 3 : -bw / 3 - bw);
            for (int k = 0; k < 5; k++) {
                int sy = r.y + r.h - (k + 1) * sh - k * sg;
                fill(px, pitch, R(bx, sy, bw, sh), k < n ? segc[k] : scale(segc[k], 1, 6));
            }
        }
        small_text(px, pitch, r.x + r.w / 2, r.y + r.h + 2, r.w + 2 * g_label_room(r), t->label, C_LABEL);
        break; }
    case ENC: {
        int rad = r.w / 2, cx = r.x + rad, cy = r.y + rad;
        disc(px, pitch, cx, cy, rad, t->down ? 0x5a5f69 : 0x40434b);
        disc(px, pitch, cx, cy, rad - 4, 0x2a2c32);
        disc(px, pitch, cx, cy, rad * 6 / 10, 0x34373e);
        double a = t->val * 3.14159265 / 12.0;
        disc(px, pitch, cx + (int)((rad - 9) * mcos(a)), cy + (int)((rad - 9) * msin(a)), 3, 0xd0d3d8);
        break; }
    }
}
static void draw_panel(uint8_t *px, uint32_t pitch) {
    struct rect sb = scrbtn;
    fill(px, pitch, strip, BG_STRIP);
    fill(px, pitch, body, BG_BODY);
    if (scr.y > 0 && DH >= DW) fill(px, pitch, R(0, 0, (int)DW, scr.y), 0);   /* camera gap */
    if (DH < DW) {   /* landscape: black beside the screen, the knob bar under it */
        fill(px, pitch, R(0, 0, scr.x, scr.h), 0);
        fill(px, pitch, R(scr.x + scr.w, 0, body.x - scr.x - scr.w, scr.h), 0);
        fill(px, pitch, kbar, BG_BODY);
    }
    for (int i = 0; i < nit; i++) draw_item(px, pitch, &it[i]);
    if (sb.w > 20 && sb.h > 20) {   /* SCREEN: the display window's own full-screen icon sits on this face */
        box(px, pitch, sb, C_BTN);
        small_text(px, pitch, sb.x + sb.w / 2, sb.y + sb.h + 2, sb.w + 6, "SCREEN", C_LABEL);
    }
}
static uint16_t xt[4096]; static int xt_w = -1;   /* screen column -> MPC pixel column */
static void draw_screen(uint8_t *px, uint32_t pitch, struct srcfb *s) {
    if (scr.y > 0) fill(px, pitch, (struct rect){ 0, 0, scr.w, scr.y }, 0);
    int sw = MPC_W < (int)s->w ? MPC_W : (int)s->w, sh = MPC_H < (int)s->h ? MPC_H : (int)s->h;
    for (int y = 0; y < scr.h; y++) {
        int dy = scr.y + y; if (dy < 0 || dy >= (int)DH) continue;
        int sy = y * MPC_H / scr.h; if (sy >= sh) sy = sh - 1;
        const uint32_t *in = (const uint32_t *)(s->px + (size_t)sy * s->pitch);
        uint32_t *out = (uint32_t *)(px + (size_t)dy * pitch);
        if (xt_w != scr.w) { xt_w = scr.w < 4096 ? scr.w : 4096; for (int x = 0; x < xt_w; x++) xt[x] = (uint16_t)(x * MPC_W / scr.w); }
        int xn = scr.w < xt_w ? scr.w : xt_w; if (scr.x + xn > (int)DW) xn = (int)DW - scr.x;
        uint32_t *o = out + scr.x;
        for (int x = 0; x < xn; x++) { int sx = xt[x]; o[x] = sx < sw ? in[sx] : 0; }   /* terminal: full screen, no menu mask */
    }
}
/* TAP TEMPO is a plain momentary button now: the add-on still calls this on its release, but we do nothing -
 * the official firmware draws its own BPM display, and we never hold or alter its frames. */
void panel_tap_release(void) {}

static void dirty(int i) {
    struct drm_mode_fb_dirty_cmd d = { .fb_id = our[i].id };
    dio(drmfd, DRM_IOCTL_MODE_DIRTYFB, &d);
}

static unsigned ncall;
static void dlog(const char *what, uint32_t fb, long ret) {
    ncall++;
    if (ncall <= 40 || ncall % 200 == 0 || ret < 0) {
        char b[64]; int n = 0; const char *p = what; while (*p && n < 40) b[n++] = *p++;
        b[n++] = ret < 0 ? '!' : ' '; b[n] = 0;
        shim_log(b, ret < 0 ? "failed errno" : "fb", ret < 0 ? (unsigned)errno : fb);
    }
}

/* ---------- display size changes (window resized / full screen) ----------
 * The program can't cope with the display changing size (the real MPC's screen never does), so the add-on
 * hides those announcements from it (udev wrapper below) and follows the new size itself: it keeps taking
 * the program's frames at the old size and shows them on its own buffers at the new size. */
static volatile int hotplug;
static uint32_t crtc_id, conn_id;
static struct drm_mode_modeinfo cur_mode; static int have_mode;
/* the preferred mode of one connector; 0 if it is not connected */
static int conn_mode(int fd, uint32_t id, struct drm_mode_modeinfo *out, int logit) {
    static struct drm_mode_modeinfo modes[32];
    struct drm_mode_get_connector c = { .connector_id = id };
    if (dio(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) return 0;      /* count_modes = 0: re-probe */
    if (c.connection != 1 || !c.count_modes) return 0;
    uint32_t n = c.count_modes > 32 ? 32 : c.count_modes;
    struct drm_mode_get_connector d = { .connector_id = id, .count_modes = n, .modes_ptr = (uint64_t)(uintptr_t)modes };
    if (dio(fd, DRM_IOCTL_MODE_GETCONNECTOR, &d) < 0 || d.count_modes > n) return 0;
    int pick = 0;
    for (uint32_t i = 0; i < d.count_modes; i++) if (modes[i].type & DRM_MODE_TYPE_PREFERRED) { pick = (int)i; break; }
    if (logit) shim_log("panel: connected output, size", 0, id * 100000000u + modes[pick].hdisplay * 10000u + modes[pick].vdisplay);
    *out = modes[pick];
    return 1;
}
/* after a display-change announcement: look at every output, follow the connected one */
static int scan_outputs(int fd, struct drm_mode_modeinfo *out) {
    uint32_t ids[32];
    struct drm_mode_card_res r = { 0 };
    if (dio(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0) return 0;
    if (r.count_connectors > 32) r.count_connectors = 32;
    struct drm_mode_card_res r2 = { .connector_id_ptr = (uint64_t)(uintptr_t)ids, .count_connectors = r.count_connectors };
    if (dio(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2) < 0) return 0;
    if (conn_mode(fd, conn_id, out, 0)) return 1;               /* ours is still connected */
    for (uint32_t i = 0; i < r2.count_connectors && i < 32; i++)
        if (ids[i] != conn_id && conn_mode(fd, ids[i], out, 1)) {
            shim_log("panel: picture moved to output", 0, ids[i]);
            conn_id = ids[i];
            /* find a display controller (CRTC) that can drive it */
            uint32_t encs[8], crtcs[32];
            struct drm_mode_get_connector c = { .connector_id = conn_id, .count_encoders = 8, .encoders_ptr = (uint64_t)(uintptr_t)encs };
            struct drm_mode_card_res r3 = { .crtc_id_ptr = (uint64_t)(uintptr_t)crtcs, .count_crtcs = r.count_crtcs > 32 ? 32 : r.count_crtcs };
            if (dio(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) >= 0 && c.count_encoders && dio(fd, DRM_IOCTL_MODE_GETRESOURCES, &r3) >= 0) {
                struct drm_mode_get_encoder e = { .encoder_id = encs[0] };
                if (dio(fd, DRM_IOCTL_MODE_GETENCODER, &e) >= 0)
                    for (uint32_t k = 0; k < r3.count_crtcs && k < 32; k++)
                        if (e.possible_crtcs & (1u << k)) { crtc_id = crtcs[k]; shim_log("panel: using display controller", 0, crtc_id); break; }
            }
            return 1;
        }
    shim_log("panel: no connected output found", 0, r2.count_connectors);
    return 0;
}
static volatile int udev_hot;
static int new_mode(int fd, struct drm_mode_modeinfo *out) {
    if (udev_hot) { udev_hot = 0; return scan_outputs(fd, out); }
    return conn_mode(fd, conn_id, out, 0);
}

/* Called from the add-on's ioctl() for DRM requests. Returns 1 if handled (*ret set). */
int panel_ioctl(int fd, unsigned long req, void *arg, long *ret) {
    if (!panel_on() || !arg) return 0;
    if (req == DRM_IOCTL_MODE_PAGE_FLIP || req == DRM_IOCTL_MODE_SETCRTC) {
        uint32_t *fbp = req == DRM_IOCTL_MODE_PAGE_FLIP ? &((struct drm_mode_crtc_page_flip *)arg)->fb_id : &((struct drm_mode_crtc *)arg)->fb_id;
        uint32_t fb = *fbp;
        static int first_set = 1;
        if (req == DRM_IOCTL_MODE_SETCRTC) {   /* remember which output and mode the program uses */
            struct drm_mode_crtc *c = arg;
            if (first_set && c->count_connectors && c->set_connectors_ptr) {
                shim_log("panel: program chose output", 0, *(uint32_t *)(uintptr_t)c->set_connectors_ptr);
                shim_log("panel: program chose controller", 0, c->crtc_id);
            }
            if (c->crtc_id && !crtc_id) crtc_id = c->crtc_id;   /* first time only: later the add-on decides */
            if (c->count_connectors && c->set_connectors_ptr && !conn_id) conn_id = *(uint32_t *)(uintptr_t)c->set_connectors_ptr;
            if (c->mode_valid && !have_mode) { cur_mode = c->mode; have_mode = 1; }
        }
        if (!fb || is_ours(fb)) { dlog("panel: program call passed through, fb", fb, 0); return 0; }
        struct srcfb *s = get_src(fd, fb);
        int modeset = 0;
        if (first_set && conn_id && crtc_id && have_mode) {   /* make sure the picture goes to the output the phone shows */
            first_set = 0;
            struct drm_mode_modeinfo m; uint32_t before = conn_id;
            udev_hot = 1;
            if (new_mode(fd, &m)) {
                shim_log("panel: showing on output", 0, conn_id);
                if (conn_id != before || m.hdisplay != cur_mode.hdisplay || m.vdisplay != cur_mode.vdisplay) { cur_mode = m; modeset = 1; }
            }
        }
        /* check the window size about once a second (and at once after a display-change announcement) */
        static long last_check;
        struct timespec now; syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now);
        long ms = now.tv_sec * 1000L + now.tv_nsec / 1000000L;
        if (ms - last_check > 1000) { last_check = ms; hotplug = 1; }
        if (hotplug && conn_id && crtc_id && have_mode) {
            hotplug = 0;
            struct drm_mode_modeinfo m;
            uint32_t before = conn_id;
            if (new_mode(fd, &m) && (m.hdisplay != cur_mode.hdisplay || m.vdisplay != cur_mode.vdisplay || conn_id != before)) {
                cur_mode = m; modeset = 1;
                shim_log("panel: window size changed to", 0, m.hdisplay * 10000u + m.vdisplay);
            }
        }
        uint32_t tw = have_mode ? cur_mode.hdisplay : (s ? s->w : 0), th = have_mode ? cur_mode.vdisplay : (s ? s->h : 0);
        if (!s || !make_ours(fd, tw, th)) { dlog("panel: could not map program frame, fb", fb, 0); return 0; }
        if (!nit) layout();
        last_src = fb;
        int b = front == 0 ? 1 : 0;
        busy = 1;
        draw_screen(our[b].px, our[b].pitch, s);
        if (drawn_ver[b] != pver) { draw_panel(our[b].px, our[b].pitch); drawn_ver[b] = pver; }   /* only after a change */
        busy = 0;
        *fbp = our[b].id;
        uint32_t prog_crtc = req == DRM_IOCTL_MODE_PAGE_FLIP ? ((struct drm_mode_crtc_page_flip *)arg)->crtc_id : ((struct drm_mode_crtc *)arg)->crtc_id;
        if (crtc_id && req == DRM_IOCTL_MODE_PAGE_FLIP) ((struct drm_mode_crtc_page_flip *)arg)->crtc_id = crtc_id;
        if (crtc_id && req == DRM_IOCTL_MODE_SETCRTC) {
            struct drm_mode_crtc *pc = arg; pc->crtc_id = crtc_id;
            if (pc->count_connectors && pc->set_connectors_ptr) *(uint32_t *)(uintptr_t)pc->set_connectors_ptr = conn_id;
        }
        if (modeset) {   /* switch the output to the new size with our buffer, then do the program's request on it */
            struct drm_mode_crtc c = { .set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id, .count_connectors = 1,
                                       .crtc_id = crtc_id, .fb_id = our[b].id, .mode_valid = 1, .mode = cur_mode };
            long r = dio(fd, DRM_IOCTL_MODE_SETCRTC, &c);
            dlog("panel: new size set, our fb", our[b].id, r);
            if (req == DRM_IOCTL_MODE_SETCRTC) ((struct drm_mode_crtc *)arg)->mode = cur_mode;
        } else if (req == DRM_IOCTL_MODE_SETCRTC && have_mode) ((struct drm_mode_crtc *)arg)->mode = cur_mode;
        *ret = dio(fd, req, arg);
        int e = errno;
        dlog(req == DRM_IOCTL_MODE_PAGE_FLIP ? "panel: flip, our fb" : "panel: setcrtc, our fb", our[b].id, *ret);
        errno = e;
        *fbp = fb;
        if (req == DRM_IOCTL_MODE_PAGE_FLIP) ((struct drm_mode_crtc_page_flip *)arg)->crtc_id = prog_crtc;
        else ((struct drm_mode_crtc *)arg)->crtc_id = prog_crtc;
        if (*ret >= 0) { front = b; if (nold) drop_old(); } else errno = e;
        return 1;
    }
    if (req == DRM_IOCTL_MODE_DIRTYFB && front >= 0) {
        struct drm_mode_fb_dirty_cmd *d = arg;
        if (d->fb_id != last_src) { dlog("panel: dirty on other fb (passed through)", d->fb_id, 0); return 0; }
        struct srcfb *s = get_src(fd, d->fb_id);
        if (!s) return 0;
        draw_screen(our[front].px, our[front].pitch, s);
        if (drawn_ver[front] != pver) { draw_panel(our[front].px, our[front].pitch); drawn_ver[front] = pver; }   /* panel changed */
        dirty(front);
        dlog("panel: dirty, our fb", our[front].id, 0);
        *ret = 0; return 1;
    }
    if (req == DRM_IOCTL_MODE_GETCRTC || req == DRM_IOCTL_MODE_GETPLANE) {
        *ret = dio(fd, req, arg);
        uint32_t *fbp = req == DRM_IOCTL_MODE_GETCRTC ? &((struct drm_mode_crtc *)arg)->fb_id : &((struct drm_mode_get_plane *)arg)->fb_id;
        if (*ret >= 0 && *fbp && is_ours(*fbp)) *fbp = last_src;
        return 1;
    }
    if (req == DRM_IOCTL_MODE_RMFB) {
        uint32_t id = *(uint32_t *)arg;
        for (int i = 0; i < 8; i++) if (src[i].id == id && src[i].px) { syscall(SYS_munmap, src[i].px, (long)src[i].pitch * src[i].h); src[i].px = 0; src[i].id = 0; }
    }
    return 0;
}

/* ---------- MIDI out to the fake control chip (mpc.py ctl listens on /tmp/mpcctl-in.sock) ---------- */
static int msock = -1;
/* Never block: this runs on the program's main thread, which also feeds the fake control chip.
 * Knob moves that can't go out right away are dropped (the next one carries the newer value);
 * pad and button presses are retried for up to ~50 ms. */
static void midi(int a, int b, int c) {
    if (msock < 0) msock = (int)syscall(SYS_socket, AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    const char *p = "/tmp/mpcctl-in.sock"; for (int i = 0; p[i]; i++) sa.sun_path[i] = p[i];
    unsigned char m[4] = { 8, (unsigned char)a, (unsigned char)b, (unsigned char)c };
    int tries = (a & 0xF0) == 0xB0 ? 1 : 50;
    for (int i = 0; i < tries; i++) {
        if (syscall(SYS_sendto, msock, m, 4, MSG_DONTWAIT, &sa, sizeof sa) >= 0) return;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return;
        struct timespec ts = { 0, 1000000 }; syscall(SYS_nanosleep, &ts, 0);
    }
}

/* ---------- touch ---------- */
static struct { int item, x0, y0, moved; } own[16];
static void redraw(struct item *t) {
    if (front < 0) return;
    draw_item(our[front].px, our[front].pitch, t);
    pver++; drawn_ver[front] = 0;
    dirty(front);
}
static int hit(int x, int y) {
    for (int i = 0; i < nit; i++) {
        if (it[i].kind == BANK || it[i].kind == METER) continue;
        struct rect r = it[i].r; int pad = it[i].kind == KNOB || it[i].kind == FADER ? 8 : 0;
        if (x >= r.x - pad && x < r.x + r.w + pad && y >= r.y - pad && y < r.y + r.h + pad) return i;
    }
    return -1;
}
static void set_val(struct item *t, int v) {
    if (v < 0) v = 0; if (v > 127) v = 127;
    if (v != t->val) { t->val = v; midi(0xB0, t->num, v); }
}
static void press(int slot, int x, int y) {
    int i = hit(x, y); own[slot].item = i; own[slot].x0 = x; own[slot].y0 = y; own[slot].moved = 0;
    if (i < 0) return;
    struct item *t = &it[i]; t->down = 1; t->sx = x; t->sy = y; t->sv = t->val;
    if (t->kind == PAD) midi(0x99, t->num, 110);
    else if (t->kind == BTN) midi(0x90, t->num, 127);
    else if (t->kind == FADER) set_val(t, (t->r.y + t->r.h - y) * 127 / (t->r.h > 1 ? t->r.h : 1));
    if (t->kind == BTN && t->num == N_SHIFT) {   /* snapshot the real light state; reveal the SHIFT lights together after a short delay */
        for (int j = 0; j < 128; j++) { btn_pre[j] = btn_raw[j]; btn_shift_target[j] = btn_raw[j]; }
        shift_show_at = now_ms() + SHIFT_REVEAL_MS; shift_shown = 0; shift_held = 1;
        tap_clock_start();   /* make sure the 50 ms clock runs so the reveal fires even with no more LED data */
    }
    redraw(t);
}
static void move(int slot, int x, int y) {
    int i = own[slot].item; if (i < 0) return;
    struct item *t = &it[i];
    if (x - own[slot].x0 > 6 || own[slot].x0 - x > 6 || y - own[slot].y0 > 6 || own[slot].y0 - y > 6) own[slot].moved = 1;
    if (t->kind == KNOB) { set_val(t, t->sv + (t->sy - y) * 127 / 160); redraw(t); }   /* drag up = turn right */
    else if (t->kind == FADER) { set_val(t, (t->r.y + t->r.h - y) * 127 / (t->r.h > 1 ? t->r.h : 1)); redraw(t); }
    else if (t->kind == ENC) {   /* drag up / right = turn right, one step per 12 pixels */
        int d = ((t->sy - y) + (x - t->sx)) / 12;
        while (d > 0) { midi(0xB0, 100, 1); d--; t->sy -= 12; t->val++; }
        while (d < 0) { midi(0xB0, 100, 127); d++; t->sy += 12; t->val--; }
        redraw(t);
    }
}
static long now_ms(void) { struct timespec t; syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }
/* a small clock so the release goes out even when the MPC is idle (no new frames, no touches) */
static void mic_check(void);
static void *tap_clock(void *u) {
    (void)u;
    for (;;) {
        struct timespec ts = { 0, 20000000 }; syscall(SYS_nanosleep, &ts, 0); mic_check(); shift_reveal();
    }
    return 0;
}
static void tap_clock_start(void) {
    static int started;
    if (started) return; started = 1;
    pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, tap_clock, 0) != 0) started = 0;
    pthread_attr_destroy(&a);
}
static void release(int slot) {
    int i = own[slot].item; own[slot].item = -1; if (i < 0) return;
    struct item *t = &it[i]; t->down = 0;
    if (t->kind == BTN && t->num == N_SHIFT) {   /* restore every button to its pre-SHIFT state and redraw now */
        shift_held = 0; shift_shown = 0;
        for (int j = 0; j < 128; j++) if (!led_instant(j)) { btn_led[j] = btn_pre[j]; btn_raw[j] = btn_pre[j]; }
        for (int j = 0; j < nit; j++) if (it[j].kind == BTN && it[j].label) redraw(&it[j]);
    }
    if (t->kind == PAD) midi(0x89, t->num, 0);
    else if (t->kind == BTN) midi(0x80, t->num, 0);
    else if (t->kind == ENC && !own[slot].moved) { midi(0x90, N_ENCPUSH, 127); midi(0x80, N_ENCPUSH, 0); }   /* tap = push */
    redraw(t);
}

/* libinput wrappers (the program reads the touchscreen through libinput) */
struct libinput; struct libinput_event; struct libinput_event_touch;
enum { T_DOWN = 500, T_UP = 501, T_MOTION = 502, T_CANCEL = 503 };
static struct libinput_event *(*r_get)(struct libinput *);
static int (*r_type)(struct libinput_event *);
static struct libinput_event_touch *(*r_touch)(struct libinput_event *);
static double (*r_x)(struct libinput_event_touch *, uint32_t), (*r_y)(struct libinput_event_touch *, uint32_t);
static int32_t (*r_slot)(struct libinput_event_touch *);
static void (*r_destroy)(struct libinput_event *);
static int resolve(void) {
    if (r_get) return 1;
    for (int i = 0; i < 16; i++) own[i].item = -1;
    r_type = (int (*)(struct libinput_event *))dlsym(RTLD_NEXT, "libinput_event_get_type");
    r_touch = (struct libinput_event_touch *(*)(struct libinput_event *))dlsym(RTLD_NEXT, "libinput_event_get_touch_event");
    r_x = (double (*)(struct libinput_event_touch *, uint32_t))dlsym(RTLD_NEXT, "libinput_event_touch_get_x_transformed");
    r_y = (double (*)(struct libinput_event_touch *, uint32_t))dlsym(RTLD_NEXT, "libinput_event_touch_get_y_transformed");
    r_slot = (int32_t (*)(struct libinput_event_touch *))dlsym(RTLD_NEXT, "libinput_event_touch_get_slot");
    r_destroy = (void (*)(struct libinput_event *))dlsym(RTLD_NEXT, "libinput_event_destroy");
    r_get = (struct libinput_event *(*)(struct libinput *))dlsym(RTLD_NEXT, "libinput_get_event");
    return r_get && r_type && r_touch && r_x && r_y && r_slot && r_destroy;
}
static int touch_keep;   /* devrelay: turn touches into panel presses while another program draws the screen */
/* devrelay calls this about once a second while a program other than the stock app (one that doesn't read the
 * touchscreen itself, e.g. Melodee) has the display: the panel's touch areas follow the display's size, without
 * taking the display. on = 0: the stock app reads the touchscreen itself again. */
void panel_touch_follow(int on) {
    static int fd = -1;
    touch_keep = 0;
    if (!on) return;
    for (int c = 0; c < 4 && fd < 0; c++) {
        char path[] = "/dev/dri/card0"; path[13] = (char)('0' + c);
        fd = (int)syscall(SYS_openat, AT_FDCWD, path, O_RDWR | O_CLOEXEC, 0);
        /* the first to open a display nobody owns becomes its owner (DRM master): give that up at once, or the
         * program that draws the screen can't take it ("Acquire display ... Device or resource busy") */
        if (fd >= 0) syscall(SYS_ioctl, fd, DRM_IO(0x1f), 0);
        struct drm_mode_modeinfo m;
        if (fd >= 0 && !scan_outputs(fd, &m)) { syscall(SYS_close, fd); fd = -1; }
    }
    struct drm_mode_modeinfo m;
    if (fd < 0 || !scan_outputs(fd, &m)) return;
    if (!nit || m.hdisplay != DW || m.vdisplay != DH) { DW = m.hdisplay; DH = m.vdisplay; layout(); }
    touch_keep = 1;
}
static int slot_of(struct libinput_event_touch *te) { int s = r_slot(te) + 1; return s < 0 ? 0 : s > 15 ? 15 : s; }
static int touch_mapped;

struct libinput_event *libinput_get_event(struct libinput *li) {
    if (!resolve()) { errno = ENOSYS; return 0; }
    for (;;) {
        struct libinput_event *ev = r_get(li);
        if (!ev || !panel_on() || (front < 0 && !touch_keep)) return ev;
        int t = r_type(ev);
        if (t < T_DOWN || t > T_CANCEL) return ev;
        struct libinput_event_touch *te = r_touch(ev);
        int s = slot_of(te);
        if (t == T_DOWN) {
            int x = (int)r_x(te, DW), y = (int)r_y(te, DH);
            int on_screen = x >= scr.x && x < scr.x + scr.w && y >= scr.y && y < scr.y + scr.h;
            if (on_screen && hit(x, y) >= 0) on_screen = 0;   /* B1-B3 areas at the top of the screen */
            if (!on_screen) { press(s, x, y); own[s].item = own[s].item < 0 ? -2 : own[s].item; r_destroy(ev); continue; }
            own[s].item = -1;
        } else if (own[s].item != -1) {
            if (t == T_MOTION) move(s, (int)r_x(te, DW), (int)r_y(te, DH));
            else { if (own[s].item >= 0) release(s); own[s].item = -1; }
            r_destroy(ev); continue;
        }
        return ev;
    }
}
/* udev: the program watches for display changes; swallow "drm" ones and let the panel follow them */
struct udev_monitor; struct udev_device;
static struct udev_device *(*r_recv)(struct udev_monitor *);
static const char *(*r_subsys)(struct udev_device *);
static struct udev_device *(*r_dunref)(struct udev_device *);
struct udev_device *udev_monitor_receive_device(struct udev_monitor *m) {
    if (!r_recv) {
        r_recv = (struct udev_device *(*)(struct udev_monitor *))dlsym(RTLD_NEXT, "udev_monitor_receive_device");
        r_subsys = (const char *(*)(struct udev_device *))dlsym(RTLD_NEXT, "udev_device_get_subsystem");
        r_dunref = (struct udev_device *(*)(struct udev_device *))dlsym(RTLD_NEXT, "udev_device_unref");
        if (!r_recv) return 0;
    }
    struct udev_device *d = r_recv(m);
    if (d && r_subsys && r_dunref) {
        const char *ss = r_subsys(d);
        if (ss && ss[0] == 'd' && ss[1] == 'r' && ss[2] == 'm' && ss[3] == 0) {
            hotplug = 1; udev_hot = 1;
            shim_log("panel: display change hidden from the program", 0, 0);
            r_dunref(d); errno = EAGAIN; return 0;
        }
    }
    return d;
}

/* screen touches: give the program coordinates in its own 320x240 picture */
static double map_coord(double v, uint32_t size, int disp, int off, int len, int mpc) {
    double m = (v - off) * mpc / len;
    if (m < 0) m = 0;
    if (m > mpc - 1) m = mpc - 1;
    return size <= (uint32_t)mpc + 8 ? m * size / mpc : m * size / disp;
}
double libinput_event_touch_get_x_transformed(struct libinput_event_touch *te, uint32_t w) {
    if (!resolve()) return 0;
    if (!panel_on() || front < 0) return r_x(te, w);
    if (!touch_mapped) { touch_mapped = 1; shim_log("panel: program asks touch width", 0, w); }
    return map_coord(r_x(te, DW), w, (int)DW, scr.x, scr.w, MPC_W);
}
double libinput_event_touch_get_y_transformed(struct libinput_event_touch *te, uint32_t h) {
    if (!resolve()) return 0;
    if (!panel_on() || front < 0) return r_y(te, h);
    return map_coord(r_y(te, DH), h, (int)DH, scr.y, scr.h, MPC_H);
}

/* ---------- the program's input format, for audiobridge ----------
 * The microphone reaches the program through an ALSA "file" plugin straight at the program's end (any plugin layer
 * in between reads ahead and drops sound). So audiobridge must write exactly the format the program chose; this
 * notes it in /tmp/mpc-mic.fmt ("<alsa format number> <channels> <rate>") whenever the program sets up its input. */
int snd_pcm_hw_params(void *pcm, void *params) {
    static int (*real)(void *, void *);
    static int (*stream)(void *);
    static int (*gfmt)(const void *, int *), (*gch)(const void *, unsigned *), (*grate)(const void *, unsigned *, int *);
    if (!real) {
        real = (int (*)(void *, void *))dlsym(RTLD_NEXT, "snd_pcm_hw_params");
        stream = (int (*)(void *))dlsym(RTLD_NEXT, "snd_pcm_stream");
        gfmt = (int (*)(const void *, int *))dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_format");
        gch = (int (*)(const void *, unsigned *))dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_channels");
        grate = (int (*)(const void *, unsigned *, int *))dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_rate");
        if (!real) return -38;   /* ENOSYS */
    }
    int r = real(pcm, params);
    if (r == 0 && stream && gfmt && gch && grate && stream(pcm) == 1 /* capture */) {
        int f = -1, dir = 0; unsigned ch = 0, rate = 0;
        gfmt(params, &f); gch(params, &ch); grate(params, &rate, &dir);
        char line[48]; int n = 0;
        unsigned v[3] = { (unsigned)f, ch, rate };
        for (int k = 0; k < 3; k++) {   /* "%d %u %u\n" without stdio */
            char d[12]; int m = 0; unsigned x = v[k];
            do { d[m++] = (char)('0' + x % 10); x /= 10; } while (x);
            while (m) line[n++] = d[--m];
            line[n++] = k < 2 ? ' ' : '\n';
        }
        long fd = syscall(SYS_openat, AT_FDCWD, "/tmp/mpc-mic.fmt", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) { syscall(SYS_write, fd, line, n); syscall(SYS_close, fd); }
        shim_log("mic: program input format/channels/rate", line, 0);
    }
    return r;
}

/* ---------- present a /dev/fb0 frame to the Terminal display ----------
 * For programs that draw to the legacy framebuffer instead of DRM (SampleBoot's boot menu, SampQOL's
 * splash). fbshim calls this with one 320x240 BGRX frame whenever such a program writes a full frame to
 * /dev/fb0. The program never sets up DRM, so we open the display ourselves - reusing the same output
 * scan, buffers and panel the normal (DRM) path uses - then show the frame in the MPC screen area with
 * the on-screen panel around it (so the panel's encoder knob can drive the menu). Returns 1 once up. */
#ifndef DRM_IOCTL_SET_MASTER
#define DRM_IOCTL_SET_MASTER DRM_IO(0x1e)
#endif
static int fb_up;
int panel_fb_present(const unsigned char *frame) {
    if (!fb_up) {
        for (int c = 0; c < 4 && !fb_up; c++) {
            char path[] = "/dev/dri/card0"; path[13] = (char)('0' + c);
            int fd = (int)syscall(SYS_openat, AT_FDCWD, path, O_RDWR | O_CLOEXEC, 0);
            if (fd < 0) continue;
            dio(fd, DRM_IOCTL_SET_MASTER, 0);           /* usually already master as the only opener */
            struct drm_mode_modeinfo m;
            if (scan_outputs(fd, &m) && make_ours(fd, m.hdisplay, m.vdisplay)) {
                cur_mode = m; have_mode = 1; fb_up = 1;
                shim_log("panel: fb0 display up", path, (unsigned)(m.hdisplay * 10000 + m.vdisplay));
            } else {
                syscall(SYS_close, fd);
            }
        }
        if (!fb_up) { shim_log("panel: fb0 display setup failed - no DRM output", 0, 0); return 0; }
    }
    {   /* follow the window: about once a second, as the app's display path does (at power-on the phone's
         * display window may not have its final size yet - it then shows a squashed, cut-off picture) */
        static long last_check;
        struct timespec now; syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now);
        long ms = now.tv_sec * 1000L + now.tv_nsec / 1000000L;
        struct drm_mode_modeinfo m;
        if (ms - last_check > 1000) {
            last_check = ms;
            if (new_mode(drmfd, &m) && (m.hdisplay != cur_mode.hdisplay || m.vdisplay != cur_mode.vdisplay)
                && make_ours(drmfd, m.hdisplay, m.vdisplay)) {
                cur_mode = m;
                shim_log("panel: fb0 window size changed to", 0, m.hdisplay * 10000u + m.vdisplay);
            }
        }
    }
    struct srcfb s = { 0, MPC_W, MPC_H, MPC_W * 4, (uint8_t *)frame };
    int b = front == 1 ? 0 : 1;
    busy = 1;
    if (!nit) layout();
    draw_screen(our[b].px, our[b].pitch, &s);
    draw_panel(our[b].px, our[b].pitch);
    busy = 0;
    struct drm_mode_crtc cc = { .set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id, .count_connectors = 1,
                                .crtc_id = crtc_id, .fb_id = our[b].id, .mode_valid = 1, .mode = cur_mode };
    if (dio(drmfd, DRM_IOCTL_MODE_SETCRTC, &cc) < 0) {   /* lost the display to another program? take it back */
        dio(drmfd, DRM_IOCTL_SET_MASTER, 0);
        if (dio(drmfd, DRM_IOCTL_MODE_SETCRTC, &cc) < 0) {
            static int nf; if (nf++ < 5) shim_log("panel: fb0 SETCRTC failed (another program has the display)", 0, 0);
            return 0;
        }
    }
    front = b;
    return 1;
}

/* Give the display back: devrelay (the static-program helper) calls this the moment the MPC app starts, so
 * there is one display owner at a time. Frees our buffers and closes the device (which drops DRM master). */
#ifndef DRM_IOCTL_DROP_MASTER
#define DRM_IOCTL_DROP_MASTER DRM_IO(0x1f)
#endif
void panel_fb_release(void) {
    if (!fb_up) return;
    busy = 1;
    for (int i = 0; i < nour; i++) drop(&our[i]);
    drop_old();
    nour = 0; front = -1; nit = 0;
    dio(drmfd, DRM_IOCTL_DROP_MASTER, 0);
    syscall(SYS_close, drmfd);
    drmfd = -1; fb_up = 0; have_mode = 0;
    busy = 0;
    shim_log("panel: fb0 display released", 0, 0);
}
