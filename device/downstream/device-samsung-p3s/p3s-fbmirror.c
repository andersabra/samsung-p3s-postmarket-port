/*
 * p3s-fbmirror - show the console framebuffer on the Galaxy S21 Ultra panel.
 *
 * The panel is a command-mode DSI panel driven by Samsung's dpu (decon) fbdev
 * driver. That driver only sends a frame when asked to (FBIOPAN_DISPLAY),
 * ignores write(), has no fillrect/imageblit (so fbcon cannot draw on it), and
 * powers the panel off whenever any user closes the device. So this daemon is
 * the one and only user of the decon framebuffer: it unblanks it once, keeps it
 * open, and copies changed rows from the source framebuffer (simple-framebuffer,
 * where the kernel console draws) into it, panning after each change.
 *
 * The source may be smaller than the panel (a device tree with a smaller console
 * framebuffer keeps text away from damaged panel edges); it is drawn at the top
 * right unless -x/-y give other offsets. The uncovered area stays black.
 *
 * The power button switches the panel off and on (a real panel power-down via
 * FBIOBLANK, not black pixels), and with -t it also switches off after that many
 * seconds without a key press. Any key press counts as activity; only the power
 * button switches the panel back on.
 *
 * usage: p3s-fbmirror [-c] [-i interval_ms] [-t idle_s] [-x offset_px] [-y offset_px] [-s /dev/fbSRC] [-d /dev/fbDST]
 *   -c  after the panel is up, reset the console: clear fbcon's stuck "graphics"
 *       state (KD_GRAPHICS -> KD_TEXT on the foreground VT), unblank it and turn
 *       off the blank timer, so text is drawn into the source framebuffer
 *   -t  switch the panel off after this many seconds without a key press (0 = never)
 * Without -s/-d it picks the "simple" framebuffer as source and the first other
 * framebuffer at least as large as destination.
 */
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

struct fb {
    int fd;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    uint8_t *mem;
    size_t len;
};

static int fb_open(struct fb *b, const char *path) {
    b->fd = open(path, O_RDWR | O_CLOEXEC);
    if (b->fd < 0) return -1;
    if (ioctl(b->fd, FBIOGET_VSCREENINFO, &b->v) || ioctl(b->fd, FBIOGET_FSCREENINFO, &b->f)) {
        close(b->fd);
        b->fd = -1;
        return -1;
    }
    return 0;
}

static int fb_map(struct fb *b) {
    b->len = (size_t)b->f.line_length * b->v.yres;
    if (!b->len || b->len > b->f.smem_len) b->len = b->f.smem_len;
    b->mem = mmap(0, b->len, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
    if (b->mem == MAP_FAILED) { b->mem = 0; return -1; }
    return 0;
}

/* Candidates are found through sysfs, not by opening them: closing a decon fb
 * powers the panel off. */
static int sysfs_read(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)n, f)) { fclose(f); return -1; }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

static unsigned long vsize_w(const char *vs) { return strtoul(vs, 0, 10); }

static unsigned long vsize_h(const char *vs) {
    const char *c = strchr(vs, ',');
    return c ? strtoul(c + 1, 0, 10) : 0;
}

static void find_fbs(char *src, char *dst, size_t n) {
    char p[96], name[64], size[64], ssize[64] = "";
    int s = -1;
    for (int i = 0; i < 8; i++) {
        snprintf(p, sizeof p, "/sys/class/graphics/fb%d/name", i);
        if (sysfs_read(p, name, sizeof name)) continue;
        if (!strcmp(name, "simple")) {
            s = i;
            snprintf(p, sizeof p, "/sys/class/graphics/fb%d/virtual_size", i);
            sysfs_read(p, ssize, sizeof ssize);
            if (!*src) snprintf(src, n, "/dev/fb%d", i);
            break;
        }
    }
    if (*dst || s < 0) return;
    for (int i = 0; i < 8; i++) {
        if (i == s) continue;
        snprintf(p, sizeof p, "/sys/class/graphics/fb%d/name", i);
        if (sysfs_read(p, name, sizeof name)) continue;
        snprintf(p, sizeof p, "/sys/class/graphics/fb%d/virtual_size", i);
        if (sysfs_read(p, size, sizeof size)) continue;
        /* decon0 is 1440x6400 virtual (double buffered); the DP decons are 640x960 */
        if (vsize_h(ssize) && vsize_w(size) >= vsize_w(ssize) && vsize_h(size) >= vsize_h(ssize)) {
            snprintf(dst, n, "/dev/fb%d", i);
            return;
        }
    }
}

