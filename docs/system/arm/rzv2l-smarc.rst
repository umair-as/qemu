.. SPDX-License-Identifier: GPL-2.0-or-later

Renesas RZ/V2L SMARC Evaluation Kit (``rzv2l-smarc``)
=====================================================

The ``rzv2l-smarc`` machine models the Renesas RZ/V2L SMARC Evaluation
Kit: the RZ/V2L (R9A07G054L2) SMARC module with 2 GiB of DDR4.

Supported devices
-----------------

 * 2 Cortex-A55 cores
 * Generic Interrupt Controller (GICv3, identifying as an Arm GIC-600 r1p6)
 * System Controller (SYSC): the device ID, product and boot mode
   registers only
 * Serial ports SCIF0-4 (SCIFA), asynchronous mode: transmit, receive,
   loopback and the five interrupt lines.
   SCIF0 is the board console and takes the first ``-serial`` backend;
   SCIF1-4 take the next ones.

Missing devices
---------------

Every other on-chip block is missing. The serial ports have no modem
lines, break or error detection, and do not enforce the baud rate.
Accesses to unmodelled addresses,
and to SYSC registers other than the identification registers, fail
with an external abort instead of reading as zero.

When a kernel is loaded with ``-kernel``, SCIF0 starts in the state the
board's boot firmware leaves the console in (115200 8N1, transmitter and
receiver enabled), so the kernel's early console works without the
firmware.

Machine options
---------------

``boot-mode=<0-5>``
  The boot mode pins, MD_BOOT[2:0], as reported in ``SYS_LSI_MODE``.
  The default is 0 (eSD). Modes 6 and 7 are reserved and refused.

``debugen=on|off``
  The DEBUGEN pin, as reported in ``SYS_LSI_MODE``. The default is off.

The machine has exactly two CPUs and 2 GiB of RAM; other ``-smp`` and
``-m`` values are refused.

Boot options
------------

The machine can start a Linux kernel directly using the standard
``-kernel`` functionality. The kernel is entered at non-secure EL1, and
QEMU's built-in PSCI implementation answers SMC calls in place of the
board's Trusted Firmware-A. The kernel is given the memory window the
board firmware gives it, 0x48000000 to 0xbfffffff; the first 128 MiB of
DDR are left to the secure firmware.

.. code-block:: bash

  $ qemu-system-aarch64 -M rzv2l-smarc \
      -kernel Image \
      -dtb r9a07g054l2-smarc.dtb
