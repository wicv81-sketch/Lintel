#include "fwcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* See fwcfg.h. UNTESTED. Item numbers and the DMA layout follow docs/specs/fw_cfg.rst. */

#define FW_CFG_SIGNATURE 0x0000
#define FW_CFG_ID        0x0001
#define FW_CFG_RAM_SIZE  0x0003
#define FW_CFG_NB_CPUS   0x0005
#define FW_CFG_FILE_DIR  0x0019

#define FEATURE_TRADITIONAL 1u
#define FEATURE_DMA         2u

#define DMA_ERROR  0x01u
#define DMA_READ   0x02u
#define DMA_SKIP   0x04u
#define DMA_SELECT 0x08u
#define DMA_WRITE  0x10u

static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
static uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }

static void rebuild_dir(HvFwCfg *f) {
    free(f->dir);
    f->dir_size = 4 + 64u * (uint32_t)f->nfiles;
    f->dir = calloc(1, f->dir_size);
    uint32_t n = be32((uint32_t)f->nfiles);
    memcpy(f->dir, &n, 4);
    for (int i = 0; i < f->nfiles; i++) {
        uint8_t *e = f->dir + 4 + 64 * i;
        uint32_t size = be32(f->files[i].size);
        uint16_t select = be16((uint16_t)(HV_FWCFG_FILE_FIRST + i));
        memcpy(e, &size, 4);
        memcpy(e + 4, &select, 2);
        memcpy(e + 8, f->files[i].name, 56);
    }
}

/* The bytes of the selected item: its start and length, or 0 if there is no such item. */
static uint32_t item(HvFwCfg *f, uint16_t sel, const uint8_t **p, uint8_t *tmp) {
    switch (sel) {
    case FW_CFG_SIGNATURE: memcpy(tmp, "QEMU", 4); *p = tmp; return 4;
    case FW_CFG_ID: {
        uint32_t v = FEATURE_TRADITIONAL | FEATURE_DMA; /* little endian */
        memcpy(tmp, &v, 4);
        *p = tmp;
        return 4;
    }
    case FW_CFG_RAM_SIZE: memcpy(tmp, &f->ram_size, 8); *p = tmp; return 8;
    case FW_CFG_NB_CPUS: memcpy(tmp, &f->nb_cpus, 2); *p = tmp; return 2;
    case FW_CFG_FILE_DIR: *p = f->dir; return f->dir_size;
    default:
        if (sel >= HV_FWCFG_FILE_FIRST && sel < HV_FWCFG_FILE_FIRST + f->nfiles) {
            *p = f->files[sel - HV_FWCFG_FILE_FIRST].data;
            return f->files[sel - HV_FWCFG_FILE_FIRST].size;
        }
        return 0;
    }
}

/* Next byte of the selected item; reads past its end (or of an unknown item) give 0. */
static uint8_t next_byte(HvFwCfg *f) {
    uint8_t tmp[8];
    const uint8_t *p = NULL;
    uint32_t n = item(f, f->sel, &p, tmp);
    uint8_t v = f->pos < n ? p[f->pos] : 0;
    f->pos++;
    return v;
}

static uint64_t fwcfg_read(void *opaque, uint64_t off, unsigned size) {
    HvFwCfg *f = opaque;
    if (off < 8) { /* data: the bytes in order, first byte most significant */
        uint64_t v = 0;
        for (unsigned i = 0; i < size && i < 8; i++) v = (v << 8) | next_byte(f);
        return v;
    }
    if (off >= 0x10 && off < 0x18) { /* DMA register reads back the signature "QEMU CFG" */
        uint64_t sig = 0x51454d5520434647ull;
        unsigned shift = (unsigned)(8 - (off - 0x10) - size) * 8;
        return (sig >> shift) & (size >= 8 ? ~0ull : ((1ull << (size * 8)) - 1));
    }
    return 0;
}

