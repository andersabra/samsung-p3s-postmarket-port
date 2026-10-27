/*
 * p3s-console-size - set the size of the console framebuffer (simple-framebuffer in
 * /chosen) of the Galaxy S21 Ultra device tree, for panels with damaged edges.
 *
 * The device tree gives a full-panel 1440x3200 console. A smaller console is drawn at
 * the top right of the panel by p3s-fbmirror, which keeps text away from a damaged
 * left or bottom edge. The input can be a vendor_boot.img or a .dtb; a new file of
 * the same size is written, the input is never changed.
 *
 * usage: p3s-console-size IN OUT WIDTH HEIGHT
 *   e.g. p3s-console-size /boot/vendor_boot.img /tmp/vendor_boot-small.img 1280 2560
 *        doas dd if=/tmp/vendor_boot-small.img of=/dev/disk/by-partlabel/vendor_boot
 *        (zero the partition first if the new image is smaller than the old one)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* Patch width/height/stride/reg size of /chosen/framebuffer* in every FDT in buf. */
static int patch(uint8_t *buf, size_t len, uint32_t w, uint32_t h) {
    int n = 0;
    for (size_t i = 0; i + 40 <= len; i++) {
        if (be32(buf + i) != 0xd00dfeed) continue;
        uint32_t tot = be32(buf + i + 4), ost = be32(buf + i + 8), ostr = be32(buf + i + 12);
        if (tot < 1000 || tot > 4000000 || i + tot > len || ost >= tot || ostr >= tot) continue;
        const uint8_t *strs = buf + i + ostr;
        size_t p = i + ost, end = i + tot;
        int depth = 0, chosen = -1, fb = -1;
        while (p + 4 <= end) {
            uint32_t tag = be32(buf + p);
            p += 4;
            if (tag == 1) { /* BEGIN_NODE */
                const char *name = (const char *)buf + p;
                size_t l = strnlen(name, end - p);
                depth++;
                if (depth == 2 && !strcmp(name, "chosen")) chosen = depth;
                else if (chosen == 2 && depth == 3 && !strncmp(name, "framebuffer", 11)) fb = depth;
                p = (p + l + 1 + 3) & ~(size_t)3;
            } else if (tag == 2) { /* END_NODE */
                if (fb == depth) fb = -1;
                if (chosen == depth) chosen = -1;
                depth--;
            } else if (tag == 3) { /* PROP */
                uint32_t l = be32(buf + p), no = be32(buf + p + 4);
                p += 8;
                if (fb > 0) {
                    const char *nm = (const char *)strs + no;
                    uint32_t v = !strcmp(nm, "width") ? w : !strcmp(nm, "height") ? h : !strcmp(nm, "stride") ? w * 4 : 0;
                    if (v && l == 4) {
                        printf("%s: %u -> %u\n", nm, be32(buf + p), v);
                        put32(buf + p, v);
                        n++;
                    } else if (!strcmp(nm, "reg") && l == 12) {
                        printf("reg size: 0x%x -> 0x%x\n", be32(buf + p + 8), w * h * 4);
                        put32(buf + p + 8, w * h * 4);
                        n++;
                    }
                }
                p = (p + l + 3) & ~(size_t)3;
            } else if (tag == 4) { /* NOP */
            } else break;          /* END or garbage */
        }
        i += tot - 1;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s IN OUT WIDTH HEIGHT   (panel: 1440x3200, e.g. 1280 2560)\n", argv[0]);
        return 2;
    }
    uint32_t w = (uint32_t)strtoul(argv[3], 0, 10), h = (uint32_t)strtoul(argv[4], 0, 10);
    if (!w || !h || w > 1440 || h > 3200) { fprintf(stderr, "size must be within 1440x3200\n"); return 2; }
    if (!strcmp(argv[1], argv[2])) { fprintf(stderr, "OUT must be a different file than IN\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    uint8_t *buf = malloc(len > 0 ? (size_t)len : 1);
    if (!buf || len <= 0 || fread(buf, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    fclose(f);
    int n = patch(buf, (size_t)len, w, h);
    if (n != 4) { fprintf(stderr, "expected 4 framebuffer properties, found %d; nothing written\n", n); return 1; }
    f = fopen(argv[2], "wb");
    if (!f || fwrite(buf, 1, (size_t)len, f) != (size_t)len || fclose(f)) { perror(argv[2]); return 1; }
    printf("wrote %s\n", argv[2]);
    return 0;
}
