#include <stdio.h>
#include "ramfb.h"

#include <stdlib.h>
#include <string.h>

#define FOURCC_XR24 0x34325258u /* 'XR24': XRGB8888, B G R x bytes in memory */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void ramfb_write(void *opaque, const uint8_t *d, uint32_t size) {
    HvRamfb *r = opaque;
    if (size < 28) return;
    uint64_t addr = (uint64_t)rd32(d) << 32 | rd32(d + 4);
    r->fourcc = rd32(d + 8);
    r->width = rd32(d + 16);
    r->height = rd32(d + 20);
    r->stride = rd32(d + 24);
    r->addr = addr;
    fprintf(stderr, "ramfb: guest set %ux%u fourcc %#x stride %u at %#llx\n", r->width, r->height, r->fourcc, r->stride, (unsigned long long)addr);
}

HvRamfb *hv_ramfb_create(HvMachine *m, HvFwCfg *f) {
    HvRamfb *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->m = m;
    if (hv_fwcfg_add_writable(f, "etc/ramfb", 28, ramfb_write, r) != 0) {
        free(r);
        return NULL;
    }
    return r;
}

int hv_ramfb_frame(HvRamfb *r, const uint8_t **pixels, uint32_t *w, uint32_t *h, uint32_t *stride) {
    if (!r || !r->addr || r->fourcc != FOURCC_XR24 || !r->width || !r->height || r->width > 4096 || r->height > 4096) return -1;
    uint32_t row = r->stride ? r->stride : r->width * 4;
    HvRam *ram = hv_ram_lookup(r->m, r->addr);
    if (!ram || r->addr + (uint64_t)row * r->height > ram->pa + ram->size) return -1;
    *pixels = ram->host + (r->addr - ram->pa);
    *w = r->width, *h = r->height, *stride = row;
    return 0;
}
