ffwrap for Jellyfin (Windows)
=============================

WHO NEEDS THIS
--------------
Only you if ALL of these are true:
  - your Jellyfin server runs on a Windows PC that is wired to your router,
  - you stream through a debrid / Stremio-style plugin (Gelato and similar),
  - films on the PS3 buffer, stutter or stall, especially in their first
    minutes, or the PS3 loses the server while a film is starting.

If films play smoothly, you do not need it.

WHAT IT DOES
------------
When a film starts, Jellyfin's ffmpeg downloads it from the internet as fast
as your connection allows. Many home routers cannot do that and still pass
traffic between the PC and the PS3, so the PS3's stream stalls.

ffwrap sits in front of Jellyfin's ffmpeg and makes it download at 1.75 times
the film's own bitrate instead (after a 4-second head start). That is far
enough ahead of playback for the PS3 to keep a buffer, without flooding the
router. Nothing else about Jellyfin changes.

(A gigabit network switch between the router, the PC and the PS3 fixes the
same problem in hardware.)

INSTALL
-------
1. Unzip everything into one folder (keep the files together).
2. Double-click  install-ffwrap.bat  and click Yes when Windows asks for
   administrator rights.
3. If Jellyfin is running from its tray icon, the window asks you to close it:
   right-click the Jellyfin icon by the clock and choose Exit. The installer
   carries on by itself and starts Jellyfin again at the end.
4. The window ends with SUCCESS or FAILED and the reason. Press a key to close.

Nothing to download: it is built with the C# compiler that comes with Windows.
It is installed to  C:\ProgramData\Jellyfin\ffwrap  and Jellyfin's setting
"FFmpeg path" (Dashboard > Playback > Transcoding) is pointed at it. Your old
setting is saved as  encoding.xml.before-ffwrap  next to Jellyfin's settings.

CHANGE THE SPEED
----------------
Edit  C:\ProgramData\Jellyfin\ffwrap\ffwrap.ini  and change the readrate line,
e.g.  readrate=1.5  (any value from 1.1 to 4). Higher builds a bigger buffer
but pulls harder on your internet connection. Takes effect on the next film.

UNINSTALL
---------
Double-click  uninstall-ffwrap.bat  (Yes to the administrator prompt). Jellyfin
goes back to its own ffmpeg and the ffwrap folder is removed.

AFTER A JELLYFIN UPDATE
-----------------------
An update can reset Jellyfin's "FFmpeg path". If films start stalling again
after updating Jellyfin, run  install-ffwrap.bat  again.

FILES
-----
install-ffwrap.bat     installs (asks for administrator rights)
uninstall-ffwrap.bat   removes it again
ffwrap-setup.ps1       what both of them run
ffwrap.cs              the source of the wrapper, built during install
README.txt             this file
