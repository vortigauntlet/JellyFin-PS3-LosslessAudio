// Host stand-in for the lv2 storage syscalls the vendored NTFS/exFAT readers call
// (third_party/mohasi_fs/jf_port.h): the "device" is an image file.
//
// There is no write: the readers have no write path, and this file has no write call to
// give them.  `g_dev_fail` makes every read fail the way a pulled drive does.

#define _FILE_OFFSET_BITS 64
#include "jf_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int g_dev_fd = -1;                 // the image, opened read-only by the test
int g_dev_fail = 0;                // 1 = every read fails (device removed)
unsigned long g_dev_reads = 0;     // sector reads served, to bound a scan

void jf_port_usleep(unsigned usec) { (void)usec; }   // nothing to wait for on the host

int getStorageInfo(uint64_t deviceId, StorageDeviceInfo *info)
{
   (void)deviceId;
   if (g_dev_fd < 0) return -1;
   struct stat st;
   if (fstat(g_dev_fd, &st) != 0) return -1;
   memset(info, 0, sizeof *info);
   snprintf(info->label, sizeof info->label, "TEST");
   info->sectorSize  = 512;
   info->sectorCount = (uint64_t)st.st_size / 512;
   return 0;
}

int openStorage(uint64_t deviceId, int *outStorageHandle)
{
   (void)deviceId;
   if (g_dev_fd < 0) return -1;
   *outStorageHandle = 1;
   return 0;
}

int closeStorage(int storageHandle) { (void)storageHandle; return 0; }

int readStorageRaw(int storageHandle, uint64_t sector, uint32_t count, void *buffer, uint32_t *outRead)
{
   (void)storageHandle;
   *outRead = 0;
   if (g_dev_fail || g_dev_fd < 0) return (int)0x8001002f;   // ENXIO: not "busy", so no retry
   if (((uintptr_t)buffer & 31) != 0) return (int)0x80010009;  // lv2 wants 32-byte alignment
   ssize_t want = (ssize_t)count * 512;
   ssize_t got = pread(g_dev_fd, buffer, (size_t)want, (off_t)(sector * 512ULL));
   if (got != want) return (int)0x8001002f;
   g_dev_reads++;
   *outRead = count;
   return 0;
}
