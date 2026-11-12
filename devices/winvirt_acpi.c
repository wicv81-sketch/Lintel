#include "winvirt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ACPI tables for the Windows board, handed to the firmware through fw_cfg the way QEMU's `virt` does:
     etc/acpi/tables       FACS, DSDT, FADT, MADT, GTDT, MCFG, SPCR and the XSDT, back to back
     etc/acpi/rsdp         the RSDP
     etc/table-loader      QEMU's linker/loader script (hw/acpi/bios-linker-loader.c): allocate the two files, add the
                           allocated address into the pointer fields, fix the checksums. EDK2's AcpiPlatformDxe runs it.
   Pointers inside the files hold offsets into etc/acpi/tables until the firmware adds the real base.
   Hardware-reduced ACPI with PSCI over HVC (FADT), GICv3 (MADT), the architected timer (GTDT), PCIe ECAM (MCFG),
   the PL011 as the debug port (SPCR). The DSDT is hand-assembled AML: the CPUs, COM0 and a PCI0 root bridge with
   its window and interrupt routing. UNTESTED on a guest: the generator is checked for structure and checksums
   by a script on a Linux host, not by Windows or a real ACPI parser (no iasl here). */

typedef struct {
    uint8_t d[8192];
    size_t n;
    int err;
} Buf;

static void b8(Buf *b, unsigned v) {
    if (b->n >= sizeof b->d) { b->err = 1; return; }
    b->d[b->n++] = (uint8_t)v;
}
static void b16(Buf *b, unsigned v) { b8(b, v), b8(b, v >> 8); }
static void b32(Buf *b, uint32_t v) { b16(b, v & 0xffff), b16(b, v >> 16); }
static void b64(Buf *b, uint64_t v) { b32(b, (uint32_t)v), b32(b, (uint32_t)(v >> 32)); }
static void bytes(Buf *b, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) b8(b, ((const uint8_t *)p)[i]);
}
static void pad(Buf *b, size_t a) { while (b->n % a) b8(b, 0); }
static void w16(Buf *b, size_t at, unsigned v) { b->d[at] = (uint8_t)v, b->d[at + 1] = (uint8_t)(v >> 8); }
static void w32(Buf *b, size_t at, uint32_t v) { w16(b, at, v & 0xffff), w16(b, at + 2, v >> 16); }
static void w64(Buf *b, size_t at, uint64_t v) { w32(b, at, (uint32_t)v), w32(b, at + 4, (uint32_t)(v >> 32)); }

/* A table header (36 bytes); returns where the table starts in the buffer. The checksum is the loader's job. */
static size_t sdt_begin(Buf *b, const char *sig, unsigned rev) {
    pad(b, 8);
    size_t at = b->n;
    bytes(b, sig, 4), b32(b, 0), b8(b, rev), b8(b, 0);
    bytes(b, "HENGE ", 6), bytes(b, "LINTEL  ", 8), b32(b, 1), bytes(b, "HENG", 4), b32(b, 1);
    return at;
}
static void sdt_end(Buf *b, size_t at) { w32(b, at + 4, (uint32_t)(b->n - at)); }

/* ---- AML ---- */

static void nameseg(Buf *a, const char *s) {
    for (int i = 0; i < 4; i++) b8(a, s[i] ? s[i] : '_');
}
/* PkgLength in its two-byte form (up to 0xfff), counting its own two bytes. */
static size_t pkg_begin(Buf *a) { size_t at = a->n; b16(a, 0); return at; }
static void pkg_end(Buf *a, size_t at) {
    size_t len = a->n - at;
    if (len > 0xfff) { a->err = 1; return; }
    a->d[at] = (uint8_t)(0x40 | (len & 0xf));
    a->d[at + 1] = (uint8_t)(len >> 4);
}
static void aml_int(Buf *a, uint32_t v) {
    if (v == 0) b8(a, 0x00);
    else if (v == 1) b8(a, 0x01);
    else if (v <= 0xff) b8(a, 0x0a), b8(a, v);
    else if (v <= 0xffff) b8(a, 0x0b), b16(a, v);
    else b8(a, 0x0c), b32(a, v);
}
static void aml_name(Buf *a, const char *seg) { b8(a, 0x08); nameseg(a, seg); }
static void aml_str(Buf *a, const char *s) { b8(a, 0x0d); bytes(a, s, strlen(s) + 1); }
/* "PNP0A08": three letters packed into 5 bits each, four hex digits, as a DWord. */
static void aml_eisaid(Buf *a, const char *id) {
    unsigned hex = 0;
    for (int i = 3; i < 7; i++) hex = hex << 4 | (unsigned)(id[i] <= '9' ? id[i] - '0' : id[i] - 'A' + 10);
    unsigned c1 = (unsigned)id[0] - 0x40, c2 = (unsigned)id[1] - 0x40, c3 = (unsigned)id[2] - 0x40;
    b8(a, 0x0c);
    b8(a, c1 << 2 | c2 >> 3), b8(a, (c2 & 7) << 5 | c3), b8(a, hex >> 8), b8(a, hex & 0xff);
}
static size_t dev_begin(Buf *a, const char *seg) {
    b8(a, 0x5b), b8(a, 0x82);
    size_t at = pkg_begin(a);
    nameseg(a, seg);
    return at;
}
static void aml_buffer(Buf *a, const Buf *res) { /* Name(<seg>, Buffer(){res}) : the caller wrote the Name */
    b8(a, 0x11);
    size_t at = pkg_begin(a);
    aml_int(a, (uint32_t)res->n);
    bytes(a, res->d, res->n);
    pkg_end(a, at);
}

