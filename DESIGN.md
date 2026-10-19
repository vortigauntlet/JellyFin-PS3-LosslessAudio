# SoundCloud standalone UI design

This document applies only to APP_VARIANT == 1 in this repository. The Jellyfin UI and its palette remain unchanged.

## Product and information architecture

The standalone app is a controller-first public music client. Its root categories are Search, Stations, Library, and Settings. Stations use the last played track as the related-track seed, or the first public Like from the selected profile on first use. Library contains public Likes, Playlists, Reposts, and Tracks. Private feeds, account sign-in, and unverified Charts are out of scope.

Keep service data honest. Do not invent followers, play counts, charts, or account state when the API does not provide them. The profile field accepts a public SoundCloud profile name or link.

## Visual direction

Use SoundCloud orange (#FF5500) as the primary accent. The default theme is dark espresso with warm orange and apricot waveform colors; the alternate built-in theme is light. Keep the brand mark compact and balanced, using an equalizer-like silhouette with a broad center and no long tapered tail.

The existing XMB chrome provides the console frame. The SoundCloud screen adds a compact horizontal category rail above the list, with orange selection and high-contrast labels. Keep content inside the overscan-safe margins and preserve readable row spacing at 480p, 720p, and 1080p.

## Type

Use the repository's bundled Rodin face for body copy and the bundled Roboto Condensed Bold face for short category labels and the SoundCloud lockup. These are available to the PS3 renderer and avoid downloading proprietary web fonts. Keep titles legible at TV distance; reserve condensed type for brief labels.

## Interaction

- Left/right changes root categories; up/down changes the selected row.
- Cross opens the selected item or changes a setting; Circle returns from lists.
- L1/R1 continues to switch XMB tabs.
- Keep the global Square visualizer shortcut visible in the hint bar.

## Playback and monetization

Use the existing public-track resolver and stream pipeline. Do not add ad skipping, ad blocking, or a claim that SoundCloud's free-tier ad behavior is reproduced. This client does not implement a SoundCloud sign-in or private-content flow.


## Spotify standalone UI (APP_VARIANT == 2)

This section applies only to the Spotify Connect receiver. Keep SoundCloud and Jellyfin surfaces unchanged. Use the 2015 PS3 Spotify app as the reference: the compact green Spotify disc, horizontal Browse / Your Music tabs, a flat black canvas (#000) with #181818 for the player bar and #282828 for selection (near-black #121212 read as grey on a TV), square album covers without a frame, white active labels, subdued gray metadata, and Spotify green (#1ED760) for the active tab and progress. Use the bundled console fonts and UIS scaling so the layout remains legible at 480p, 720p, and 1080p.

Your Music lists the account's Liked Songs, listed from Spotify's collection service (the Web API answers this app's token with 429) and named through cspot's metadata channel as the selection moves; Cross loads the highlighted song and up to 199 after it into cspot directly, as a phone's Load would. Browse lists the queue the Spotify Connect client sent to this receiver; rows whose metadata cspot has not loaded use an honest queued-song label. Catalog search is still done in the phone app. Use a fixed #1ED760 Spotify mark and active states; keep covers frameless. The bottom player is an opaque #181818 bar with a #282828 top rule; with nothing playing here it shows the receiver's status line. L1/R1 skip tracks; left/right switches tabs; Cross plays the highlighted row (or opens Now Playing when there is none); Start opens Now Playing; Circle returns. Finish and flip the last frame before returning to the XMB.
