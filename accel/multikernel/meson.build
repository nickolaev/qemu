# SPDX-License-Identifier: GPL-2.0-or-later

multikernel_ss = ss.source_set()
multikernel_ss.add(files('multikernel-accel.c', 'multikernel-host.c'))
system_ss.add_all(when: 'CONFIG_MULTIKERNEL', if_true: multikernel_ss)
