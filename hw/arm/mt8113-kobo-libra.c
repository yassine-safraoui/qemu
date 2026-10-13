/* Kobo Libra Colour / Netronix E70T00, MediaTek MT8113T.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <libfdt.h>
#include "hw/arm/mt8113.h"
#include "hw/adc/mt6577_auxadc.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/split-irq.h"
#include "hw/i2c/bd71828.h"
#include "hw/i2c/fp9935.h"
#include "hw/i2c/max20342.h"
#include "hw/sd/sd.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "system/block-backend.h"
#include "system/reset.h"

#define TYPE_LIBRA_MACHINE MACHINE_TYPE_NAME("mt8113-kobo-libra")
OBJECT_DECLARE_SIMPLE_TYPE(LibraMachineState, LIBRA_MACHINE)

struct LibraMachineState {
    MachineState parent_obj;
    MT8113State *soc;
};

static void libra_reset(void *opaque)
{
    LibraMachineState *s = opaque;

    for (unsigned i = 0; i < MT8113_NUM_CPUS; i++) {
        cpu_reset(CPU(&s->soc->cpu[i]));
    }
}

static void libra_load_firmware(MachineState *machine)
{
    g_autofree char *fit = NULL;
    gsize size;
    int length;
    int node;
    const void *payload;
    const fdt32_t *load;
    uint8_t bootargs[0x21c] = { 0 };
    /* BL2 hands its boot arguments to the stock AArch32 U-Boot in r0. */
    static const uint32_t handoff[] = {
        0x58000180, 0xd2803a61, 0xd51e4001, 0xd53e1101,
        0x9275f821, 0xb2400021, 0xd51e1101, 0x580000e1,
        0xd51e4021, 0xd5033fdf, 0xd69f03e0, 0xd503201f,
        0x40000100, 0, 0x44e00000, 0,
    };

    if (!machine->firmware ||
        !g_file_get_contents(machine->firmware, &fit, &size, NULL) ||
        size < sizeof(struct fdt_header) || fdt_check_header(fit) ||
        fdt_totalsize(fit) > size) {
        error_report("Libra requires -bios with u-boot-mtk-fit.bin");
        exit(EXIT_FAILURE);
    }
    node = fdt_path_offset(fit, "/images/kernel@1");
    load = fdt_getprop(fit, node, "load", &length);
    if (!load || length != 4 || fdt32_to_cpu(*load) != 0x44e00000) {
        error_report("Invalid Libra U-Boot load address");
        exit(EXIT_FAILURE);
    }
    payload = fdt_getprop(fit, node, "data", &length);
    if (!payload || length <= 0 || length > 2 * MiB) {
        error_report("Invalid Libra U-Boot payload");
        exit(EXIT_FAILURE);
    }
    rom_add_blob_fixed("libra.u-boot", payload, length, 0x44e00000);
    /* magic, DRAM size, 512 efuse bytes, end magic; powerkey byte 0 — the
     * June 2024 E70T00 U-Boot selects the recovery partition when it is 1. */
    stl_le_p(bootargs, 0x504c504c);
    stl_le_p(bootargs + 4, machine->ram_size);
    stl_le_p(bootargs + 0x208, 0x504c504c);
    rom_add_blob_fixed("libra.bootargs", bootargs, sizeof(bootargs),
                       0x40000100);
    rom_add_blob_fixed("libra.handoff", handoff, sizeof(handoff),
                       0x40001000);
}

