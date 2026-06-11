#ifndef _CARTEX_H
#define _CARTEX_H

// 2026: per-car car-texture override. Public surface used by AHFAfterGp2Init + the asm hook.

extern unsigned long PerCarTextures;     // 0 = off (set on once init succeeds)
extern unsigned long PerCarCockpit;      // 0 = off (set on once cockpit init succeeds)

void CarTexInit(void);                   // parse cfg + load BMPs + bootstrap pointers (call once, late init)
void __near _cdecl AHFCarTexSwap(void);  // per-draw hook body (called from Hook_CarTex in lammcall.asm)

void CarCockpitInit(void);               // bootstrap ptrs + inline-patch rUpdCarsCockpit (call after CarTexInit)
void __near _cdecl AHFCockpitColors(void); // read-site body (called from MyCockpitColors in lammcall.asm)

#endif
