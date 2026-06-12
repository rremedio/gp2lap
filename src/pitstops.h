#ifndef _PITSTOPS_H
#define _PITSTOPS_H

// 2026: configurable pit-stop timing, read from the SeasonOverrides override file (same file as the
// per-car/per-team overrides). Global keys (affect AI and player alike). See docs/pit-stops.md.
//   PitTyreChangeMs    tyre-change base time (stock 7000)
//   PitDamageRepairMs  damage-repair base time (stock 18000)
//   PitRefuelBaseMs    fixed refuel overhead (stock 2750)
//   PitRefuelCapMs     max refuel duration cap (stock 20000)
//   PitRefuelSpeed     refuel speed, % of stock (100 = stock, 200 = 2x faster)
//   DisableRefuel      1 = cars start full and add no fuel (tyre stops still happen)

extern unsigned long PitStopsActive;     // 0 = nothing patched

void PitStopsInit(void);                  // parse override file pit keys + patch (call once, late init)

#endif
