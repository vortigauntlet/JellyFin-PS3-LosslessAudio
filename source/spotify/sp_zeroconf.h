// Spotify Connect pairing over mDNS + a small HTTP server (sp_zeroconf.cpp).
#pragma once
#include <functional>

namespace cspot { class LoginBlob; }

// Starts advertising this console as a Spotify Connect device and serving the app's getInfo and addUser requests.  When the
// app sends a login, `blob` is filled in and on_auth is called (on the server thread).  False if there is no network.
bool sp_zc_start(cspot::LoginBlob* blob, const char* device_name, std::function<void()> on_auth);
void sp_zc_stop();
