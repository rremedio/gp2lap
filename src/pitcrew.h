#ifndef _PITCREW_H
#define _PITCREW_H

// 2026: per-team pit-crew colours, read from the SeasonOverrides override file.
// Each team supplies 14 palette indices (pitcrew[0..13]) written into t_PitCrewColors
// idx2..idx15 (idx0/idx1 are the fixed shadow/highlight ramp bases 0x00/0x10). The engine
// re-expands t_PitCrewColors into its runtime LUT via MakeCrewColors at each session setup,
// so a one-time startup write persists. See docs/driver-data.md.

void PitCrewColorsInit(void);               // write per-team pit-crew colours (late init)

#endif
