#ifndef _TEAMPHYS_H
#define _TEAMPHYS_H

// 2026: per-team physics, read from the SeasonOverrides override file. Two knobs (no power/skill --
// GP2Edit owns those; no AI-grip/brakes -- too asymmetric). Keys are per-TEAM (1..14):
//   massNN                = <kg>   chassis weight; omit = the EXE's d_carstdweight (stock or edited)
//   downforceMultiplierNN = <pct>  100 = stock, clamped 1..200
// See docs/gp2lap/runtime-physics-loading.md.

extern unsigned long PerTeamPhysics;        // 0 = nothing patched

void TeamPhysInit(void);                     // parse override file + install the two stubs (late init)

#endif