/* Resource descriptors. */
static void res_mem32fixed(Buf *r, uint32_t base, uint32_t len) {
    b8(r, 0x86), b16(r, 9), b8(r, 1), b32(r, base), b32(r, len);
}
static void res_irq(Buf *r, uint32_t gsi) { /* extended interrupt: consumer, level, active high, exclusive */
    b8(r, 0x89), b16(r, 6), b8(r, 0x01), b8(r, 1), b32(r, gsi);
}
static void res_bus(Buf *r, unsigned first, unsigned last) { /* WordBusNumber: producer, fixed min and max */
    b8(r, 0x88), b16(r, 13), b8(r, 2), b8(r, 0x0c), b8(r, 0);
    b16(r, 0), b16(r, first), b16(r, last), b16(r, 0), b16(r, last - first + 1);
}
static void res_mem32(Buf *r, uint32_t first, uint32_t last) { /* DWordMemory: producer, read/write */
    b8(r, 0x87), b16(r, 23), b8(r, 0), b8(r, 0x0c), b8(r, 0x01);
    b32(r, 0), b32(r, first), b32(r, last), b32(r, 0), b32(r, last - first + 1);
}
static void res_end(Buf *r) { b8(r, 0x79), b8(r, 0); }

/* PCI interrupt routing: slot s, pin INTA..INTD. This host's PCIe takes slot s's INTA to SPI WINVIRT_IRQ_PCIE + s
   (pcie.c), the same as QEMU's swizzle for slots 0-3; only INTA is used (every device is one function). */
static void aml_prt(Buf *a) {
    aml_name(a, "_PRT");
    b8(a, 0x12);
    size_t outer = pkg_begin(a);
    b8(a, 16);
    for (unsigned slot = 0; slot < 4; slot++)
        for (unsigned pin = 0; pin < 4; pin++) {
            b8(a, 0x12);
            size_t in = pkg_begin(a);
            b8(a, 4);
            b8(a, 0x0c), b32(a, slot << 16 | 0xffff);
            aml_int(a, pin);
            b8(a, 0x00); /* no link device: the next element is the GSI itself */
            aml_int(a, 32 + WINVIRT_IRQ_PCIE + ((pin + slot) & 3));
            pkg_end(a, in);
        }
    pkg_end(a, outer);
}

static void build_aml(Buf *a, int ncpus) {
    b8(a, 0x10); /* Scope(\_SB_) */
    size_t sb = pkg_begin(a);
    b8(a, 0x5c), nameseg(a, "_SB_");

    for (int i = 0; i < ncpus; i++) {
        char nm[8];
        snprintf(nm, sizeof nm, "C%03X", i);
        size_t d = dev_begin(a, nm);
        aml_name(a, "_HID"), aml_str(a, "ACPI0007");
        aml_name(a, "_UID"), aml_int(a, (uint32_t)i);
        pkg_end(a, d);
    }

    size_t com = dev_begin(a, "COM0");
    aml_name(a, "_HID"), aml_str(a, "ARMH0011");
    aml_name(a, "_CID"), aml_str(a, "PL011");
    aml_name(a, "_UID"), aml_int(a, 0);
    Buf r = {0};
    res_mem32fixed(&r, (uint32_t)WINVIRT_UART, 0x1000);
    res_irq(&r, 32 + WINVIRT_IRQ_UART);
    res_end(&r);
    aml_name(a, "_CRS"), aml_buffer(a, &r);
    pkg_end(a, com);

    size_t pci = dev_begin(a, "PCI0");
    aml_name(a, "_HID"), aml_eisaid(a, "PNP0A08");
    aml_name(a, "_CID"), aml_eisaid(a, "PNP0A03");
    aml_name(a, "_SEG"), aml_int(a, 0);
    aml_name(a, "_BBN"), aml_int(a, 0);
    aml_name(a, "_UID"), aml_int(a, 0);
    aml_name(a, "_CCA"), aml_int(a, 1); /* DMA is cache coherent */
    memset(&r, 0, sizeof r);
    res_bus(&r, 0, 15);
    res_mem32(&r, (uint32_t)WINVIRT_PCIE_MMIO, (uint32_t)(WINVIRT_PCIE_MMIO + WINVIRT_PCIE_MMIO_SIZE - 1));
    res_end(&r);
    aml_name(a, "_CRS"), aml_buffer(a, &r);
    aml_prt(a);
    pkg_end(a, pci);

    pkg_end(a, sb);
}

