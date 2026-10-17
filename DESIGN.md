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
