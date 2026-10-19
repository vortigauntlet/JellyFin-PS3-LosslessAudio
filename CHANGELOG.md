# Changelog

## 3.2.3

### Added
- **Search types on the PS3 system keyboard** instead of the drawn one. Select the search field with X (or Down) to open it; confirming runs the search at once and highlights the first result.

### Fixed
- **Stuck on "Finding versions..." for a film opened from Search.** The first
  time a debrid title is opened from Search, the server adds it to your library
  under a new id, and the old one stops working. The page kept asking about the
  old one, so the versions never arrived until you opened the film again from
  the Movies tab. The page now switches to the new id at once, and opening the
  same search result again goes straight to it.
- **Films playing in the wrong language.** Playback started on whichever
  soundtrack the file marked as its default, which on some releases is a dub
  (Avatar's Russian remuxes start on Russian). It now starts on a soundtrack in
  your PS3's system language when the film has one, skipping commentaries, and
  only otherwise uses the file's default. Up to 16 soundtracks per version are
  listed now, up from 8, so an English track further down the list is no
  longer cut off.
- **Home's posters glide at 3.1's speed again**, and the details under a
  poster fade in as it arrives instead of waiting for it to stop.
- **The wave no longer stutters behind the version picker or the music
  player.** The version list is drawn by the graphics chip now, and every list
  remembers how it shortened a long title instead of working it out again every
  frame.
- **Episode titles use the wider type 3.1 had**, so most fit under the still.
- **The player bar goes away when paused too**, after 8 seconds without a
  button press. Skip Intro stays up for 10 seconds and Next Episode for 20.
- **Titles with no streams say so** instead of offering a Play that ends in an HTTP 500: the Version row reads "No streams found" and Play shows a message.
- **Playback that dies before its first picture explains why** instead of a black screen and a silent return to the media page.
- **Posters are sharper** (higher JPEG quality).

## 3.2.2

### Fixed
- **Menus no longer stutter while you flick between tabs.** The cache of
  drawn text was being thrown away completely every few seconds, and each time
  every label on screen had to be drawn again from scratch. Now only the oldest
  text is dropped and what is on screen stays ready.
- **Fits TVs that crop the picture's edges.** Most TVs outside a "PC" or
  "Just Scan" mode cut off the edges of the picture, which hid the logo, the
  clock and the button hints. The app now keeps a small margin by default, like
  the PS3's own menus. Settings → Display → Screen Size still adjusts it (or
  turns it off).
- **"Back to Season" froze the screen for seconds.** Leaving an episode's
  details page waited up to 2 seconds per request behind the page's background
  stream lookup before even asking the server. It no longer waits.
- **Music player inside that margin too.** The album art and the Up Next list
  no longer run off the edges on those TVs.
- **The 24Hz/50Hz output prompt** is now a properly sized panel in the middle of
  the screen, instead of small text in the corner where a TV could cut it off.
- **Version picker missing on a film opened from Search.** The first time a
  debrid title is opened, the server is still finding its versions; the page
  now says "Finding versions..." and fills them in when they arrive, instead
  of showing only Quality until you back out and in again.
- **Freeze on "Preparing" when starting a film.** Starting playback freed the
  poster memory while a poster that was still downloading could be written into
  it, right where the video decoder was being set up. On a slow connection, or
  whenever posters were slow to arrive, that could lock up the whole console.
  The poster loader now stands down cleanly before the player starts.
- **Sluggish menus when posters are slow.** The menu wrote a debug line to the
  hard drive for every poster it asked for, on the drawing thread, and the
  poster lock could leave the menu waiting on the lowest-priority thread. Both
  are gone: the log is written in the background and the locks hand priority
  to whoever holds them.
- **"Update available" on the latest version.** 3.2 and 3.2.1 still reported
  themselves as 3.2 to the update check, so 3.2.1 announced itself as an
  update on every launch.
