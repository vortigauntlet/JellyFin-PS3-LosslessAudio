// Makes C++ exceptions work (see eh_anchor.S): registers the program's exception frame table with libgcc's unwinder, then proves
// it with a throw and catch.  cspot reports every lost connection by throwing, so without this the first dropped AP connection
// aborts the whole program.  Before throwing, it asks the unwinder whether it can find frame data for a few functions and
// logs the answer, because a throw that cannot unwind aborts instead of failing politely.
#include "sp_eh.h"

#include <stdio.h>
#include <stdexcept>

#include "plog.h"

extern "C" {
extern char sp_eh_anchor[];
void __register_frame_info(const void* begin, void* ob);
struct sp_eh_bases { void *tbase, *dbase, *func; };
const void* _Unwind_Find_FDE(void* pc, struct sp_eh_bases* bases);
void* __cxa_allocate_exception(unsigned long);
}

static void probe(const char* what, void* pc) {
  sp_eh_bases b = {nullptr, nullptr, nullptr};
  const void* fde = _Unwind_Find_FDE(pc, &b);
  char line[120];
  snprintf(line, sizeof line, "spotify: eh probe %s pc=%p fde=%p func=%p", what, pc, fde, b.func);
  plog(line);
}

bool sp_eh_init(void) {
  static char object[128] __attribute__((aligned(16)));      // libgcc's struct object is smaller than this
  char line[120];
  const unsigned* head = (const unsigned*)sp_eh_anchor;
  snprintf(line, sizeof line, "spotify: eh begin=%p first=%08x %08x", sp_eh_anchor, head[0], head[1]);
  plog(line);
  // The label sits on the zero-length terminator the C start-up files leave in front of the program's own entries, and the unwinder
  // reads a table that starts with a zero as empty: begin at the first real entry (a CIE, whose length is not zero).
  const unsigned* first = head;
  for (int i = 0; i < 8 && *first == 0; i++) first++;
  __register_frame_info(first, object);
  probe("sp_eh_init", (void*)&sp_eh_init);
  probe("__cxa_allocate_exception", (void*)&__cxa_allocate_exception);
  try {
    throw std::runtime_error("eh");
  } catch (const std::runtime_error&) {
    plog("spotify: C++ exceptions work");
    return true;
  }
  return false;
}
