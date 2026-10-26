/*
NVNET_REGS.H

The registers of the Xbox's network card (the nForce MCP's Ethernet controller) and their bits: the ones the driver
(port/third_party/nxdk_net/nvnetdrv.c) uses, under the names it uses. Offsets from the card's memory window
(0xFEF00000).
*/

#ifndef NVNET_REGS_H
#define NVNET_REGS_H

/* registers */
enum
{
	NvRegIrqStatus = 0x000,
	NvRegIrqMask = 0x004,
	NvRegPollingControl = 0x008,
	NvRegDuplexMode = 0x080,
	NvRegTransmitterControl = 0x084,
	NvRegTransmitterStatus = 0x088,
	NvRegPacketFilterFlags = 0x08C,
	NvRegOffloadConfig = 0x090,
	NvRegReceiverControl = 0x094,
	NvRegReceiverStatus = 0x098,
	NvRegSlotTime = 0x09C,
	NvRegTxDeferral = 0x0A0,
	NvRegRxDeferral = 0x0A4,
	NvRegMacAddrA = 0x0A8,
	NvRegMacAddrB = 0x0AC,
	NvRegMulticastAddrA = 0x0B0,
	NvRegMulticastAddrB = 0x0B4,
	NvRegMulticastMaskA = 0x0B8,
	NvRegMulticastMaskB = 0x0BC,
	NvRegTxRingPhysAddr = 0x100,
	NvRegRxRingPhysAddr = 0x104,
	NvRegRingSizes = 0x108,             /* (each ring's size less one) */
	NvRegTransmitPoll = 0x10C,
	NvRegLinkSpeed = 0x110,
	NvRegTxWatermark = 0x13C,
	NvRegUnknownSetupReg7 = 0x140,
	NvRegTxRxControl = 0x144,
	NvRegMIIStatus = 0x180,
	NvRegMIIMask = 0x184,
	NvRegAdapterControl = 0x188,
	NvRegMIISpeed = 0x18C,
	NvRegWakeUpFlags = 0x200,
};

/* NvRegIrqStatus, NvRegIrqMask */
#define NVREG_IRQ_RX_ERROR 0x0001
#define NVREG_IRQ_RX 0x0002
#define NVREG_IRQ_RX_NOBUF 0x0004
#define NVREG_IRQ_TX_ERROR 0x0008
#define NVREG_IRQ_TX_OK 0x0010
#define NVREG_IRQ_TIMER 0x0020
#define NVREG_IRQ_LINK 0x0040
#define NVREG_IRQ_RX_FORCED 0x0080
#define NVREG_IRQ_TX_FORCED 0x0100
#define NVREG_IRQ_RX_ALL (NVREG_IRQ_RX_ERROR | NVREG_IRQ_RX | NVREG_IRQ_RX_NOBUF | NVREG_IRQ_RX_FORCED)
#define NVREG_IRQ_TX_ALL (NVREG_IRQ_TX_ERROR | NVREG_IRQ_TX_OK | NVREG_IRQ_TX_FORCED)
#define NVREG_IRQMASK_THROUGHPUT (NVREG_IRQ_RX_ERROR | NVREG_IRQ_RX | NVREG_IRQ_RX_NOBUF | NVREG_IRQ_TX_ERROR | \
	NVREG_IRQ_TX_OK | NVREG_IRQ_LINK | NVREG_IRQ_RX_FORCED)

/* NvRegDuplexMode */
#define NVREG_DUPLEX_MODE_HDFLAG 0x00000002
#define NVREG_DUPLEX_MODE_FDMASK 0xFFFFFFFD
#define NVREG_DUPLEX_MODE_FORCEH 0x003B0F3E

/* NvRegTransmitterControl, NvRegTransmitterStatus, NvRegReceiverControl, NvRegReceiverStatus */
#define NVREG_XMITCTL_START 0x01
#define NVREG_XMITSTAT_BUSY 0x01
#define NVREG_RCVCTL_START 0x01
#define NVREG_RCVSTAT_BUSY 0x01

/* NvRegPacketFilterFlags, NvRegOffloadConfig */
#define NVREG_PFF_ALWAYS_MYADDR 0x007F0020
#define NVREG_OFFLOAD_NORMAL 0x5EE

/* NvRegSlotTime, NvRegTxDeferral, NvRegRxDeferral */
#define NVREG_SLOTTIME_MASK 0x000000FF
#define NVREG_SLOTTIME_10_100_FULL 0x00007F00
#define NVREG_TX_DEFERRAL_RGMII_10_100 0x0016070F
#define NVREG_RX_DEFERRAL_DEFAULT 0x16

/* NvRegMulticast* (no multicast) */
#define NVREG_MCASTMASKA_NONE 0xFFFFFFFF
#define NVREG_MCASTMASKB_NONE 0xFFFF

/* NvRegRingSizes */
#define NVREG_RINGSZ_TXSHIFT 0
#define NVREG_RINGSZ_RXSHIFT 16

/* NvRegLinkSpeed */
#define NVREG_LINKSPEED_FORCE 0x10000
#define NVREG_LINKSPEED_10MBPS 1000
#define NVREG_LINKSPEED_100MBPS 100

/* NvRegUnknownSetupReg7 and NvRegTxWatermark: the dashboard's value */
#define NVREG_UNKSETUP7_VAL1 0x300010

/* NvRegTxRxControl */
#define NVREG_TXRXCTL_KICK 0x0001
#define NVREG_TXRXCTL_GET 0x0002
#define NVREG_TXRXCTL_DISABLE 0x0004
#define NVREG_TXRXCTL_IDLE 0x0008
#define NVREG_TXRXCTL_RESET 0x0010

/* NvRegMIIStatus, NvRegMIIMask, NvRegAdapterControl, NvRegMIISpeed */
#define NVREG_MIISTAT_LINKCHANGE 0x0008
#define NVREG_MII_LINKCHANGE 0x0008
#define NVREG_ADAPTCTL_PHYVALID 0x40000
#define NVREG_ADAPTCTL_RUNNING 0x100000
#define NVREG_ADAPTCTL_PHYSHIFT 24
#define NVREG_MIISPEED_BIT8 (1 << 8)
#define NVREG_MIIDELAY 5

/* the descriptors' flags */
#define NV_TX_LASTPACKET (1 << 0)
#define NV_TX_VALID (1 << 15)
#define NV_RX_DESCRIPTORVALID (1 << 0)
#define NV_RX_MISSEDFRAME (1 << 1)
#define NV_RX_SUBTRACT1 (1 << 2)
#define NV_RX_ERROR1 (1 << 7)
#define NV_RX_ERROR2 (1 << 8)
#define NV_RX_ERROR3 (1 << 9)
#define NV_RX_ERROR4 (1 << 10)
#define NV_RX_CRCERR (1 << 11)
#define NV_RX_OVERFLOW (1 << 12)
#define NV_RX_FRAMINGERR (1 << 13)
#define NV_RX_AVAIL (1 << 15)
#define NV_RX_ERROR_MASK (NV_RX_ERROR1 | NV_RX_ERROR2 | NV_RX_ERROR3 | NV_RX_ERROR4 | NV_RX_CRCERR | NV_RX_OVERFLOW)

#endif
