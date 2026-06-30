#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pitcrew.h"
#include "miscahf.h"        /* IDAtoFlat */
#include "basiclog.h"       /* LogLine / strbuf */
#include "override.h"       /* shared SeasonOverrides model ([Team N] pitcrew key) */

/* Per-team pit-crew colours. Plain DATA write (no code patch) into t_PitCrewColors:
   14 teams x 16 ramp-base bytes (stride 16, team index 0..13). Within each team's 16
   bytes idx0 is always 0x00 and idx1 always 0x10 (fixed shadow/highlight ramps); the user
   supplies idx2..idx15 in pitcrew[14] (pitcrew[0]->idx2 ... pitcrew[13]->idx15), each a
   palette index 0..255. The engine re-expands this table into its runtime LUT via
   MakeCrewColors (0x391ED) at session setup -- AFTER this startup write -- and the table
   lives OUTSIDE the savegame block, so a one-time write persists. No expander / re-apply
   hook needed. See docs/driver-data.md. */

#define PC_TEAMS  14
#define PC_TABLE  0x183338UL   /* t_PitCrewColors  14 teams x 16 bytes */

void PitCrewColorsInit(void)
{
  unsigned char *base = (unsigned char *)IDAtoFlat(PC_TABLE);
  int team, i, nTeams = 0;

  for (team = 1; team <= PC_TEAMS; team++) {
    const OvTeam *t = OverrideTeam(team);
    unsigned char *row;
    if (!t || !t->pitcrewSet) continue;

    row = base + (team - 1) * 16;

    /* verify the two fixed ramp bases before writing (guards a wrong address / build) */
    if (row[0] != 0x00 || row[1] != 0x10) {
      sprintf(strbuf, "- PitCrew: team%02d unexpected fixed bases 0x%02X,0x%02X; skipped\n",
              team, row[0], row[1]);
      LogLine(strbuf);
      continue;
    }

    for (i = 0; i < 14; i++) row[2 + i] = t->pitcrew[i];
    nTeams++;
  }

  if (nTeams) { sprintf(strbuf, "- PitCrew: %d teams patched\n", nTeams); LogLine(strbuf); }
}
