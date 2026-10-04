// jf-port: the console side of jf_port.h.  The four storage calls are the lv2 syscalls the
// original reached through its scCall trampolines (600 open, 601 close, 602 read,
// 609 get_device_info); the return values are passed through unchanged.  There is no
// write call: syscall 603 is not used anywhere in this directory.
//
// They need the same firmware support (HEN/CFW) the app already requires.

#include "jf_port.h"

#include <ppu-lv2.h>
#include <unistd.h>

#define STORAGE_OPEN      600
#define STORAGE_CLOSE     601
#define STORAGE_READ      602
#define STORAGE_GET_INFO  609

int getStorageInfo(uint64_t deviceId, StorageDeviceInfo *info)
{
   return (int)lv2syscall2(STORAGE_GET_INFO, deviceId, (uint64_t)(uintptr_t)info);
}

int openStorage(uint64_t deviceId, int *outStorageHandle)
{
   return (int)lv2syscall4(STORAGE_OPEN, deviceId, 0, (uint64_t)(uintptr_t)outStorageHandle, 0);
}

int closeStorage(int storageHandle)
{
   return (int)lv2syscall1(STORAGE_CLOSE, (uint64_t)storageHandle);
}

int readStorageRaw(int storageHandle, uint64_t sector, uint32_t count, void *buffer, uint32_t *outRead)
{
   return (int)lv2syscall7(STORAGE_READ, (uint64_t)storageHandle, 0, sector, count,
                           (uint64_t)(uintptr_t)buffer, (uint64_t)(uintptr_t)outRead, 0);
}

void jf_port_usleep(unsigned usec) { usleep(usec); }
