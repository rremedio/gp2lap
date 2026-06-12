#ifndef _CARSHAPE_H
#define _CARSHAPE_H

// 2026: per-team car-shape overrides, read from override.cfg (pointed to by SeasonOverrides).
//   Tier 1 (live):    per-team nose choice -- "noseNN = 0|1" (NN = team 1..14).
//   Tier 2 (planned): per-team .dat shapes -- "shapeNN = path".
// See docs/per-team-car-shapes.md.

extern unsigned long PerTeamNose;        // 0 = off (set on once nose overrides are applied)

void CarShapeInit(void);                 // parse nose keys + write dword_CB394 (call once, late init)

#endif
