/* oxbox_quiesce.h: the network card and the USB host controllers stopped writing to memory (v1.171)

A quick reboot into another XBE (HalReturnToFirmware(HalQuickRebootRoutine), as nxdk's XLaunchXBE and the game's own
launches do) doesn't reset the hardware. A network card left running keeps putting every packet that arrives into its
receive buffers, and a USB host controller keeps writing its frame counter and finished transfers, by DMA, into the
memory the last program gave them: the next one's memory now. The installer (nxdk) starts the card for its update
check and reads the pads through nxdk's USB stack, and stopped neither before starting H1U, whose own network starts
only after the intro: v1.168 and v1.170 froze dead just after the intro, both started from the installer, the log and
the boot watch silent.

oxbox_quiesce_devices() stops them through their registers, as nxdk's nvnetdrv_stop and an OHCI reset do: the
installer calls it before it launches anything, the game first thing at start and before each of its own launches.
The game's network (nvnetdrv_init) and XAPI's USB (XInitDevices) set both up again when they start.

Include it once with OXBOX_QUIESCE_DEFINE defined to define the function; elsewhere it only declares it. */

#ifndef OXBOX_QUIESCE_H
#define OXBOX_QUIESCE_H

void oxbox_quiesce_devices(void);

#if defined(OXBOX_QUIESCE_DEFINE)
void oxbox_quiesce_devices(void)
{
	/* the network card (registers at 0xFEF00000, port/oxbox/net/nvnet_regs.h's names) */
	volatile unsigned long *nic = (volatile unsigned long *)0xFEF00000;
	volatile unsigned long *usb;
	long k;

	nic[0x004 / 4] = 0;                         /* NvRegIrqMask: no interrupts */
	nic[0x094 / 4] &= ~1ul;                     /* NvRegReceiverControl: the receiver stopped */
	nic[0x084 / 4] &= ~1ul;                     /* NvRegTransmitterControl: the transmitter stopped */
	nic[0x144 / 4] = 0x0004;                    /* NvRegTxRxControl: DMA disabled... */
	for (k = 0; k < 20000 && !(nic[0x144 / 4] & 0x0008); k++)
		;                                       /* (...till it's idle: a register read is about a microsecond) */
	nic[0x144 / 4] = 0x0004 | 0x0010;           /* ...and reset */
	for (k = 0; k < 20; k++)
		(void)nic[0x144 / 4];
	nic[0x144 / 4] = 0x0004;
	nic[0x100 / 4] = 0;                         /* NvRegTxRingPhysAddr: no rings */
	nic[0x104 / 4] = 0;                         /* NvRegRxRingPhysAddr */
	nic[0x000 / 4] = 0xFFFFFFFFul;              /* NvRegIrqStatus: what's pending acknowledged */
	/* the two USB host controllers (OHCI, at 0xFED00000 and 0xFED08000): their interrupts off, then the UsbReset
	state, in which they process no lists and write nothing */
	for (usb = (volatile unsigned long *)0xFED00000; usb <= (volatile unsigned long *)0xFED08000; usb += 0x8000 / 4)
	{
		usb[0x14 / 4] = 0x8000007Ful;           /* HcInterruptDisable: all of them, and the master */
		usb[0x04 / 4] = 0;                      /* HcControl: UsbReset, the lists off */
	}
}
#endif

#endif