/* ---- the linker/loader script ---- */

#define LD_ALLOCATE     1
#define LD_ADD_POINTER  2
#define LD_ADD_CHECKSUM 3
#define ZONE_HIGH       1
#define ZONE_FSEG       2

static void name56(Buf *l, const char *s) {
    size_t n = strlen(s);
    bytes(l, s, n);
    for (size_t i = n; i < 56; i++) b8(l, 0);
}
static void ld_allocate(Buf *l, const char *file, uint32_t align, unsigned zone) {
    b32(l, LD_ALLOCATE), name56(l, file), b32(l, align), b8(l, zone), pad(l, 128);
}
static void ld_pointer(Buf *l, const char *dest, const char *src, uint32_t offset, unsigned size) {
    b32(l, LD_ADD_POINTER), name56(l, dest), name56(l, src), b32(l, offset), b8(l, size), pad(l, 128);
}
static void ld_checksum(Buf *l, const char *file, uint32_t result, uint32_t start, uint32_t len) {
    b32(l, LD_ADD_CHECKSUM), name56(l, file), b32(l, result), b32(l, start), b32(l, len), pad(l, 128);
}

#define F_TABLES "etc/acpi/tables"
#define F_RSDP   "etc/acpi/rsdp"

int hv_winvirt_install_acpi(HvWinvirt *b) {
    static Buf t, l, rsdp, aml;
    memset(&t, 0, sizeof t), memset(&l, 0, sizeof l), memset(&rsdp, 0, sizeof rsdp), memset(&aml, 0, sizeof aml);
    int ncpus = b->ncpus;

    /* FACS, first so that it is 64-byte aligned (the file is allocated at 64). */
    size_t facs = t.n;
    bytes(&t, "FACS", 4), b32(&t, 64);
    while (t.n < facs + 64) b8(&t, 0);
    w32(&t, facs + 32, 2); /* version */

    build_aml(&aml, ncpus);
    size_t dsdt = sdt_begin(&t, "DSDT", 2);
    bytes(&t, aml.d, aml.n);
    sdt_end(&t, dsdt);

    size_t fadt = sdt_begin(&t, "FACP", 6);
    while (t.n < fadt + 276) b8(&t, 0);
    w32(&t, fadt + 112, 1u << 20);      /* HW_REDUCED_ACPI */
    w16(&t, fadt + 129, 3);             /* ARM boot arch: PSCI compliant, conduit HVC */
    t.d[fadt + 131] = 3;                /* FADT minor version (6.3) */
    w64(&t, fadt + 132, facs);          /* X_FIRMWARE_CTRL, an offset until the loader adds the base */
    w64(&t, fadt + 140, dsdt);          /* X_DSDT */
    sdt_end(&t, fadt);

    size_t madt = sdt_begin(&t, "APIC", 4);
    b32(&t, 0), b32(&t, 0); /* local interrupt controller address, flags */
    for (int i = 0; i < ncpus; i++) { /* GICC, 80 bytes: the GICv3 system-register interface, the CPU's Aff0 is i */
        size_t g = t.n;
        b8(&t, 0x0b), b8(&t, 80), b16(&t, 0), b32(&t, (uint32_t)i), b32(&t, (uint32_t)i), b32(&t, 1);
        while (t.n < g + 68) b8(&t, 0);
        b64(&t, (uint64_t)i); /* MPIDR affinity fields */
        while (t.n < g + 80) b8(&t, 0);
    }
    b8(&t, 0x0c), b8(&t, 24), b16(&t, 0), b32(&t, 0), b64(&t, WINVIRT_GIC_DIST), b32(&t, 0), b8(&t, 3);
    b8(&t, 0), b16(&t, 0);                                                    /* GICD: version 3 */
    b8(&t, 0x0e), b8(&t, 16), b16(&t, 0), b64(&t, WINVIRT_GIC_REDIST), b32(&t, 0x20000u * (uint32_t)ncpus); /* GICR */
    sdt_end(&t, madt);

    size_t gtdt = sdt_begin(&t, "GTDT", 3);
    b64(&t, ~0ull), b32(&t, 0);              /* no memory-mapped counter block */
    b32(&t, 16 + 13), b32(&t, 4);            /* secure EL1 physical: PPI 13, always on */
    b32(&t, 16 + 14), b32(&t, 4);            /* non-secure EL1 physical: PPI 14 */
    b32(&t, 16 + 11), b32(&t, 4);            /* virtual: PPI 11 */
    b32(&t, 16 + 10), b32(&t, 4);            /* non-secure EL2 physical: PPI 10 */
    b64(&t, ~0ull);                          /* no counter read block */
    b32(&t, 0), b32(&t, 0);                  /* virtual EL2 timer: none */
    b32(&t, 0), b32(&t, 104);                /* no platform timers */
    sdt_end(&t, gtdt);

    size_t mcfg = sdt_begin(&t, "MCFG", 1);
    b64(&t, 0), b64(&t, WINVIRT_PCIE_ECAM), b16(&t, 0), b8(&t, 0), b8(&t, 15), b32(&t, 0);
    sdt_end(&t, mcfg);

    size_t spcr = sdt_begin(&t, "SPCR", 2);
    b8(&t, 3), b8(&t, 0), b16(&t, 0);                                         /* interface type: ARM PL011 */
    b8(&t, 0), b8(&t, 32), b8(&t, 0), b8(&t, 3), b64(&t, WINVIRT_UART);       /* base address: memory, 32 bit, dword access */
    b8(&t, 8), b8(&t, 0), b32(&t, 32 + WINVIRT_IRQ_UART);                     /* interrupt type: GIC, then the GSI */
    b8(&t, 7), b8(&t, 0), b8(&t, 1), b8(&t, 0), b8(&t, 0), b8(&t, 0);         /* 115200 8N1, no flow control */
    b16(&t, 0xffff), b16(&t, 0xffff), b8(&t, 0), b8(&t, 0), b8(&t, 0);        /* not a PCI UART */
    b32(&t, 0), b8(&t, 0), b32(&t, 0);                                        /* PCI flags, segment, clock */
    sdt_end(&t, spcr);

    size_t xsdt = sdt_begin(&t, "XSDT", 1);
    size_t entries[] = {fadt, madt, gtdt, mcfg, spcr};
    size_t xsdt_first = t.n;
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) b64(&t, entries[i]);
    sdt_end(&t, xsdt);

    /* RSDP: XsdtAddress is an offset into the tables until the loader adds the base. */
    bytes(&rsdp, "RSD PTR ", 8), b8(&rsdp, 0), bytes(&rsdp, "HENGE ", 6), b8(&rsdp, 2), b32(&rsdp, 0), b32(&rsdp, 36);
    b64(&rsdp, xsdt), b8(&rsdp, 0), b8(&rsdp, 0), b16(&rsdp, 0);

    ld_allocate(&l, F_TABLES, 64, ZONE_HIGH);
    ld_allocate(&l, F_RSDP, 16, ZONE_FSEG);
    ld_pointer(&l, F_TABLES, F_TABLES, (uint32_t)(fadt + 132), 8);
    ld_pointer(&l, F_TABLES, F_TABLES, (uint32_t)(fadt + 140), 8);
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++)
        ld_pointer(&l, F_TABLES, F_TABLES, (uint32_t)(xsdt_first + 8 * i), 8);
    size_t starts[] = {dsdt, fadt, madt, gtdt, mcfg, spcr, xsdt};
    for (size_t i = 0; i < sizeof starts / sizeof *starts; i++) {
        uint32_t len = (uint32_t)(t.d[starts[i] + 4] | t.d[starts[i] + 5] << 8);
        ld_checksum(&l, F_TABLES, (uint32_t)(starts[i] + 9), (uint32_t)starts[i], len);
    }
    ld_pointer(&l, F_RSDP, F_TABLES, 24, 8);
    ld_checksum(&l, F_RSDP, 8, 0, 20);
    ld_checksum(&l, F_RSDP, 32, 0, 36);

    if (t.err || l.err || rsdp.err || aml.err) return -1;
    if (hv_fwcfg_add_file(b->fwcfg, "etc/acpi/tables", t.d, (uint32_t)t.n)) return -1;
    if (hv_fwcfg_add_file(b->fwcfg, "etc/acpi/rsdp", rsdp.d, (uint32_t)rsdp.n)) return -1;
    if (hv_fwcfg_add_file(b->fwcfg, "etc/table-loader", l.d, (uint32_t)l.n)) return -1;
    return 0;
}
