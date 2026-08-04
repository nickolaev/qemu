/*
 * Multikernel accelerator
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_MULTIKERNEL_ACCEL_H
#define QEMU_MULTIKERNEL_ACCEL_H

#include "qemu/accel.h"
#include "qom/object.h"

#define TYPE_MULTIKERNEL_ACCEL ACCEL_CLASS_NAME("multikernel")
OBJECT_DECLARE_SIMPLE_TYPE(MultikernelAccelState, MULTIKERNEL_ACCEL)

extern bool multikernel_allowed;

#endif
