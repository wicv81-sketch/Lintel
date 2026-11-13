#ifndef HENGEVM_RAMFB_H
#define HENGEVM_RAMFB_H

#include "fwcfg.h"

/* QEMU's ramfb: a framebuffer in guest RAM that the guest describes by writing the fw_cfg file "etc/ramfb"
   (address, fourcc, flags, width, height, stride; all big endian). EDK2's RamFbDxe drives it, and Windows'
   boot display uses the UEFI GOP it provides. UNTESTED. */
typedef struct HvRamfb {
    HvMachine *m;
    uint64_t addr;
    uint32_t fourcc, width, height, stride;
} HvRamfb;

HvRamfb *hv_ramfb_create(HvMachine *m, HvFwCfg *f);
/* The frame (32 bit, B G R x in memory) if the guest has set one up: 0, or -1. */
int hv_ramfb_frame(HvRamfb *r, const uint8_t **pixels, uint32_t *w, uint32_t *h, uint32_t *stride);

#endif