- **ffwrap is now a one-click install** for Windows servers that stall on
  debrid streams: `ffwrap-for-jellyfin.zip` on the release has
  `install-ffwrap.bat` and `uninstall-ffwrap.bat`, which say SUCCESS or FAILED
  when they finish. It now downloads at 1.75× the film's bitrate instead of
  1.25×, so the PS3 builds a buffer and stops micro-stuttering.
- The log now says why a poster failed to load (no connection, the server's
  answer, or an image too large), not just that it did.

### Changed
- **Smoother background.** The gradient no longer shows bands on high-end
  TVs.
- **Longer titles under posters.** Titles use a condensed face, so about
  about 10 characters fit under a poster instead of 8, at a larger size.
- **The Next Episode prompt comes up later** (in the last 75 seconds rather
  than 90, or when the credits start), so it no longer gives away that a scene
  is the episode's last. Skip Intro now goes away after 5 seconds, and both
  prompts are translucent.
- **Posters lose the white outline.** The selected poster is marked by a soft
  purple glow that fades outward instead. Buttons keep their outline.

## 3.2.1

### Fixed
- **4:3 TVs: overlapping text on the details screen.** The Resume, Mark as
  watched and Favourite buttons and the Quality / Offline selectors now size to
  their text instead of printing over each other.

### New
- **Picture size hint in the player.** The hints bar now shows `L1` for
  picture size on films and episodes.

## 3.2

The biggest update yet: **Live TV, downloads, playing straight from USB drives,
six languages, Quick Connect sign-in** and a new Settings screen, plus a long
list of fixes.

### New
- **Quick Connect sign-in.** No more typing passwords with a controller: the
  console shows a code, you approve it in the Jellyfin app on your phone
  (Settings → Quick Connect), and you're in. Username and password still work.
- **Live TV.** If your server has Live TV (a tuner or an IPTV playlist), a Live
  TV tab lists your channels with what's on now and a TV guide. `↑`/`↓` change
  channel while watching. *My Channels* and *Recently watched* pages come
  first, and `Start` lets you hide the categories you don't use.
- **Downloads.** Save a film, an episode or a whole season (`△` on a season) to
  the PS3's hard drive or an **NTFS USB drive**, and watch it with no server.
  Downloads pause while you stream, resume after a restart, and always leave at
  least 10 GB free on the hard drive.
- **Play from USB drives, no server needed.** The Media tab plays MKV, .m2ts
  and .ts films from NTFS, exFAT or FAT32 drives, with lossless TrueHD /
  DTS-HD MA audio and subtitles, plus **FLAC, MP3 and WAV music** as albums.
- **Six languages:** English, 日本語, Português (Brasil), Deutsch, Français and
  Español. It follows your PS3's language, or pick one in Settings → Display.
- **A new Settings screen** laid out like the PS3's own: six categories
  (Playback, Audio, Subtitles, Downloads, Display, System), each with a short
  description of every setting.
- **Fit, Fill or Stretch** (`Select` in the player): Fill zooms a 16:9 film to
  fill a 4:3 TV.
- **Chapters** (`R1` in the player), for films whose server has chapter data.
- **Picture size** (`L1` in the player), for CRTs that cut off the edges.
- **Favourites**, with their own row on Home.
- **Dolby Digital output** (Settings → Audio → Audio Output), for soundbars on
  a TV's ARC port that lose the centre channel in 5.1.
- **Wake the server** with Wake-on-LAN (Settings → System). The console also
  wakes it by itself when it can't reach it.
- **The app finds your server again** if its address changes (for example the
  PC moving from Wi-Fi to Ethernet).
- **Send Log to Server** (Settings → System) for bug reports, no FTP needed.
- **HTTPS server addresses** for sign-in and browsing (experimental).
- **Pinned to the top of the Game column** in the XMB, above your games, and
  the XMB now shows the real version (03.20).
