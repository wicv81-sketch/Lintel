#include "winvirt.h"

#include <stdio.h>
#include <string.h>

/* A minimal flattened device tree in QEMU `virt`'s shape. The UEFI firmware (EDK2 ArmVirtQemu) reads it from
   the start of RAM to find memory, CPUs, the GIC, the UART and PCIe, as far as I know. UNTESTED.
   Left out until the devices exist: the fw-cfg node (EDK2 would probe it and fail), PCIe's I/O range and
   64-bit window, and the PCIe interrupt-map (the firmware may want it; if it stops early, look here first). */

typedef struct {
    uint8_t st[8192];
    size_t sn;
    char str[1024];
    size_t strn;
    int err;
} Fdt;

static void put32(Fdt *f, uint32_t v) {
    if (f->sn + 4 > sizeof f->st) { f->err = 1; return; }
    f->st[f->sn++] = (uint8_t)(v >> 24), f->st[f->sn++] = (uint8_t)(v >> 16);
    f->st[f->sn++] = (uint8_t)(v >> 8), f->st[f->sn++] = (uint8_t)v;
}

static void putbytes(Fdt *f, const void *p, size_t n) {
    size_t padded = (n + 3) & ~(size_t)3;
    if (f->sn + padded > sizeof f->st) { f->err = 1; return; }
    memcpy(f->st + f->sn, p, n);
    memset(f->st + f->sn + n, 0, padded - n);
    f->sn += padded;
}

static uint32_t name_off(Fdt *f, const char *name) {
    for (size_t o = 0; o < f->strn; o += strlen(f->str + o) + 1)
        if (!strcmp(f->str + o, name)) return (uint32_t)o;
    size_t len = strlen(name) + 1;
    if (f->strn + len > sizeof f->str) { f->err = 1; return 0; }
    memcpy(f->str + f->strn, name, len);
    f->strn += len;
    return (uint32_t)(f->strn - len);
}

static void begin(Fdt *f, const char *name) { put32(f, 1); putbytes(f, name, strlen(name) + 1); }
static void end(Fdt *f) { put32(f, 2); }

static void prop(Fdt *f, const char *name, const void *data, size_t len) {
    put32(f, 3), put32(f, (uint32_t)len), put32(f, name_off(f, name));
    putbytes(f, data, len);
}
static void prop_str(Fdt *f, const char *name, const char *s) { prop(f, name, s, strlen(s) + 1); }
static void prop_flag(Fdt *f, const char *name) { put32(f, 3), put32(f, 0), put32(f, name_off(f, name)); }
static void prop_cells(Fdt *f, const char *name, const uint32_t *c, size_t n) {
    put32(f, 3), put32(f, (uint32_t)(n * 4)), put32(f, name_off(f, name));
    for (size_t i = 0; i < n; i++) put32(f, c[i]);
}
#define CELLS(f, name, ...) do { uint32_t a_[] = {__VA_ARGS__}; prop_cells(f, name, a_, sizeof a_ / 4); } while (0)
#define HI(x) ((uint32_t)((uint64_t)(x) >> 32))
#define LO(x) ((uint32_t)(uint64_t)(x))

static void node_name(char *out, size_t n, const char *base, uint64_t addr) {
    snprintf(out, n, "%s@%llx", base, (unsigned long long)addr);
}

