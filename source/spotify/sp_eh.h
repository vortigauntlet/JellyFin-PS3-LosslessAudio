// C++ exception support for the Spotify app (sp_eh.cpp, eh_anchor.S).  Call once at start-up, before anything can throw.
#pragma once
bool sp_eh_init(void);
