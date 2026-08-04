/*
 * Multikernel control-plane machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/accel.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "system/block-backend-global-state.h"
#include "system/multikernel-accel.h"

static void multikernel_machine_init(MachineState *machine)
{
    if (!object_dynamic_cast(OBJECT(current_accel()),
                             TYPE_MULTIKERNEL_ACCEL)) {
        error_report("machine 'multikernel' requires accel 'multikernel'");
        exit(EXIT_FAILURE);
    }
    if (!machine->kernel_filename) {
        error_report("machine 'multikernel' requires -kernel");
        exit(EXIT_FAILURE);
    }
    if (!machine->initrd_filename) {
        error_report("machine 'multikernel' requires -initrd for the boot MVP");
        exit(EXIT_FAILURE);
    }
    if (machine->firmware) {
        error_report("machine 'multikernel' does not support firmware boot");
        exit(EXIT_FAILURE);
    }
    if (blk_next(NULL)) {
        error_report("machine 'multikernel' does not support block devices");
        exit(EXIT_FAILURE);
    }
}

static void multikernel_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Multikernel secondary Linux control plane";
    mc->init = multikernel_machine_init;
    mc->default_ram_size = 0;
    mc->default_ram_id = NULL;
    mc->max_cpus = 1;
    mc->no_cdrom = true;
    mc->no_floppy = true;
    mc->no_parallel = true;
    mc->default_boot_order = "";
}

static const TypeInfo multikernel_machine_type = {
    .name = MACHINE_TYPE_NAME("multikernel"),
    .parent = TYPE_MACHINE,
    .class_init = multikernel_machine_class_init,
};

static void multikernel_machine_register_types(void)
{
    type_register_static(&multikernel_machine_type);
}

type_init(multikernel_machine_register_types)
