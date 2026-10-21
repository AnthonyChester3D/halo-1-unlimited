/*
OXBOX_SDK_EXTRA.H

Win32 declarations our Xbox layer uses that the game does not (so OpenCE's XDK stand-in headers leave them out):
the XAPI heap (LocalAlloc and friends) and file sharing. Values are the public Win32 ones.
*/

#ifndef __OXBOX_SDK_EXTRA_H
#define __OXBOX_SDK_EXTRA_H

#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define LMEM_FIXED 0x0000
#define LMEM_MOVEABLE 0x0002
#define LMEM_ZEROINIT 0x0040

typedef union _LARGE_INTEGER *PLARGE_INTEGER;

HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T size);
HLOCAL WINAPI LocalReAlloc(HLOCAL memory, SIZE_T size, UINT flags);
SIZE_T WINAPI LocalSize(HLOCAL memory);
HLOCAL WINAPI LocalFree(HLOCAL memory);

#endif
