.. _multikernel:

========================
Multikernel accelerator
========================

The ``multikernel`` accelerator makes QEMU the control-plane and console
manager for an existing Linux Multikernel secondary instance.  It is not a
hardware virtualizer: the secondary Linux kernel runs natively on physical CPUs
and memory that are assigned and owned by the primary Linux kernel.  QEMU loads
and starts the secondary kernel, connects its console, and cleans up the
payload when QEMU exits.

This feature is experimental and currently available only on Linux x86_64
hosts.

Prerequisites
-------------

Before starting QEMU:

* Build QEMU with Multikernel support.  Configure with
  ``-Dmultikernel=enabled`` (or ``--enable-multikernel``) and confirm that the
  configure summary lists ``multikernel`` as an accelerator.
* Run a primary Linux kernel that provides the Multikernel host interface,
  including ``/sys/fs/multikernel/instances`` and ``/dev/mktty``.
* Create and configure a secondary instance using the host's Multikernel
  management interface.  The instance ID must be unique and its status must be
  ``ready``.  QEMU does not create the instance or allocate its CPUs, memory,
  or devices.
* Have permission to use the host's Multikernel kexec and lifecycle operations,
  and permission to access ``/dev/mktty``.  In typical configurations this
  requires elevated privileges.
* Provide a Linux kernel image and an initramfs for the secondary instance.  The
  secondary kernel must use the Multikernel console; normally this is done with
  ``console=mktty0`` in the kernel command line.

Usage
-----

Use the ``multikernel`` machine and accelerator, selecting the existing
instance with the accelerator's ``instance-id`` property.  Provide a serial
chardev, for example through ``-nographic``:

.. code-block:: shell

  qemu-system-x86_64 \
      -machine multikernel \
      -accel multikernel,instance-id=1 \
      -kernel /path/to/vmlinux \
      -initrd /path/to/initramfs.cpio.gz \
      -append 'rdinit=/init console=mktty0' \
      -nographic

QEMU checks that the selected instance is ``ready``, loads the kernel and
initramfs into it through the host Multikernel interface, opens the selected
instance's ``/dev/mktty`` console, and starts the instance.  The console is
then connected to QEMU's first serial chardev.

On normal QEMU exit, QEMU closes the console and, for payload state it loaded,
requests a graceful halt followed by an unload.  An instance that was merely
ready when QEMU was started is left allocated; QEMU never removes its host-owned
CPUs, memory, or device assignments.

Limitations
-----------

The current implementation is a boot and console control plane only.  In
particular:

* Only an already-created, ready Multikernel instance can be used.
* Only direct Linux boot with both ``-kernel`` and ``-initrd`` is supported.
  Firmware, BIOS, UEFI, pflash, PXE, disk, CD-ROM, and floppy boot are not
  supported.
* QEMU does not virtualize secondary CPUs, own or map secondary RAM, or provide
  an emulated PCI topology.  It cannot inspect or modify the secondary's
  memory.
* Ordinary QEMU devices, including block, network, virtio, and VFIO devices,
  are not a transport to the secondary instance.  Use resources assigned by the
  primary Multikernel host instead.
* Migration, snapshots, save/restore, reset, pause, and resume are not
  supported.  QEMU cannot provide these semantics for natively executing
  secondary CPUs.
* Lifecycle operations depend on the host Multikernel ABI and can fail if the
  instance changes state or the required privileges are unavailable.  QEMU
  reports the host error and avoids unloading an instance unless it can confirm
  that it is in the expected state.
