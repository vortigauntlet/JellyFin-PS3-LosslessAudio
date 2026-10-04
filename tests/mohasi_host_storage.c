// Host stand-in for the lv2 storage syscalls the vendored NTFS/exFAT readers call
// (third_party/mohasi_fs/jf_port.h): each USB port's "device" is an image file.
//
// There is no write: the readers have no write path, and this file has no write call to
// give them.  `g_port_fail[p]` makes every read fail the way a pulled drive does.

#define _FILE_OFFSET_BITS 64
#include "jf_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int g_port_fd[8]   = { -1, -1, -1, -1, -1, -1, -1, -1 };   // the image on each port, opened read-only
int g_port_fail[8] = { 0 };                                // 1 = every read fails (device removed)
unsigned long g_port_reads[8] = { 0 };                     // sector reads served per port

void jf_port_usleep(unsigned usec) { (void)usec; }   // nothing to wait for on the host

static int port_of(uint64_t id)
{
   if (id >= 0x10300000000000AULL && id <= 0x10300000000000AULL + 5) return (int)(id - 0x10300000000000AULL);
   if (id >= 0x10300000000001FULL && id <= 0x10300000000001FULL + 1) return 6 + (int)(id - 0x10300000000001FULL);
   return -1;
}

int getStorageInfo(uint64_t deviceId, StorageDeviceInfo *info)
{
   const int p = port_of(deviceId);
   if (p < 0 || g_port_fd[p] < 0) return -1;
   struct stat st;
   if (fstat(g_port_fd[p], &st) != 0) return -1;
   memset(info, 0, sizeof *info);
   snprintf(info->label, sizeof info->label, "TEST");
   info->sectorSize  = 512;
   info->sectorCount = (uint64_t)st.st_size / 512;
   return 0;
}

int openStorage(uint64_t deviceId, int *outStorageHandle)
{
   const int p = port_of(deviceId);
   if (p < 0 || g_port_fd[p] < 0) return -1;
   *outStorageHandle = p + 1;
   return 0;
}

int closeStorage(int storageHandle) { (void)storageHandle; return 0; }

int readStorageRaw(int storageHandle, uint64_t sector, uint32_t count, void *buffer, uint32_t *outRead)
{
   const int p = storageHandle - 1;
   *outRead = 0;
   if (p < 0 || p > 7 || g_port_fail[p] || g_port_fd[p] < 0) return (int)0x8001002f;   // ENXIO: not "busy", so no retry
   if (((uintptr_t)buffer & 31) != 0) return (int)0x80010009;  // lv2 wants 32-byte alignment
   ssize_t want = (ssize_t)count * 512;
   ssize_t got = pread(g_port_fd[p], buffer, (size_t)want, (off_t)(sector * 512ULL));
   if (got != want) return (int)0x8001002f;
   g_port_reads[p]++;
   *outRead = count;
   return 0;
}