- Smaller touches: `←`/`→` skip 10 s with the player bar hidden, and clicking
  a stick shows the player stats.

### Fixes
- **Films start straight after the 24 Hz switch** again, with no second
  buffering screen in between.
- **The version list showed the same version over and over** on episodes a
  debrid plugin had listed many times. Repeats are now shown once.
- **Random freezes and missing artwork** when opening pages quickly: two
  internal locks were never actually created, so parts of the app could read
  each other's network replies. Fixed, along with **Software Update**, which
  could not run in the previous build.
- Resuming a film from its page started it over instead of where you stopped.
- Pressing the PS button while paused could make the player look frozen.
- Some pages opened empty and hung on retry when the server's reply was framed
  unusually.
- The menu froze for about five seconds at a time while the server was
  unreachable, and startup waited on a server that wasn't there.
- Small text on standard-definition (480/576) TVs is now readable.
- Oversized cover images and failed video-memory allocations no longer crash
  the app; a failed seek could close the stream twice; JellyDrop jumped once an
  hour.

### Buffering on a wired server PC? Check your router
If your Jellyfin PC is plugged into the router with a cable and you stream
through a **debrid / Stremio-style plugin** (Gelato and similar), the server can
download a film from the internet as fast as your connection allows when it
starts. Some routers can't keep up with that and stop passing traffic between
your PC and the PS3 for a few seconds at a time, so the film buffers and stalls
over and over even though everything is on a fast network. Two ways out:

- **A cheap gigabit switch** between the router, the PC and the PS3, so their
  traffic never goes through the router. This also protects other streaming,
  such as Moonlight.
- **Make the server download at film speed.** This release includes
  `ffwrap`, a small stand-in for Jellyfin's ffmpeg that limits how fast it
  reads from the internet (1.75× the film's own bitrate since 3.2.2). See
  [`tools/ffwrap/README.txt`](tools/ffwrap/README.txt).

The app also notices when a stream arrives in bursts with long gaps and
reconnects by itself.

## 3.1

**The new interface is now what everyone gets.** In 3.0 (and the beta before it),
the layered interface, JellyWave, and the GPU-drawn posters and text were all
switched off unless a setting file existed on the console, and nothing created
those files. A fresh install showed the old tab strip, a flat three-band wave,
and slower CPU-drawn menus. They are all on by default now. If you installed 3.0,
installing 3.1 over it is all you need to do.

### New
- **Settings → Software Update.** Shows the result of the launch check (up to
  date, a newer version available, or couldn't check). `X` checks again, or
  reopens the update popup when there is a new version.
- The update popup now says where to get the new `.pkg`.

### Controls
- **`O` stops playback**, the same as `Start`, whether or not the player bar is
  showing. If a track menu or the volume slider is open, `O` closes that first.
- **`□` hides the player bar** without waiting for it to time out. The player
  stats overlay is still on the remote's `DISPLAY` key and in Settings.
- **Remotes without `L1`/`R1`:** the Blu-ray remote's `|<<` `>>|` and `<<` `>>`
  keys now switch tabs in the menus, so a Bluetooth remote can reach every tab.
  Unrecognised remote keys are written to the debug log so they can be mapped.

### Fixes
- **Home posters on the selected row.** The poster cache has a fixed number of
  slots, and when Home wanted more than fit, the row you were looking at could
  lose out: some of its posters stayed blank until you moved off it. The
  selected row now loads first.
- JellyWave moves at the speed it was tuned to on a TV (a little calmer than 3.0's
  default).

### Turning things back off
Each of these still has a file in `/dev_hdd0/tmp/`; put `0` in it to switch that
feature off: `jellyfin_spine.txt` (layered interface), `jellyfin_gpuwave.txt`
(JellyWave), `jellyfin_gpucards.txt` and `jellyfin_gputext.txt` (GPU posters and
text). `jellyfin_jwspeed.txt` sets the JellyWave speed (default 14).
