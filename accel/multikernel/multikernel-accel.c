/*
 * Multikernel accelerator
 *
 * QEMU is the lifecycle and console manager. The secondary Linux kernel runs
 * on physical CPUs and memory owned by the primary Linux Multikernel kernel.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/accel.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "accel/accel-ops.h"
#include "accel/accel-cpu-ops.h"
#include "accel/dummy-cpus.h"
#include "chardev/char-fe.h"
#include "hw/core/boards.h"
#include "system/cpus.h"
#include "system/multikernel-accel.h"
#include "system/system.h"
#include "multikernel-host.h"

bool multikernel_allowed;

struct MultikernelAccelState {
    AccelState parent_obj;

    unsigned int id;
    char *instance_name;
    int console_fd;
    CharFrontend console;
    Notifier exit_notifier;
    bool console_connected;
    bool payload_loaded;
    bool started;
    bool cleanup_started;
};

static void multikernel_console_disconnect(MultikernelAccelState *s)
{
    if (s->console_fd >= 0) {
        qemu_set_fd_handler(s->console_fd, NULL, NULL, NULL);
    }
    if (s->console_connected) {
        qemu_chr_fe_set_handlers(&s->console, NULL, NULL, NULL, NULL, NULL,
                                 NULL, false);
        qemu_chr_fe_deinit(&s->console, false);
        s->console_connected = false;
    }
    if (s->console_fd >= 0) {
        close(s->console_fd);
        s->console_fd = -1;
    }
}

static int multikernel_console_can_read(void *opaque)
{
    return 4096;
}

static void multikernel_console_receive(void *opaque, const uint8_t *buf,
                                        int size)
{
    MultikernelAccelState *s = opaque;
    int offset = 0;

    while (offset < size) {
        ssize_t result = write(s->console_fd, buf + offset, size - offset);
        if (result > 0) {
            offset += result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        error_report("multikernel: console write failed: %s",
                     result < 0 ? strerror(errno) : "short write");
        return;
    }
}

static void multikernel_console_read(void *opaque)
{
    MultikernelAccelState *s = opaque;
    uint8_t buffer[4096];
    ssize_t length;

    do {
        length = read(s->console_fd, buffer, sizeof(buffer));
    } while (length < 0 && errno == EINTR);

    if (length > 0) {
        qemu_chr_fe_write_all(&s->console, buffer, length);
    } else if (length == 0) {
        multikernel_console_disconnect(s);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        error_report("multikernel: console read failed: %s", strerror(errno));
        multikernel_console_disconnect(s);
    }
}

static bool multikernel_console_connect(MultikernelAccelState *s,
                                        Error **errp)
{
    Chardev *backend = serial_hd(0);

    if (!backend) {
        error_setg(errp, "multikernel requires a serial chardev; use "
                   "-nographic or -serial");
        return false;
    }
    s->console_fd = multikernel_host_open_console(s->id, errp);
    if (s->console_fd < 0) {
        return false;
    }
    if (!qemu_chr_fe_init(&s->console, backend, errp)) {
        close(s->console_fd);
        s->console_fd = -1;
        return false;
    }
    s->console_connected = true;
    qemu_chr_fe_set_handlers(&s->console, multikernel_console_can_read,
                             multikernel_console_receive, NULL, NULL, s,
                             NULL, true);
    qemu_set_fd_handler(s->console_fd, multikernel_console_read, NULL, s);
    return true;
}

static void multikernel_cleanup(MultikernelAccelState *s)
{
    Error *local_err = NULL;
    g_autofree char *status = NULL;

    if (s->cleanup_started) {
        return;
    }
    s->cleanup_started = true;
    multikernel_console_disconnect(s);

    if (!s->payload_loaded || !s->instance_name) {
        return;
    }

    status = multikernel_host_read_status(s->instance_name, &local_err);
    if (!status) {
        error_report_err(local_err);
        return;
    }
    if (!strcmp(status, "active") && s->started) {
        if (multikernel_host_halt(s->id, false, &local_err) < 0) {
            error_report_err(local_err);
            return;
        }
        if (!multikernel_host_wait_status(s->instance_name, "loaded", 5000,
                                          &local_err)) {
            error_report_err(local_err);
            return;
        }
        g_clear_pointer(&status, g_free);
        status = multikernel_host_read_status(s->instance_name, &local_err);
        if (!status) {
            error_report_err(local_err);
            return;
        }
    }
    if (!strcmp(status, "loaded")) {
        if (multikernel_host_unload(s->id, &local_err) < 0) {
            error_report_err(local_err);
            return;
        }
        s->payload_loaded = false;
        s->started = false;
    } else {
        error_report("multikernel: refusing to unload instance %u "
                     "in state '%s'",
                     s->id, status);
    }
}

static void multikernel_exit_notify(Notifier *notifier, void *data)
{
    MultikernelAccelState *s = container_of(notifier,
                                             MultikernelAccelState,
                                             exit_notifier);
    multikernel_cleanup(s);
}

static int multikernel_init_machine(AccelState *as, MachineState *ms)
{
    MultikernelAccelState *s = MULTIKERNEL_ACCEL(as);
    Error *local_err = NULL;
    g_autofree char *status = NULL;

    if (s->id < MULTIKERNEL_MIN_ID || s->id > MULTIKERNEL_MAX_ID) {
        error_report("multikernel: id must be between %d and %d",
                     MULTIKERNEL_MIN_ID, MULTIKERNEL_MAX_ID);
        return -EINVAL;
    }
    if (!ms->kernel_filename) {
        error_report("multikernel: -kernel is required");
        return -EINVAL;
    }
    if (!ms->initrd_filename) {
        error_report("multikernel: -initrd is required for the boot MVP");
        return -EINVAL;
    }

    s->instance_name = multikernel_host_find_instance(s->id, &local_err);
    if (!s->instance_name) {
        error_report_err(local_err);
        return -ENOENT;
    }
    status = multikernel_host_read_status(s->instance_name, &local_err);
    if (!status) {
        error_report_err(local_err);
        return -EIO;
    }
    if (strcmp(status, "ready")) {
        error_report("multikernel: instance %u ('%s') must be ready, is '%s'",
                     s->id, s->instance_name, status);
        return -EBUSY;
    }

    s->exit_notifier.notify = multikernel_exit_notify;
    qemu_add_exit_notifier(&s->exit_notifier);
    return 0;
}

static void multikernel_setup_post(AccelState *as)
{
    MultikernelAccelState *s = MULTIKERNEL_ACCEL(as);
    MachineState *ms = MACHINE(qdev_get_machine());
    Error *local_err = NULL;

    if (multikernel_host_load(s->id, ms->kernel_filename,
                              ms->initrd_filename, ms->kernel_cmdline,
                              &local_err) < 0) {
        error_report_err(local_err);
        exit(EXIT_FAILURE);
    }
    s->payload_loaded = true;
    if (!multikernel_host_wait_status(s->instance_name, "loaded", 5000,
                                      &local_err)) {
        error_report_err(local_err);
        multikernel_cleanup(s);
        exit(EXIT_FAILURE);
    }
    if (!multikernel_console_connect(s, &local_err)) {
        error_report_err(local_err);
        multikernel_cleanup(s);
        exit(EXIT_FAILURE);
    }
    if (multikernel_host_exec(s->id, &local_err) < 0) {
        error_report_err(local_err);
        multikernel_cleanup(s);
        exit(EXIT_FAILURE);
    }
    s->started = true;
    if (!multikernel_host_wait_status(s->instance_name, "active", 5000,
                                      &local_err)) {
        error_report_err(local_err);
        multikernel_cleanup(s);
        exit(EXIT_FAILURE);
    }
}

static void multikernel_get_id(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    uint64_t value = MULTIKERNEL_ACCEL(obj)->id;
    visit_type_uint64(v, name, &value, errp);
}

static void multikernel_set_id(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (value > MULTIKERNEL_MAX_ID) {
        error_setg(errp, "multikernel id must be at most %d",
                   MULTIKERNEL_MAX_ID);
        return;
    }
    MULTIKERNEL_ACCEL(obj)->id = value;
}

static void multikernel_accel_initfn(Object *obj)
{
    MULTIKERNEL_ACCEL(obj)->console_fd = -1;
}

static void multikernel_accel_finalize(Object *obj)
{
    MultikernelAccelState *s = MULTIKERNEL_ACCEL(obj);
    multikernel_console_disconnect(s);
    g_free(s->instance_name);
}

static void multikernel_accel_class_init(ObjectClass *oc, const void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);

    ac->name = "Multikernel";
    ac->init_machine = multikernel_init_machine;
    ac->setup_post = multikernel_setup_post;
    ac->allowed = &multikernel_allowed;

    object_class_property_add(oc, "instance-id", "uint64", multikernel_get_id,
                              multikernel_set_id, NULL, NULL);
    object_class_property_set_description(
        oc, "instance-id", "Existing Multikernel instance ID");
}

static const TypeInfo multikernel_accel_type = {
    .name = TYPE_MULTIKERNEL_ACCEL,
    .parent = TYPE_ACCEL,
    .instance_size = sizeof(MultikernelAccelState),
    .instance_init = multikernel_accel_initfn,
    .instance_finalize = multikernel_accel_finalize,
    .class_init = multikernel_accel_class_init,
};
module_obj(TYPE_MULTIKERNEL_ACCEL);

static bool multikernel_cpus_are_resettable(void)
{
    return false;
}

static void multikernel_accel_ops_class_init(ObjectClass *oc, const void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    ops->create_vcpu_thread = dummy_start_vcpu_thread;
    ops->handle_interrupt = generic_handle_interrupt;
    ops->cpus_are_resettable = multikernel_cpus_are_resettable;
}

static const TypeInfo multikernel_accel_ops_type = {
    .name = ACCEL_OPS_NAME("multikernel"),
    .parent = TYPE_ACCEL_OPS,
    .class_init = multikernel_accel_ops_class_init,
    .abstract = true,
};
module_obj(ACCEL_OPS_NAME("multikernel"));

static void multikernel_type_init(void)
{
    type_register_static(&multikernel_accel_type);
    type_register_static(&multikernel_accel_ops_type);
}

type_init(multikernel_type_init);
