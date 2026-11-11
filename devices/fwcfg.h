#ifndef HENGEVM_FWCFG_H
#define HENGEVM_FWCFG_H

#include <stddef.h>
#include <stdint.h>

#include "machine.h"

/* QEMU's fw_cfg as the ARM `virt` board maps it (hw/nvram/fw_cfg.c, `fw_cfg,mmio`), enough for EDK2 ArmVirtQemu:
     +0x00 data (read: bytes in order), +0x08 selector (16 bit, big endian), +0x10 DMA address (64 bit, big endian).
   Items are the standard ones (signature, ID, RAM size, CPU count) plus named files added by the board
   (etc/acpi/tables, etc/acpi/rsdp, etc/table-loader, ...). UNTESTED: written from QEMU's documentation
   (docs/specs/fw_cfg.rst) and source, not built. */
#define HV_FWCFG_MAX_FILES 16
#define HV_FWCFG_FILE_FIRST 0x20

typedef struct HvFwCfgFile {
    char name[56];
    uint8_t *data; /* owned: copied in by hv_fwcfg_add_file */
    uint32_t size;
} HvFwCfgFile;

typedef struct HvFwCfg {
    HvMachine *m;
    uint16_t sel;
    uint32_t pos;     /* offset in the selected item */
    uint64_t dma_addr; /* the two 32 bit halves written so far (big endian register) */
    uint64_t ram_size;
    uint16_t nb_cpus;
    HvFwCfgFile files[HV_FWCFG_MAX_FILES];
    int nfiles;
    uint8_t *dir;     /* the FILE_DIR item, rebuilt when a file is added */
    uint32_t dir_size;
} HvFwCfg;

HvFwCfg *hv_fwcfg_create(HvMachine *m, uint64_t pa, uint64_t ram_size, int ncpus);
/* Adds a named file (name at most 55 characters), copying size bytes. Returns 0, or -1 if full or too long. */
int hv_fwcfg_add_file(HvFwCfg *f, const char *name, const void *data, uint32_t size);
void hv_fwcfg_destroy(HvFwCfg *f);

#endif
