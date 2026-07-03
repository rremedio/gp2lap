#ifndef ROSTER_H
#define ROSTER_H

/* Phase 4a: field up to 20 teams / 40 drivers (stock 14/28). RosterInit sets d_anzteams
   (0x179ED7, stock 14) to the active team count = 14 stock + a contiguous run of valid
   override-added teams 15..20 (see OverrideActiveTeams). The pool builder then enumerates
   the added teams, and the native qualifying sort still truncates the field to the fastest
   26 that race -- no car-struct work (only <=26 are ever instantiated per session).

   Driver data (drvdata) and per-team perf (teamphys) fill the 40/20-wide tables for the
   added teams; their car liveries/helmets are layered on later (4a.2/4a.3). Until then,
   teams 15..20 render with the stock team-14 skin via the sub_677D0 clamp (safe fallback). */

void RosterInit(void);   /* set d_anzteams from OverrideActiveTeams; call after OverrideLoad */

#endif /* ROSTER_H */