static void console_fixup(void) {
    int t = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (t < 0) { fprintf(stderr, "p3s-fbmirror: /dev/tty0: %s\n", strerror(errno)); return; }
    /* only a real KD_GRAPHICS -> KD_TEXT change clears fbcon's per-fb graphics flag */
    if (ioctl(t, KDSETMODE, KD_GRAPHICS) || ioctl(t, KDSETMODE, KD_TEXT))
        fprintf(stderr, "p3s-fbmirror: KDSETMODE: %s\n", strerror(errno));
    /* ESC[9;0] = no blank timeout, ESC[13] = unblank */
    static const char esc[] = "\033[9;0]\033[13]";
    if (write(t, esc, sizeof esc - 1) < 0) fprintf(stderr, "p3s-fbmirror: tty0 write: %s\n", strerror(errno));
    close(t);
}

/* Key input: every /dev/input/event* that has keys. Not grabbed, so the console
 * still sees them. */
#define MAXIN 16
static struct pollfd in_fd[MAXIN];
static int in_power[MAXIN], n_in;

static int test_bit(const unsigned long *b, int n) {
    return (b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

static void open_inputs(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return;
    for (struct dirent *e; (e = readdir(dir)) && n_in < MAXIN;) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long ev[1] = {0}, keys[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0 || !test_bit(ev, EV_KEY) ||
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0) { close(fd); continue; }
        in_fd[n_in].fd = fd;
        in_fd[n_in].events = POLLIN;
        in_power[n_in] = test_bit(keys, KEY_POWER);
        n_in++;
    }
    closedir(dir);
}

/* Wait up to ms for key input. Returns 1 if any key was pressed, 2 if it was the
 * power button. */
static int wait_keys(int ms) {
    if (!n_in) { usleep((useconds_t)ms * 1000); return 0; }
    int r = 0;
    if (poll(in_fd, (nfds_t)n_in, ms) <= 0) return 0;
    for (int i = 0; i < n_in; i++) {
        if (!(in_fd[i].revents & POLLIN)) continue;
        struct input_event e[16];
        ssize_t n;
        while ((n = read(in_fd[i].fd, e, sizeof e)) > 0)
            for (size_t k = 0; k < (size_t)n / sizeof e[0]; k++) {
                if (e[k].type != EV_KEY || e[k].value != 1) continue;
                if (e[k].code == KEY_POWER && in_power[i]) r = 2;
                else if (!r) r = 1;
            }
    }
    return r;
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

int main(int argc, char **argv) {
    char src[32] = "", dst[32] = "";
    int interval = 50, xoff = -1, yoff = 0, fixcon = 0, idle_s = 0;
    for (int c; (c = getopt(argc, argv, "ci:t:x:y:s:d:")) != -1;) {
        if (c == 'c') fixcon = 1;
        else if (c == 't') idle_s = atoi(optarg);
        else if (c == 'i') interval = atoi(optarg);
        else if (c == 'x') xoff = atoi(optarg);
        else if (c == 'y') yoff = atoi(optarg);
        else if (c == 's') snprintf(src, sizeof src, "%s", optarg);
        else if (c == 'd') snprintf(dst, sizeof dst, "%s", optarg);
        else {
            fprintf(stderr, "usage: %s [-c] [-i ms] [-t s] [-x px] [-y px] [-s /dev/fbN] [-d /dev/fbN]\n", argv[0]);
            return 2;
        }
    }
    if (interval < 5) interval = 5;

    /* wait up to 30 s for both framebuffers (dpu may still be probing) */
    for (int t = 0; t < 300 && (!*src || !*dst); t++) {
        find_fbs(src, dst, sizeof src);
        if (!*src || !*dst) usleep(100000);
    }
    if (!*src || !*dst) { fprintf(stderr, "p3s-fbmirror: no source/destination framebuffer found\n"); return 1; }

    struct fb s = {0}, d = {0};
    if (fb_open(&s, src) || fb_map(&s)) { fprintf(stderr, "p3s-fbmirror: %s: %s\n", src, strerror(errno)); return 1; }
    if (fb_open(&d, dst)) { fprintf(stderr, "p3s-fbmirror: %s: %s\n", dst, strerror(errno)); return 1; }
    if (s.v.bits_per_pixel != 32 || d.v.bits_per_pixel != 32 || s.v.yres > d.v.yres || s.v.xres > d.v.xres) {
        fprintf(stderr, "p3s-fbmirror: unsupported formats %s %ux%u/%u vs %s %ux%u/%u\n", src, s.v.xres, s.v.yres,
                s.v.bits_per_pixel, dst, d.v.xres, d.v.yres, d.v.bits_per_pixel);
        return 1;
    }
    if (xoff < 0) xoff = (int)(d.v.xres - s.v.xres);
    if ((uint32_t)xoff + s.v.xres > d.v.xres) xoff = (int)(d.v.xres - s.v.xres);
    if (yoff < 0 || (uint32_t)yoff + s.v.yres > d.v.yres) yoff = (int)(d.v.yres - s.v.yres);
    if (ioctl(d.fd, FBIOBLANK, FB_BLANK_UNBLANK)) fprintf(stderr, "p3s-fbmirror: unblank: %s\n", strerror(errno));
    if (fb_map(&d)) { fprintf(stderr, "p3s-fbmirror: mmap %s: %s\n", dst, strerror(errno)); return 1; }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);

    open_inputs();
    fprintf(stderr, "p3s-fbmirror: mirroring %s (%ux%u) -> %s (%ux%u) at x=%d y=%d, every %d ms; %d key inputs, idle off %d s\n",
            src, s.v.xres, s.v.yres, dst, d.v.xres, d.v.yres, xoff, yoff, interval, n_in, idle_s);

    /* everything the source does not cover: opaque black */
    for (uint32_t y = 0; y < d.v.yres; y++) {
        uint32_t *row = (uint32_t *)(d.mem + (size_t)y * d.f.line_length);
        for (uint32_t x = 0; x < d.v.xres; x++) row[x] = 0xff000000u;
    }

    /* fbcon/simplefb may leave the alpha byte at 0, and decon blends with per-pixel
     * alpha, so force it opaque. A shadow copy of the source detects changes. */
    const uint32_t amask = s.v.transp.length ? (uint32_t)(((1ull << s.v.transp.length) - 1) << s.v.transp.offset)
                                             : 0xff000000u;
    const size_t rowbytes = (size_t)s.v.xres * 4;
    uint8_t *shadow = malloc(rowbytes * s.v.yres);
    if (!shadow) { fprintf(stderr, "p3s-fbmirror: out of memory\n"); return 1; }

    int first = 1, panel_on = 1;
    long last_key = now_ms();
    while (!stop) {
        long t0 = now_ms();
        if (!panel_on) {
            /* panel off: nothing to draw, only wait for the power button */
            if (wait_keys(1000) == 2) {
                if (ioctl(d.fd, FBIOBLANK, FB_BLANK_UNBLANK))
                    fprintf(stderr, "p3s-fbmirror: unblank: %s\n", strerror(errno));
                panel_on = 1;
                first = 1; /* redraw everything */
                last_key = now_ms();
            }
            continue;
        }
        int changed = first;
        for (uint32_t y = 0; y < s.v.yres; y++) {
            const uint8_t *a = s.mem + (size_t)y * s.f.line_length;
            uint8_t *sh = shadow + (size_t)y * rowbytes;
            if (!first && !memcmp(a, sh, rowbytes)) continue;
            memcpy(sh, a, rowbytes);
            const uint32_t *in = (const uint32_t *)sh;
            uint32_t *out = (uint32_t *)(d.mem + (size_t)(y + yoff) * d.f.line_length) + xoff;
            for (uint32_t x = 0; x < s.v.xres; x++) out[x] = in[x] | amask;
            changed = 1;
        }
        if (changed) {
            d.v.xoffset = 0;
            d.v.yoffset = 0;
            if (ioctl(d.fd, FBIOPAN_DISPLAY, &d.v) && first)
                fprintf(stderr, "p3s-fbmirror: pan: %s\n", strerror(errno));
            if (first && fixcon) console_fixup();
            first = 0;
        }
        long left = interval - (now_ms() - t0);
        int k = wait_keys(left > 0 ? (int)left : 0);
        if (k) last_key = now_ms();
        if (k == 2 || (idle_s > 0 && now_ms() - last_key >= idle_s * 1000L)) {
            if (ioctl(d.fd, FBIOBLANK, FB_BLANK_POWERDOWN))
                fprintf(stderr, "p3s-fbmirror: powerdown: %s\n", strerror(errno));
            panel_on = 0;
        }
    }
    /* leaving: closing the decon fb powers the panel off */
    return 0;
}
