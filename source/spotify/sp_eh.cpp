// Makes C++ exceptions work on this toolchain, then proves it with a throw and catch.  cspot reports every lost connection by
// throwing, so without this the first dropped connection aborts the whole program.  Two things were missing:
//
//   * The unwinder finds frame tables only through __register_frame_info, which the PS3 start-up never calls.  The label
//     sp_eh_anchor (eh_anchor.S) sits on the zero-length terminator the C start-up files leave in front of the program's own
//     entries; the unwinder reads a table that starts with a zero as empty, so registration begins at the first real entry.
//   * ld splits the program into several TOCs (the register r2 that addresses global data) and the unwinder does not restore
//     r2 when it enters a handler, so a handler ran with another group's value.  The Spotify app is therefore compiled with
//     -mminimal-toc and linked with --no-multi-toc (Makefile): one TOC, nothing to restore.
#include "sp_eh.h"

#include <stdexcept>

#include "plog.h"

extern "C" {
extern char sp_eh_anchor[];
void __register_frame_info(const void* begin, void* ob);
}

bool sp_eh_init(void) {
  static char object[128] __attribute__((aligned(16)));      // libgcc's struct object is smaller than this
  const unsigned* first = (const unsigned*)sp_eh_anchor;
  for (int i = 0; i < 8 && *first == 0; i++) first++;
  __register_frame_info(first, object);
  try {
    throw std::runtime_error("eh");
  } catch (const std::runtime_error&) {
    plog("spotify: C++ exceptions work");
    return true;
  }
  return false;
}
