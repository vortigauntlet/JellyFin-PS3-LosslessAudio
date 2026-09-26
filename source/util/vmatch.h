#pragma once
// Does a version's name look like THIS film?  (2026-09-27)
//
// Debrid indexers file wrong releases under a title: The Avengers (2012) on
// this server lists "Avengers Assemble.2012.Complete.1080p.WEBRip" -- the
// whole cartoon series -- as its version 2, and the version fallback played
// it.  Scores the release filename (the text after the folder emoji in
// Gelato/MediaFusion names, else the whole label) against the title:
//
//   +3  the title's words, in order, directly followed by a year
//   +1  every significant title word appears somewhere
//   -4  a series pack: "complete", "season", "S01", "episode", "collection"
//   -2  3D (half-OU / SBS: the PS3 shows it squashed)
//   -1  a '|' in the name (Gelato's HTTP 500 bug)
//
// >= 3 is a confident match; < 0 is probably not the film.
int version_match_score(const char *label, const char *title);