static size_t build(const HvWinvirt *b, uint8_t *out, size_t cap) {
    static Fdt f;
    memset(&f, 0, sizeof f);
    char nm[48];
    const uint32_t GIC = 1;

    begin(&f, "");
    CELLS(&f, "#address-cells", 2);
    CELLS(&f, "#size-cells", 2);
    prop_str(&f, "compatible", "linux,dummy-virt");
    CELLS(&f, "interrupt-parent", GIC);

    begin(&f, "chosen");
    end(&f);

    node_name(nm, sizeof nm, "memory", WINVIRT_MEM);
    begin(&f, nm);
    prop_str(&f, "device_type", "memory");
    CELLS(&f, "reg", HI(WINVIRT_MEM), LO(WINVIRT_MEM), HI(b->ram_size), LO(b->ram_size));
    end(&f);

    begin(&f, "cpus");
    CELLS(&f, "#address-cells", 1);
    CELLS(&f, "#size-cells", 0);
    for (int i = 0; i < b->ncpus; i++) {
        snprintf(nm, sizeof nm, "cpu@%x", i);
        begin(&f, nm);
        prop_str(&f, "device_type", "cpu");
        prop_str(&f, "compatible", "arm,arm-v8");
        prop_str(&f, "enable-method", "psci");
        CELLS(&f, "reg", (uint32_t)i);
        end(&f);
    }
    end(&f);

    begin(&f, "psci");
    prop(&f, "compatible", "arm,psci-1.0\0arm,psci-0.2\0arm,psci", sizeof "arm,psci-1.0\0arm,psci-0.2\0arm,psci");
    prop_str(&f, "method", "hvc");
    end(&f);

    node_name(nm, sizeof nm, "intc", WINVIRT_GIC_DIST);
    begin(&f, nm);
    prop_str(&f, "compatible", "arm,gic-v3");
    CELLS(&f, "#interrupt-cells", 3);
    prop_flag(&f, "interrupt-controller");
    CELLS(&f, "#address-cells", 2);
    CELLS(&f, "#size-cells", 2);
    CELLS(&f, "reg", HI(WINVIRT_GIC_DIST), LO(WINVIRT_GIC_DIST), 0, 0x10000,
          HI(WINVIRT_GIC_REDIST), LO(WINVIRT_GIC_REDIST), 0, 0x20000u * (uint32_t)b->ncpus); /* 128 KB per CPU */
    CELLS(&f, "phandle", GIC);
    end(&f);

    begin(&f, "timer");
    prop_str(&f, "compatible", "arm,armv8-timer");
    CELLS(&f, "interrupts", 1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4); /* sec phys, phys, virt, hyp */
    prop_flag(&f, "always-on");
    end(&f);

    node_name(nm, sizeof nm, "pl011", WINVIRT_UART);
    begin(&f, nm);
    prop(&f, "compatible", "arm,pl011\0arm,primecell", 24);
    CELLS(&f, "reg", HI(WINVIRT_UART), LO(WINVIRT_UART), 0, 0x1000);
    CELLS(&f, "interrupts", 0, WINVIRT_IRQ_UART, 4);
    end(&f);

    node_name(nm, sizeof nm, "pl031", WINVIRT_RTC);
    begin(&f, nm);
    prop(&f, "compatible", "arm,pl031\0arm,primecell", 24);
    CELLS(&f, "reg", HI(WINVIRT_RTC), LO(WINVIRT_RTC), 0, 0x1000);
    CELLS(&f, "interrupts", 0, WINVIRT_IRQ_RTC, 4);
    end(&f);

    node_name(nm, sizeof nm, "pcie", WINVIRT_PCIE_ECAM);
    begin(&f, nm);
    prop_str(&f, "compatible", "pci-host-ecam-generic");
    prop_str(&f, "device_type", "pci");
    CELLS(&f, "#address-cells", 3);
    CELLS(&f, "#size-cells", 2);
    CELLS(&f, "#interrupt-cells", 1);
    CELLS(&f, "bus-range", 0, 0xf);
    CELLS(&f, "reg", HI(WINVIRT_PCIE_ECAM), LO(WINVIRT_PCIE_ECAM), 0, (uint32_t)WINVIRT_PCIE_ECAM_SIZE);
    /* One 32-bit memory window, identity mapped (space code 0x02000000). */
    CELLS(&f, "ranges", 0x02000000, 0, LO(WINVIRT_PCIE_MMIO), 0, LO(WINVIRT_PCIE_MMIO), 0,
          (uint32_t)WINVIRT_PCIE_MMIO_SIZE);
    end(&f);
    end(&f); /* root */
    put32(&f, 9);
    if (f.err) return 0;

    /* Header (40 bytes), an empty reserve map (16), the structure block, then the strings. */
    size_t off_st = 56, off_str = off_st + f.sn, total = off_str + f.strn;
    if (total > cap) return 0;
    memset(out, 0, total);
    uint32_t h[10] = {0xd00dfeed, (uint32_t)total, (uint32_t)off_st, (uint32_t)off_str, 40, 17, 16, 0,
                      (uint32_t)f.strn, (uint32_t)f.sn};
    for (int i = 0; i < 10; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24), out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8), out[i * 4 + 3] = (uint8_t)h[i];
    }
    memcpy(out + off_st, f.st, f.sn);
    memcpy(out + off_str, f.str, f.strn);
    return total;
}

/* Writes the tree at the start of RAM, where the firmware looks for it. */
int hv_winvirt_install_dtb(HvWinvirt *b) {
    size_t n = build(b, b->ram, 0x10000);
    if (!n) fprintf(stderr, "winvirt: device tree did not fit\n");
    return n ? 0 : -1;
}
