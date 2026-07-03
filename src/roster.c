#include <stdio.h>
#include "roster.h"
#include "override.h"       /* OverrideActiveTeams / OverrideTeamDefined / OV_STOCKTEAMS / OV_TEAMS */
#include "miscahf.h"        /* IDACodeReftoDataRef */
#include "basiclog.h"       /* LogLine / strbuf */

/* operand of `mov ecx, d_anzteams` @0x14EBA+2 -> &d_anzteams (0x179ED7, stock dd 14) */
#define ROSTER_ANZREF 0x14EBCUL

void RosterInit(void)
{
  unsigned long *anz    = (unsigned long *)IDACodeReftoDataRef(ROSTER_ANZREF);
  int            active = OverrideActiveTeams();

  /* if the first team past the active run carries data but is incomplete, say why it's dropped */
  if (active < OV_TEAMS && OverrideTeamDefined(active + 1)) {
    sprintf(strbuf, "- Roster: [Team %d] incomplete (needs TeamName + EngineName + Power + Livery + "
                    "a seat with Name/Num/Qual/Race); not fielded\n", active + 1);
    LogLine(strbuf);
  }

  if (active <= OV_STOCKTEAMS) return;            /* no valid added team -> leave stock 14 */

  if (!anz) { LogLine("- Roster: d_anzteams unresolved; added teams DISABLED\n"); return; }
  if (*anz != OV_STOCKTEAMS) {                    /* guard against a mis-resolved address */
    sprintf(strbuf, "- Roster: stock d_anzteams reads %lu (want 14); added teams DISABLED\n", *anz);
    LogLine(strbuf);
    return;
  }

  *anz = (unsigned long)active;
  sprintf(strbuf, "- Roster: %d teams active (14 stock + %d added); field truncates to fastest 26\n",
          active, active - OV_STOCKTEAMS);
  LogLine(strbuf);

  /* the driver-selection screen freezes at 19-20 teams (roadmap 6e); everything else is fine */
  if (active >= 19)
    LogLine("- Roster: WARNING 19-20 teams freeze the driver-selection screen (see 6e); prefer <=18\n");
}
