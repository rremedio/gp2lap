#ifndef _TEAMPHYS_H
#define _TEAMPHYS_H

// 2026: per-team physics from the SeasonOverrides model. Season keys per [Team N] (1..14):
//   mass=<kg>, downforce=<pct 1..200>, power/qualpower=<PS 0..1579>, reliability=<0..32767>.
// mass/downforce ride read-site stubs (0 table entry = stock); power/reliability are written
// into the exe perf tables. 1b adds PER-TRACK layering: each [Track N] can name an Override
// file whose [Team N] physics values override the season for that track, applied at SOS and
// reverted (to season, else the stock snapshot) when the track changes.
// See docs/gp2lap/runtime-physics-loading.md and the season-realism roadmap (in the vault).

extern unsigned long PerTeamPhysics;        // 0 = nothing patched

void TeamPhysInit(void);                     // season layer + snapshot + install the stubs (late init)
void PerTrackPhysSOS(void);                  // SOS: merge the current track's override file, (re)apply

#endif
