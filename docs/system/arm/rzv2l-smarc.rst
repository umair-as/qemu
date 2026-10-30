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
 * Clock Pulse Generator (CPG): the clock, reset and module-stop
   registers, with their monitors; the clocks and resets have no effect
   on the other devices, except the watchdog reset lines
 * OS timers OSTM0-2
 * Pin function controller: the pin registers, without electrical
   effect
 * Interrupt control unit (IA55): the registers, and the IRQ0-7 pin
   interrupts to the GIC (level and edge detection); the NMI and the
   GPIO interrupts TINT0-31 are not routed
 * Watchdog timers WDT0-2. A watchdog reset request resets the system
   when the CPG's ``CPG_WDTRST_SEL`` allows it; ``-action watchdog=``
   selects what a reset does.
 * SD/MMC host interfaces SDHI0 and SDHI1, with their built-in DMA
   controllers, for SD memory cards. Transfers complete at once, so the
   data timeout never fires; SDIO, HPI and the sampling clock controller
   (bus tuning) are not modelled.
 * Gigabit Ethernet interfaces ETH0 and ETH1, each with its Microchip
   KSZ9131 PHY at MDIO address 7, whose interrupt reaches the GIC through
   IRQ2 (ETH0) and IRQ3 (ETH1) of the IA55: the descriptor DMA
   controller, the MAC with the MDIO pin interface, and the TCP/IP
   checksum offload. Transfers complete at once. Flow control, the
   hardware MDIO controller, energy-efficient Ethernet, the receive
   filters, wake-up and ARP offload are not modelled.

Missing devices
---------------

I2C, USB, the QSPI controller, the display, CAN-FD,
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

Networking
----------

The first ``-nic`` (or ``-net nic``) is ETH0, the second ETH1. A channel
without a network backend has no cable: its PHY reports the link down.
``set_link`` on the backend unplugs and plugs the cable. When a kernel
is loaded with ``-kernel``, the machine writes each channel's MAC
address into the ``local-mac-address`` property of the node the
``ethernet0`` and ``ethernet1`` aliases name, as the board's U-Boot does.

.. code-block:: bash

  $ qemu-system-aarch64 -M rzv2l-smarc ... -nic user

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
