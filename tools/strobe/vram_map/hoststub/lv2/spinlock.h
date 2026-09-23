/* Host stand-in: the replay is single-threaded, the lock is a no-op. */
#ifndef HOSTSTUB_LV2_SPINLOCK_H
#define HOSTSTUB_LV2_SPINLOCK_H
#include <ppu-types.h>
static inline void sysSpinlockInitialize(s32 *l) { *l = 0; }
static inline void sysSpinlockLock(s32 *l) { (void)l; }
static inline void sysSpinlockUnlock(s32 *l) { (void)l; }
#endif
