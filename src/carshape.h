#ifndef _CARSHAPE_H
#define _CARSHAPE_H

// 2026: per-team car-shape overrides, read from override.cfg (pointed to by SeasonOverrides).
//   Tier 1: per-team nose choice  -- "noseNN = 0|1"  (NN = team 1..14).
//   Tier 2: per-team .dat shapes  -- "shapeNN = path" (NN = team 1..14).
// See docs/per-team-car-shapes.md.

extern unsigned long PerTeamNose;        // 0 = off (set on once nose overrides are applied)
extern unsigned long PerTeamShape;       // 0 = off (set on once .dat shapes are loaded)
extern unsigned char *CarShapeCarPtr;    // car ptr (ESI), stashed by the asm hook each draw

void CarShapeInit(void);                 // parse cfg, write CB394, load .dats, snapshot (call once, late init)
void __near _cdecl AHFCarShapeSwap(void);// per-draw hook body (called from Hook_CarShape in lammcall.asm)

#endif
