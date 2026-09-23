/* Host stand-in for PSL1GHT's ppu-types.h -- just enough for librt/heap.c.
 * uintptr_t is 8 bytes on the PPU (LP64) and on x86-64, so the heap's block
 * headers (2 x uintptr_t) and its arithmetic are the same on both. */
#ifndef HOSTSTUB_PPU_TYPES_H
#define HOSTSTUB_PPU_TYPES_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint8_t u8;  typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t  s8;  typedef int16_t  s16; typedef int32_t  s32; typedef int64_t  s64;
#endif
