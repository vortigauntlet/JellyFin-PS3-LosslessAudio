# mohasi_fs: read-only NTFS and exFAT readers

Vendored from **[mohasi/ps3-dev](https://github.com/mohasi/ps3-dev)**,
`libs/simple-lib-core/` (`src/ntfs.c`, `src/exfat.c`, `include/ntfs.h`,
`include/exfat.h`), commit recorded in `VERSION`. Licence **Apache-2.0** (the
`LICENSE` file here is upstream's). This project is GPL-3.0; the Apache-2.0
files keep their own licence and are included under its terms.

Hand-written readers that parse the volume through the lv2 raw storage
syscalls: partition table (MBR and GPT), NTFS (MFT, runlists, `$I30` index,
`$ATTRIBUTE_LIST`, sparse and LZNT1 data) and exFAT (entry sets, FAT chains,
up-case table). Used by `source/local/lfs.cpp` to play files from USB drives.

## What was changed

Every edit is marked `jf-port` in the source. Nothing else was touched.

* **No write path.** Every function that allocates clusters, writes MFT
  records, bitmaps or directory entries, or calls the storage write syscall
  (603) was deleted, not disabled: `writeNtfs`, `truncateNtfs`, the
  create/mkdir/unlink/rmdir/rename entry points and everything only they
  called (about 3,400 lines of `ntfs.c`, 1,700 of `exfat.c`), the `$LogFile`
  clean check that only gated writes, and the dirty-flag handling. A volume is
  always mounted read only. `make -f tests/Makefile.host check` fails if the
  write syscall, `scCall` or `lv2syscall` appears in `ntfs.c` or `exfat.c`.
* **No VFS half.** Both files ended with a VFS backend (volume and handle pools,
  a mutex, registration). It is gone; `source/local/lfs.cpp` owns volumes,
  handles and the one lock these readers need (they share static scratch
  buffers, so every call must be serialised).
* **Platform layer.** `jf_port.h` replaces `storage-device.h`, `syscall.h`,
  `thread.h`, `string-utilities.h`, `<sys/timer.h>` and `<cell/rtc.h>`: four
  storage functions (implemented over lv2 syscalls 600, 601, 602 and 609 in
  `jf_port_ps3.c`, and over an image file in the host tests), a sleep, and the
  string helpers copied unchanged from upstream. exFAT's scratch block is an
  ordinary `memalign` allocation instead of `sysMemAllocate`.
* **One bug fix** in `ntfs.c`: a sparse-only attribute (flag `0x8000`, which
  Windows and ntfs-3g give a compression unit of 16 clusters) was decoded as
  LZNT1, so any sparse file with a written extent failed to read. The unit now
  applies only to attributes with the compressed flag.

## Tests

`tests/test_mohasi_fs.c` mounts images made with the Linux drivers
(`tests/fixtures/fs/make_images.sh`) bare, in an MBR partition and in a GPT
partition, lists directories (including 400 entries, Unicode and a name outside
the BMP), compares the content of every file with its generator and reads a
5 GB sparse file past 4 GiB. It also cuts the drive mid-read. It runs on the host
and big-endian under `qemu-ppc64`.
