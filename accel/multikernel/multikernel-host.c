/*
 * Linux Multikernel host control interface
 *
 * Keep the downstream syscall and sysfs ABI in this file so it can be
 * replaced by a stable ioctl interface without changing the accelerator.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "multikernel-host.h"

#include <sys/syscall.h>

#define MULTIKERNEL_INSTANCES "/sys/fs/multikernel/instances"
#define MULTIKERNEL_CONSOLE "/dev/mktty"

#define KEXEC_FILE_UNLOAD 0x00000001
#define KEXEC_FILE_NO_INITRAMFS 0x00000004
#define KEXEC_MULTIKERNEL 0x00000010
#define KEXEC_MK_ID_MASK 0x0000ffe0
#define KEXEC_MK_ID_SHIFT 5
#define KEXEC_MK_ID(id) (((id) << KEXEC_MK_ID_SHIFT) & KEXEC_MK_ID_MASK)

#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 672274793
#define LINUX_REBOOT_CMD_MULTIKERNEL 0x4d4b4c49
#define LINUX_REBOOT_CMD_MULTIKERNEL_HALT 0x4d4b4c48
#define LINUX_REBOOT_CMD_MULTIKERNEL_HALT_FORCE 0x4d4b4c46

typedef struct MultikernelBootArgs {
    int mk_id;
} MultikernelBootArgs;

static char *multikernel_host_read_file(const char *path, Error **errp)
{
    g_autofree char *contents = NULL;
    gsize length;
    GError *gerror = NULL;

    if (!g_file_get_contents(path, &contents, &length, &gerror)) {
        error_setg(errp, "cannot read %s: %s", path, gerror->message);
        g_error_free(gerror);
        return NULL;
    }

    return g_strdup(g_strstrip(contents));
}

char *multikernel_host_find_instance(unsigned int id, Error **errp)
{
    g_autoptr(GDir) directory = NULL;
    const char *entry;
    GError *gerror = NULL;
    char *match = NULL;

    directory = g_dir_open(MULTIKERNEL_INSTANCES, 0, &gerror);
    if (!directory) {
        error_setg(errp, "cannot open %s: %s", MULTIKERNEL_INSTANCES,
                   gerror->message);
        g_error_free(gerror);
        return NULL;
    }

    while ((entry = g_dir_read_name(directory))) {
        g_autofree char *id_path = g_build_filename(MULTIKERNEL_INSTANCES,
                                                    entry, "id", NULL);
        g_autofree char *contents = NULL;
        const char *end = NULL;
        unsigned long found;

        contents = multikernel_host_read_file(id_path, NULL);
        if (!contents) {
            continue;
        }
        if (qemu_strtoul(contents, &end, 10, &found) || end == contents ||
            *end || found != id) {
            continue;
        }
        if (match) {
            error_setg(errp, "Multikernel instance ID %u is not unique", id);
            g_free(match);
            return NULL;
        }
        match = g_strdup(entry);
    }

    if (!match) {
        error_setg(errp, "Multikernel instance ID %u was not found", id);
    }
    return match;
}

char *multikernel_host_read_status(const char *instance_name, Error **errp)
{
    g_autofree char *path = g_build_filename(MULTIKERNEL_INSTANCES,
                                             instance_name, "status", NULL);
    return multikernel_host_read_file(path, errp);
}

bool multikernel_host_wait_status(const char *instance_name,
                                  const char *expected,
                                  unsigned int timeout_ms,
                                  Error **errp)
{
    const unsigned int interval_ms = 10;
    unsigned int elapsed = 0;

    do {
        g_autofree char *status = multikernel_host_read_status(instance_name,
                                                               errp);
        if (!status) {
            return false;
        }
        if (!strcmp(status, expected)) {
            return true;
        }
        if (elapsed >= timeout_ms) {
            error_setg(errp,
                       "Multikernel instance '%s' did not reach state '%s' "
                       "within %u ms (current state '%s')",
                       instance_name, expected, timeout_ms, status);
            return false;
        }
        g_usleep(interval_ms * 1000);
        elapsed += interval_ms;
    } while (true);
}

int multikernel_host_load(unsigned int id, const char *kernel,
                          const char *initrd, const char *cmdline,
                          Error **errp)
{
    int kernel_fd = -1;
    int initrd_fd = -1;
    unsigned long flags = KEXEC_MULTIKERNEL | KEXEC_MK_ID(id);
    size_t cmdline_len = cmdline && *cmdline ? strlen(cmdline) + 1 : 0;
    long result;
    int saved_errno;

    kernel_fd = open(kernel, O_RDONLY | O_CLOEXEC);
    if (kernel_fd < 0) {
        error_setg_errno(errp, errno, "cannot open kernel '%s'", kernel);
        return -errno;
    }
    if (initrd) {
        initrd_fd = open(initrd, O_RDONLY | O_CLOEXEC);
        if (initrd_fd < 0) {
            saved_errno = errno;
            error_setg_errno(errp, saved_errno, "cannot open initramfs '%s'",
                             initrd);
            close(kernel_fd);
            return -saved_errno;
        }
    } else {
        flags |= KEXEC_FILE_NO_INITRAMFS;
    }

    result = syscall(SYS_kexec_file_load, kernel_fd, initrd_fd, cmdline_len,
                     cmdline_len ? cmdline : NULL, flags);
    saved_errno = errno;
    if (initrd_fd >= 0) {
        close(initrd_fd);
    }
    close(kernel_fd);
    if (result < 0) {
        error_setg_errno(errp, saved_errno,
                         "cannot load Multikernel instance %u", id);
        return -saved_errno;
    }
    return 0;
}

static int multikernel_host_reboot(unsigned int id, unsigned int command,
                                   Error **errp)
{
    MultikernelBootArgs args = { .mk_id = id };
    long result;
    int saved_errno;

    result = syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                     command, &args);
    saved_errno = errno;
    if (result < 0) {
        error_setg_errno(errp, saved_errno,
                         "Multikernel lifecycle command 0x%x failed for "
                         "instance %u", command, id);
        return -saved_errno;
    }
    return 0;
}

int multikernel_host_exec(unsigned int id, Error **errp)
{
    return multikernel_host_reboot(id, LINUX_REBOOT_CMD_MULTIKERNEL, errp);
}

int multikernel_host_halt(unsigned int id, bool force, Error **errp)
{
    return multikernel_host_reboot(
        id, force ? LINUX_REBOOT_CMD_MULTIKERNEL_HALT_FORCE
                  : LINUX_REBOOT_CMD_MULTIKERNEL_HALT,
        errp);
}

int multikernel_host_unload(unsigned int id, Error **errp)
{
    unsigned long flags = KEXEC_FILE_UNLOAD | KEXEC_MULTIKERNEL |
                          KEXEC_MK_ID(id);
    long result;
    int saved_errno;

    result = syscall(SYS_kexec_file_load, -1, -1, 0, NULL, flags);
    saved_errno = errno;
    if (result < 0) {
        error_setg_errno(errp, saved_errno,
                         "cannot unload Multikernel instance %u", id);
        return -saved_errno;
    }
    return 0;
}

int multikernel_host_open_console(unsigned int id, Error **errp)
{
    g_autofree char *selector = g_strdup_printf("%u\n", id);
    size_t selector_len = strlen(selector);
    size_t written = 0;
    int fd;

    fd = open(MULTIKERNEL_CONSOLE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error_setg_errno(errp, errno, "cannot open %s", MULTIKERNEL_CONSOLE);
        return -errno;
    }
    while (written < selector_len) {
        ssize_t result = write(fd, selector + written, selector_len - written);
        if (result < 0) {
            int saved_errno = errno;
            if (saved_errno == EINTR) {
                continue;
            }
            error_setg_errno(errp, saved_errno,
                             "cannot select Multikernel instance %u on %s",
                             id, MULTIKERNEL_CONSOLE);
            close(fd);
            return -saved_errno;
        }
        written += result;
    }
    return fd;
}
