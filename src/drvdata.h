#ifndef _DRVDATA_H
#define _DRVDATA_H

/* 2026: per-driver data override, read from the shared SeasonOverrides model
   ([Team N] name1/name2, qual1/2, race1/2, range1/2, weight1/2, num1/2,
   selected1/2, disabled1/2). Applies, at late init, into GP2.EXE's own tables:
     t_DriverNames  (0x179026, 40 x 24)   driver name strings
     word_1745E8    (40 x 4)              qual word @+0, race word @+2 (skill)
     word_174688    (40 x 4)              B/range @+0, A/weight @+2
     t_CaridTeamTab (0x178F9A, 40 bytes)  packed carId|MP|selected per team seat
   driverIndex = carId - 1.  RestoreGameState (savegame / network restore) overwrites
   the names + t_CaridTeamTab block, so DriverDataReapply re-writes those two (only)
   after each restore via an asm wrap on the three restore call sites.
   See docs/gp2lap/driver-data-override.md. */

void DriverDataInit(void);                  /* full apply + install the restore-reapply wrap */
void __near _cdecl DriverDataReapply(void); /* re-write names + t_CaridTeamTab after a restore
                                               (called from MyRestoreGameState in lammcall.asm) */

#endif /* _DRVDATA_H */
