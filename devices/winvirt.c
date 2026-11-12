#include "winvirt.h"
#include "sysreg.h"
#include "timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Modeled on devices/vmapple.c. What changed:
     gone:  boot ROM transport (bdif), config region, AES, Apple's paravirtual GPU, the RTC select window,
            the avp-ctrr and avp-sealed registers, the virtio USB installer port, the GIC MSI frame.
     kept:  GICv3, PL011, PL031, PCIe (INTx only), xHCI, the timer, PSCI over HVC.
     TODO:  (ACPI is in winvirt_acpi.c, fw_cfg in fwcfg.c), a device tree at WINVIRT_MEM, NVMe, a framebuffer, HID keyboard/tablet. */

static uint64_t tick_locked(HvMachine *m, HvCpu *cpu) {
    HvWinvirt *b = m->tick_opaque;
    uint64_t deadline = hv_timer_update(b->gic, cpu);
    if (cpu->index != 0) return deadline; /* the others only have their own timers to look after */
    hv_pl031_tick(b->rtc);
    hv_xhci_poll(b->usb);
    return deadline;
}

/* Devices and the GIC are shared by the CPUs: one at a time (as vmapple's tick). */
static uint64_t tick(HvMachine *m, HvCpu *cpu) {
    if (cpu->index != 0) return hv_timer_update(((HvWinvirt *)m->tick_opaque)->gic, cpu);
    hv_dev_lock(m);
    uint64_t deadline = tick_locked(m, cpu);
    hv_dev_unlock(m);
    return deadline;
}

int hv_winvirt_create(HvWinvirt *b, HvMachine *m, HvCpu *cpu, int ncpus, uint64_t ram_size) {
    memset(b, 0, sizeof *b);
    b->m = m;
    b->cpu = cpu;
    b->ncpus = ncpus < 1 ? 1 : ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : ncpus;
    b->ram_size = ram_size;

    /* Reserved, not committed: pages are only used once touched. The iOS memory-limit tricks in
       vmapple.c (alloc_no_footprint, HV_RAM_FILE) still need porting for a phone. */
    b->ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    b->flash = mmap(NULL, WINVIRT_FLASH_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b->ram == MAP_FAILED || b->flash == MAP_FAILED) return -1;
    hv_machine_add_ram(m, WINVIRT_FLASH, WINVIRT_FLASH_SIZE, b->flash, 0);
    hv_machine_add_ram(m, WINVIRT_MEM, ram_size, b->ram, 0);

    b->gic = hv_gicv3_create(m, WINVIRT_GIC_DIST, WINVIRT_GIC_REDIST);
    b->uart = hv_pl011_create(m, b->gic, WINVIRT_UART, 32 + WINVIRT_IRQ_UART);
    b->rtc = hv_pl031_create(m, b->gic, WINVIRT_RTC, 32 + WINVIRT_IRQ_RTC);
    b->pcie = hv_pcie_create(m, b->gic, WINVIRT_PCIE_ECAM, WINVIRT_PCIE_ECAM_SIZE, WINVIRT_PCIE_MMIO,
                             WINVIRT_PCIE_MMIO_SIZE, 32 + WINVIRT_IRQ_PCIE);
    b->usb = hv_xhci_create(m, b->pcie);
    b->fwcfg = hv_fwcfg_create(m, WINVIRT_FWCFG, ram_size, b->ncpus);
    if (b->fwcfg && hv_winvirt_install_acpi(b) != 0) return -1;
    if (!b->fwcfg || !b->gic || !b->uart || !b->rtc || !b->pcie || !b->usb) return -1;

    m->tick = tick;
    m->tick_opaque = b;
    cpu->m = m;
    return 0;
}

int hv_winvirt_load_firmware(HvWinvirt *b, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(b->flash, 1, WINVIRT_FLASH_SIZE, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    return n == 0 || more ? -1 : 0;
}

void hv_winvirt_reset_cpu(HvWinvirt *b) {
    b->m->cntvoff = hv_counter();
    for (int i = 0; i < b->ncpus; i++) {
        HvCpu *c = &b->cpu[i];
        hv_cpu_reset_index(c, b->m, (uint32_t)i);
        c->mpidr = 0x80000000u | (uint32_t)i; /* Aff0 = i: the MADT's GICC entries must say the same */
        c->pc = WINVIRT_FLASH;
        c->x[0] = WINVIRT_RAM; /* the device tree, at the start of RAM: what QEMU hands a firmware (EDK2 reads x0) */
        c->off = i != 0; /* secondaries wait for PSCI CPU_ON (HVC conduit: say so in the FADT) */
    }
}

void hv_winvirt_destroy(HvWinvirt *b) {
    if (b->usb) { pthread_mutex_destroy(&b->usb->mu); free(b->usb); }
    if (b->gic) { pthread_mutex_destroy(&b->gic->mu); free(b->gic); }
    hv_fwcfg_destroy(b->fwcfg);
    free(b->pcie);
    free(b->uart);
    free(b->rtc);
    if (b->flash && b->flash != MAP_FAILED) munmap(b->flash, WINVIRT_FLASH_SIZE);
    if (b->ram && b->ram != MAP_FAILED) munmap(b->ram, b->ram_size);
    memset(b, 0, sizeof *b);
}
