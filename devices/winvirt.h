#ifndef HENGEVM_WINVIRT_H
#define HENGEVM_WINVIRT_H

#include <stdint.h>

#include "fwcfg.h"
#include "gicv3.h"
#include "pcie.h"
#include "pl011.h"
#include "pl031.h"
#include "xhci.h"

/* A virtual ARM64 PC for Windows: QEMU's `virt` memory map (hw/arm/virt.c) in place of vmapple's.
   Drop this file and winvirt.c into devices/ next to vmapple.c. UNTESTED: written from reading
   vmapple.c and the device headers, not built. */
#define WINVIRT_FLASH          0x00000000ull /* UEFI firmware (EDK2 AAVMF), mapped as RAM like vmapple's ROM */
#define WINVIRT_FLASH_SIZE     0x04000000ull /* 64 MB */
#define WINVIRT_GIC_DIST       0x08000000ull
#define WINVIRT_GIC_REDIST     0x080a0000ull
#define WINVIRT_UART           0x09000000ull
#define WINVIRT_RTC            0x09010000ull
#define WINVIRT_FWCFG          0x09020000ull /* devices/fwcfg.c; serves the ACPI tables from winvirt_acpi.c */
#define WINVIRT_PCIE_MMIO      0x10000000ull
#define WINVIRT_PCIE_MMIO_SIZE 0x2eff0000ull
#define WINVIRT_PCIE_ECAM      0x3f000000ull
#define WINVIRT_PCIE_ECAM_SIZE 0x01000000ull
#define WINVIRT_MEM            0x40000000ull /* RAM; the device tree EDK2 reads sits at its start */

/* SPI numbers; the INTID is 32 more. */
#define WINVIRT_IRQ_UART 1
#define WINVIRT_IRQ_RTC  2
#define WINVIRT_IRQ_PCIE 3 /* INTx A-D on SPIs 3..6 */

typedef struct HvWinvirt {
    HvMachine *m;
    HvCpu *cpu;
    int ncpus;
    HvGic *gic;
    HvPl011 *uart;
    HvPl031 *rtc;
    HvPcie *pcie;
    HvFwCfg *fwcfg;
    HvXhci *usb;   /* TODO: its HID devices are Apple's trackpad; Windows needs a plain keyboard and tablet */
    uint8_t *flash, *ram;
    uint64_t ram_size;
} HvWinvirt;

int hv_winvirt_create(HvWinvirt *b, HvMachine *m, HvCpu *cpus, int ncpus, uint64_t ram_size);
/* Copies the UEFI firmware image into the flash region. */
int hv_winvirt_load_firmware(HvWinvirt *b, const char *path);
/* Writes the device tree the firmware reads at the start of RAM (winvirt_dtb.c). Call before the first CPU runs. */
int hv_winvirt_install_dtb(HvWinvirt *b);
/* Adds the ACPI tables and their linker/loader script to fw_cfg (winvirt_acpi.c); hv_winvirt_create calls it. */
int hv_winvirt_install_acpi(HvWinvirt *b);
/* CPU reset: EL1, MMU off, interrupts masked, at the start of the flash; secondaries wait for PSCI CPU_ON. */
void hv_winvirt_reset_cpu(HvWinvirt *b);
void hv_winvirt_destroy(HvWinvirt *b);

#endif