static void dma_run(HvFwCfg *f, uint64_t desc_pa) {
    HvRam *r = hv_ram_lookup(f->m, desc_pa);
    if (!r || desc_pa + 16 > r->pa + r->size) return;
    uint8_t *d = r->host + (desc_pa - r->pa);
    uint32_t control, length;
    uint64_t addr;
    memcpy(&control, d, 4);
    memcpy(&length, d + 4, 4);
    memcpy(&addr, d + 8, 8);
    control = be32(control);
    length = be32(length);
    addr = __builtin_bswap64(addr);
    if (control & DMA_SELECT) {
        f->sel = (uint16_t)(control >> 16);
        f->pos = 0;
    }
    uint32_t err = 0;
    HvRam *dst = NULL;
    if (control & DMA_READ) {
        dst = hv_ram_lookup(f->m, addr);
        if (!dst || addr + length > dst->pa + dst->size) err = DMA_ERROR;
    }
    HvFwCfgFile *wf = NULL;
    if (!err && (control & DMA_WRITE)) {
        if (f->sel >= HV_FWCFG_FILE_FIRST && f->sel < HV_FWCFG_FILE_FIRST + f->nfiles)
            wf = &f->files[f->sel - HV_FWCFG_FILE_FIRST];
        HvRam *src = hv_ram_lookup(f->m, addr);
        if (!wf || !wf->on_write || !src || addr + length > src->pa + src->size || f->pos + length > wf->size) err = DMA_ERROR;
        else {
            memcpy(wf->data + f->pos, src->host + (addr - src->pa), length);
            f->pos += length;
            wf->on_write(wf->opaque, wf->data, wf->size);
        }
    }
    if (!err) {
        if (control & DMA_READ)
            for (uint32_t i = 0; i < length; i++) dst->host[addr - dst->pa + i] = next_byte(f);
        else if (control & DMA_SKIP)
            f->pos += length;
    }
    /* Done: the guest polls control until it reads 0 (or the error bit). */
    uint32_t done = be32(err);
    memcpy(d, &done, 4);
    if (err == 0 && dst) hv_invalidate_code(f->m, addr);
}

static void fwcfg_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvFwCfg *f = opaque;
    if (off == 0x08) { /* selector, big endian on the bus */
        static int shown;
        if (shown++ < 60) fprintf(stderr, "fw_cfg select %#x\n", size == 1 ? (unsigned)v : size == 2 ? be16((uint16_t)v) : be16((uint16_t)(v >> 16)));
        f->sel = size == 1 ? (uint16_t)v : size == 2 ? be16((uint16_t)v) : be16((uint16_t)(v >> 16));
        f->pos = 0;
    } else if (off == 0x10 && size == 8) { /* whole address in one access */
        dma_run(f, __builtin_bswap64(v));
    } else if (off == 0x10 && size == 4) { /* high half first */
        f->dma_addr = (uint64_t)be32((uint32_t)v) << 32;
    } else if (off == 0x14 && size == 4) { /* the low half starts the transfer */
        f->dma_addr |= be32((uint32_t)v);
        dma_run(f, f->dma_addr);
    }
}

int hv_fwcfg_add_writable(HvFwCfg *f, const char *name, uint32_t size, void (*on_write)(void *, const uint8_t *, uint32_t),
                          void *opaque) {
    uint8_t *zero = calloc(1, size ? size : 1);
    if (!zero) return -1;
    int rc = hv_fwcfg_add_file(f, name, zero, size);
    free(zero);
    if (rc) return rc;
    f->files[f->nfiles - 1].on_write = on_write;
    f->files[f->nfiles - 1].opaque = opaque;
    return 0;
}

int hv_fwcfg_add_file(HvFwCfg *f, const char *name, const void *data, uint32_t size) {
    if (f->nfiles >= HV_FWCFG_MAX_FILES || strlen(name) > 55) return -1;
    HvFwCfgFile *x = &f->files[f->nfiles];
    memset(x, 0, sizeof *x);
    snprintf(x->name, sizeof x->name, "%s", name);
    x->data = malloc(size ? size : 1);
    if (!x->data) return -1;
    memcpy(x->data, data, size);
    x->size = size;
    f->nfiles++;
    rebuild_dir(f);
    return 0;
}

HvFwCfg *hv_fwcfg_create(HvMachine *m, uint64_t pa, uint64_t ram_size, int ncpus) {
    HvFwCfg *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->m = m;
    f->ram_size = ram_size;
    f->nb_cpus = (uint16_t)ncpus;
    rebuild_dir(f);
    hv_machine_add_mmio(m, pa, 0x18, f, fwcfg_read, fwcfg_write, "fw_cfg");
    return f;
}

void hv_fwcfg_destroy(HvFwCfg *f) {
    if (!f) return;
    for (int i = 0; i < f->nfiles; i++) free(f->files[i].data);
    free(f->dir);
    free(f);
}
