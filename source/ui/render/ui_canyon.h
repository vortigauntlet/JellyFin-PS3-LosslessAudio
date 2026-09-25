#pragma once
// The Canyon music visualizer on the RSX -- the drawing side of canyon.h.
//
// The music screen draws it IN PLACE OF wave_draw() when the visualizer mode
// is Canyon (L2 / R2 on the music screen cycle Wave <-> Canyon, persisted in
// jellyfin_visualizer.txt; Square picks the next Canyon preset).
//
// Presets are read at runtime from the console's own
// /dev_flash/vsh/resource/qgl/canyon.qrc; without it the built-in look is used.
// The two vertex buffers (~0.8 MB each, RSX local memory) are allocated on
// first use and kept.

#include <ppu-types.h>

enum { VIZ_WAVE = 0, VIZ_CANYON = 1, VIZ_COUNT = 2 };

int  viz_mode(void);                 // loads jellyfin_visualizer.txt on first call
void viz_set_mode(int mode);         // set + persist
const char *viz_mode_name(int mode);

// Canyon.  canyon_draw() returns false if the visualizer could not start
// (allocation failed); the caller then draws the wave instead.
bool canyon_draw(float bright, bool paused);
void canyon_track(int track_index);  // a new song: cross-fade to its preset
void canyon_next_preset(void);       // Square: cross-fade to the next preset
const char *canyon_preset_name(void);
