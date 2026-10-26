/*
OXBOX_KERNEL.H

The Xbox kernel's types and calls that the network card driver and lwIP's port use (port/third_party/nxdk_net),
for code that doesn't include the XDK headers (platform.h). The calls go through oxbox_kernel_export (oxbox_entry.c),
the kernel's export table by ordinal: Halo's XBE imports only what Halo used.

Layouts and ordinals: the Xbox kernel's, as nxdk's xboxkrnl.h (CC0) and xboxkrnl.exe.def give them.
*/

#ifndef OXBOX_KERNEL_H
#define OXBOX_KERNEL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NTAPI __attribute__((stdcall))
#define FASTCALL __attribute__((fastcall))

typedef unsigned char UCHAR, BOOLEAN, KIRQL, *PKIRQL;
typedef char CCHAR, KPROCESSOR_MODE;
typedef short CSHORT;
typedef long LONG, NTSTATUS, KPRIORITY;
typedef unsigned long ULONG, *PULONG, DWORD, ULONG_PTR, SIZE_T;
typedef void VOID, *PVOID;
typedef void *HANDLE;

typedef union
{
	struct { ULONG LowPart; LONG HighPart; } u;
	long long QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;

typedef struct
{
	UCHAR Type, Absolute, Size, Inserted;
	LONG SignalState;
	LIST_ENTRY WaitListHead;
} DISPATCHER_HEADER;

typedef struct
{
	DISPATCHER_HEADER Header;
	LONG Limit;
} KSEMAPHORE, *PKSEMAPHORE, *PRKSEMAPHORE;

typedef struct _KDPC
{
	CSHORT Type;
	BOOLEAN Inserted;
	UCHAR Padding;
	LIST_ENTRY DpcListEntry;
	PVOID DeferredRoutine, DeferredContext, SystemArgument1, SystemArgument2;
} KDPC, *PKDPC, *PRKDPC;

typedef struct _KINTERRUPT
{
	PVOID ServiceRoutine, ServiceContext;
	ULONG BusInterruptLevel, Irql;
	UCHAR Connected, ShareVector, Mode, Padding7;
	ULONG ServiceCount;
	ULONG DispatchCode[22];
} KINTERRUPT, *PKINTERRUPT;

typedef BOOLEAN (NTAPI *PKSERVICE_ROUTINE)(PKINTERRUPT Interrupt, PVOID ServiceContext);
typedef VOID (NTAPI *PKDEFERRED_ROUTINE)(PKDPC Dpc, PVOID DeferredContext, PVOID SystemArgument1, PVOID SystemArgument2);

enum { KernelMode = 0, UserMode = 1 };
enum { Executive = 0 };
enum { LevelSensitive = 0, Latched = 1 };

#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102L)
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#define PAGE_SIZE 0x1000
#define PAGE_READWRITE 0x04
#define DISPATCH_LEVEL 2
#define IO_NETWORK_INCREMENT 2
#define XC_FACTORY_ETHERNET_ADDR 0x0101

#define XNET_ETHERNET_LINK_ACTIVE 0x01
#define XNET_ETHERNET_LINK_100MBPS 0x02
#define XNET_ETHERNET_LINK_10MBPS 0x04
#define XNET_ETHERNET_LINK_FULL_DUPLEX 0x08
#define XNET_ETHERNET_LINK_HALF_DUPLEX 0x10

void *oxbox_kernel_export(unsigned long ordinal);

#define OXBOX_KERNEL(type, ordinal) ((type)oxbox_kernel_export(ordinal))

#define ExAllocatePoolWithTag(size, tag) OXBOX_KERNEL(PVOID (NTAPI *)(SIZE_T, ULONG), 15)(size, tag)
#define ExFreePool(pointer) OXBOX_KERNEL(VOID (NTAPI *)(PVOID), 17)(pointer)
#define ExQueryNonVolatileSetting(index, type, value, length, result) \
	OXBOX_KERNEL(NTSTATUS (NTAPI *)(ULONG, PULONG, PVOID, ULONG, PULONG), 24)(index, type, value, length, result)
#define HalGetInterruptVector(level, irql) OXBOX_KERNEL(ULONG (NTAPI *)(ULONG, PKIRQL), 44)(level, irql)
#define KeConnectInterrupt(interrupt) OXBOX_KERNEL(BOOLEAN (NTAPI *)(PKINTERRUPT), 98)(interrupt)
#define KeDelayExecutionThread(mode, alertable, interval) \
	OXBOX_KERNEL(NTSTATUS (NTAPI *)(KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER), 99)(mode, alertable, interval)
#define KeDisconnectInterrupt(interrupt) OXBOX_KERNEL(BOOLEAN (NTAPI *)(PKINTERRUPT), 100)(interrupt)
#define KeGetCurrentIrql() OXBOX_KERNEL(KIRQL (NTAPI *)(void), 103)()
#define KeInitializeDpc(dpc, routine, context) \
	OXBOX_KERNEL(VOID (NTAPI *)(PKDPC, PKDEFERRED_ROUTINE, PVOID), 107)(dpc, routine, context)
#define KeInitializeInterrupt(interrupt, routine, context, vector, irql, mode, share) \
	OXBOX_KERNEL(VOID (NTAPI *)(PKINTERRUPT, PKSERVICE_ROUTINE, PVOID, ULONG, KIRQL, int, BOOLEAN), 109) \
		(interrupt, routine, context, vector, irql, mode, share)
#define KeInitializeSemaphore(semaphore, count, limit) \
	OXBOX_KERNEL(VOID (NTAPI *)(PRKSEMAPHORE, LONG, LONG), 112)(semaphore, count, limit)
#define KeInsertQueueDpc(dpc, argument1, argument2) \
	OXBOX_KERNEL(BOOLEAN (NTAPI *)(PRKDPC, PVOID, PVOID), 119)(dpc, argument1, argument2)
#define KeRaiseIrqlToDpcLevel() OXBOX_KERNEL(KIRQL (NTAPI *)(void), 129)()
#define KeReleaseSemaphore(semaphore, increment, adjustment, wait) \
	OXBOX_KERNEL(LONG (NTAPI *)(PRKSEMAPHORE, KPRIORITY, LONG, BOOLEAN), 132)(semaphore, increment, adjustment, wait)
#define KeTickCount (*(volatile ULONG *)oxbox_kernel_export(156))
#define KeWaitForSingleObject(object, reason, mode, alertable, timeout) \
	OXBOX_KERNEL(NTSTATUS (NTAPI *)(PVOID, int, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER), 159) \
		(object, reason, mode, alertable, timeout)
#define KfLowerIrql(irql) OXBOX_KERNEL(VOID (FASTCALL *)(KIRQL), 161)(irql)
#define MmAllocateContiguousMemoryEx(size, lowest, highest, alignment, protect) \
	OXBOX_KERNEL(PVOID (NTAPI *)(SIZE_T, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG), 166)(size, lowest, highest, alignment, protect)
#define MmFreeContiguousMemory(pointer) OXBOX_KERNEL(VOID (NTAPI *)(PVOID), 171)(pointer)
#define MmGetPhysicalAddress(pointer) OXBOX_KERNEL(ULONG_PTR (NTAPI *)(PVOID), 173)(pointer)
#define MmLockUnlockBufferPages(base, size, unlock) \
	OXBOX_KERNEL(VOID (NTAPI *)(PVOID, SIZE_T, BOOLEAN), 175)(base, size, unlock)
#define PhyGetLinkState(update) OXBOX_KERNEL(DWORD (NTAPI *)(BOOLEAN), 252)(update)
#define PhyInitialize(force_reset, parameter) OXBOX_KERNEL(NTSTATUS (NTAPI *)(BOOLEAN, PVOID), 253)(force_reset, parameter)
#define RtlZeroMemory(pointer, size) memset((pointer), 0, (size))

/* XAPI's, linked from Halo's XBE (retail.ld) */
HANDLE NTAPI CreateThread(PVOID attributes, SIZE_T stack_size, PVOID start, PVOID parameter, DWORD flags, DWORD *id);
int NTAPI CloseHandle(HANDLE handle);
DWORD NTAPI GetTickCount(void);
int NTAPI SetThreadPriority(HANDLE thread, int priority);
#define THREAD_PRIORITY_HIGHEST 2
DWORD NTAPI GetLastError(void);
VOID NTAPI SetLastError(DWORD error);
VOID NTAPI Sleep(DWORD milliseconds);

void platform_log(const char *format, ...);

#endif