static void libra_init(MachineState *machine)
{
    LibraMachineState *s = LIBRA_MACHINE(machine);
    MT8113State *soc = MT8113(object_new(TYPE_MT8113));
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    DeviceState *card = qdev_new(TYPE_EMMC);
    DeviceState *adc = qdev_new(TYPE_MT6577_AUXADC);
    I2CSlave *pmic;
    I2CSlave *epd;

    if (machine->ram_size != GiB) {
        error_report("Libra requires 1 GiB RAM");
        exit(EXIT_FAILURE);
    }
    s->soc = soc;
    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc));
    object_property_set_uint(OBJECT(soc), "reset-vector", 0x40001000,
                             &error_fatal);
    qdev_prop_set_bit(DEVICE(&soc->gce), "inclusive-end-address", false);
    qdev_prop_set_bit(DEVICE(&soc->hwtcon), "scanout-image-buffer", true);
    qdev_prop_set_uint32(DEVICE(&soc->hwtcon), "initial-width", 1680);
    qdev_prop_set_uint32(DEVICE(&soc->hwtcon), "initial-height", 1264);
    qdev_realize(DEVICE(soc), NULL, &error_fatal);
    memory_region_add_subregion(get_system_memory(), MT8113_RAM_BASE,
                                machine->ram);
    object_property_add_child(OBJECT(machine), "auxadc", OBJECT(adc));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(adc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(adc), 0, 0x11001000);

    pmic = i2c_slave_new(TYPE_BD71828, 0x4b);
    object_property_add_child(OBJECT(machine), "pmic", OBJECT(pmic));
    qdev_prop_set_bit(DEVICE(pmic), "power-button-support", true);
    i2c_slave_realize_and_unref(pmic, soc->i2c[1].bus, &error_fatal);
    i2c_slave_create_simple(soc->i2c[1].bus, TYPE_BD71828, 0x4c);
    qdev_connect_gpio_out_named(DEVICE(pmic), "irq", 0,
        qdev_get_gpio_in_named(DEVICE(&soc->gpio), "gpio-in", 62));
    epd = i2c_slave_create_simple(soc->i2c[0].bus, TYPE_FP9935, 0x18);
    qdev_connect_gpio_out_named(DEVICE(&soc->gpio), "gpio-out", 11,
        qdev_get_gpio_in_named(DEVICE(epd), "enable", 0));
    qdev_connect_gpio_out_named(DEVICE(epd), "power-good", 0,
        qdev_get_gpio_in_named(DEVICE(&soc->gpio), "gpio-in", 10));
    /* The Libra Colour DTB marks MAX20342 disabled, but the SoC vbus GPIO
     * wiring is shared; keep the Elipsa topology and let the detector idle. */
    I2CSlave *detector = i2c_slave_create_simple(soc->i2c[0].bus,
                                                TYPE_MAX20342, 0x35);
    qdev_connect_gpio_out(DEVICE(detector), 0,
        qdev_get_gpio_in_named(DEVICE(&soc->gpio), "gpio-in", 39));
    DeviceState *vbus = qdev_new(TYPE_SPLIT_IRQ);
    object_property_add_child(OBJECT(machine), "usb-vbus", OBJECT(vbus));
    qdev_prop_set_uint16(vbus, "num-lines", 2);
    qdev_realize_and_unref(vbus, NULL, &error_fatal);
    qdev_connect_gpio_out(vbus, 0,
        qdev_get_gpio_in_named(DEVICE(detector), "vbus", 0));
    qdev_connect_gpio_out(vbus, 1,
        qdev_get_gpio_in_named(DEVICE(pmic), "vbus", 0));
    qdev_connect_gpio_out_named(DEVICE(soc), "usb-vbus", 0,
        qdev_get_gpio_in(vbus, 0));
    I2CSlave *touch = i2c_slave_create_simple(soc->i2c[2].bus,
                                             "elan-ekth3500", 0x10);
    qdev_connect_gpio_out_named(DEVICE(touch), "irq", 0,
        qdev_get_gpio_in_named(DEVICE(&soc->gpio), "gpio-in", 20));
    qdev_connect_gpio_out_named(DEVICE(&soc->gpio), "gpio-out", 14,
        qdev_get_gpio_in_named(DEVICE(touch), "reset", 0));

    qdev_prop_set_uint64(card, "boot-partition-size", 4 * MiB);
    qdev_prop_set_bit(card, "boot-partitions-in-memory", true);
    qdev_prop_set_drive_err(card, "drive", di ? blk_by_legacy_dinfo(di) : NULL,
                            &error_fatal);
    qdev_realize(card, qdev_get_child_bus(DEVICE(&soc->msdc0), "sd-bus"),
                 &error_fatal);
    object_unref(OBJECT(card));
    libra_load_firmware(machine);
    qemu_register_reset(libra_reset, s);
}

static void libra_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Kobo Libra Colour (MediaTek MT8113T / Netronix E70T00)";
    mc->init = libra_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a53");
    mc->default_ram_size = GiB;
    mc->default_ram_id = "libra.ram";
    mc->default_cpus = mc->min_cpus = mc->max_cpus = MT8113_NUM_CPUS;
    mc->no_parallel = true;
    mc->ignore_memory_transaction_failures = true;
    mc->auto_create_sdcard = false;
}

static const TypeInfo libra_type = {
    .name = TYPE_LIBRA_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(LibraMachineState),
    .class_init = libra_class_init,
    .interfaces = aarch64_machine_interfaces,
};

static void libra_register_types(void)
{
    type_register_static(&libra_type);
}
type_init(libra_register_types)
