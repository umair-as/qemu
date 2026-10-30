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
   registers, the Cortex-A55 reset vectors and the Cortex-A55 side of the
   sleep handshake with the Cortex-M33
 * System counter (SYC, a CoreSight timestamp generator): the control
   registers firmware programs
 * TrustZone Address Space Controllers TZC0-3 (TZC-400): programming is
   stored, not enforced
 * On-chip SRAM for the Cortex-M33 and the Cortex-A55
 * Serial ports SCIF0-4 (SCIFA), asynchronous mode: transmit, receive,
   loopback and the five interrupt lines.
   SCIF0 is the board console and takes the first ``-serial`` backend;
   SCIF1-4 take the next ones.
 * Clock Pulse Generator (CPG): the clock, reset and module-stop
   registers, with their monitors; the clocks and resets have no effect
   on the other devices, except the watchdog reset lines and the
   Cortex-A55 cores, which the CPG starts and stops through their reset
   lines and P-Channel power handshake
 * OS timers OSTM0-2
 * Pin function controller: the pin registers, without electrical
   effect
 * Interrupt control unit (IA55): the registers only, no interrupt
   routing
 * Watchdog timers WDT0-2. A watchdog reset request resets the system
   when the CPG's ``CPG_WDTRST_SEL`` allows it; ``-action watchdog=``
   selects what a reset does.
 * SD/MMC host interfaces SDHI0 and SDHI1, with their built-in DMA
   controllers, for SD memory cards. Transfers complete at once, so the
   data timeout never fires; SDIO, HPI and the sampling clock controller
   (bus tuning) are not modelled.

Missing devices
---------------

Ethernet, I2C, USB, the QSPI controller, the display, CAN-FD,
the ADC, the thermal sensor and the DMA controller are stubs: they log
every access and read as zero, so their drivers fail when they probe.
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

SD cards
--------

``-drive if=sd,index=0`` is the card in the slot on the SMARC module
(SDHI0), the slot the board boots from in eSD mode; ``index=1`` is the
slot on the carrier board (SDHI1). A slot without a drive is empty. The
image size must be a power of two up to 2 GiB, or a multiple of 512 KiB
above. When the device tree loaded with ``-dtb`` has no ``mmc0`` or
``mmc1`` alias, the machine adds them for SDHI0 and SDHI1, so Linux
names the cards ``mmcblk0`` and ``mmcblk1`` in every boot.

.. code-block:: bash

  $ qemu-system-aarch64 -M rzv2l-smarc ... \
      -drive if=sd,index=0,format=raw,file=sd.img

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

With ``-bios``, the image is the board's Trusted Firmware-A BL31, which
runs at EL3 and is the PSCI firmware: it starts and stops the second
core through the SoC's reset and power registers, and resets the board
through WDT0. The machine does what the board's BL2 does before BL31:
it loads BL31 at 0x44000000, the ``-kernel`` image (an arm64 Linux
``Image``) at 0x48200000 and its device tree at 0x58000000, and leaves
BL2's parameter block for BL31 at 0x2f000 in SRAM. The kernel is BL31's
next image, entered at non-secure EL1. The boot ROM, BL2 and the DDR
initialisation are not run, there is no OP-TEE, and ``-initrd`` is not
supported. The firmware does not implement PSCI ``SYSTEM_OFF``.

.. code-block:: bash

  $ qemu-system-aarch64 -M rzv2l-smarc \
      -bios bl31.bin \
      -kernel Image \
      -dtb r9a07g054l2-smarc.dtb \
      -append "rw rootwait root=/dev/mmcblk0p2" \
      -drive if=sd,index=0,format=raw,file=sd.img
