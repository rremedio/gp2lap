#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "carshape.h"
#include "miscahf.h"        // IDAtoFlat, IDACodeReftoDataRef
#include "cfgmain.h"        // GetCfgString
#include "basiclog.h"       // LogLine / strbuf

#define CS_TEAMS 14

/* Stock GP2 picks each team's car nose (low/high) from a static 14-dword table dword_CB394:
   sub_677D0 reads dword_CB394[team] -> dword_CB358 every car draw, and CB358 drives the nose
   morph (pointsBegin += [hdr+0x34]*CB358 at 0x66439/0x66EE6). CB394 is never written at runtime,
   so writing it once at init makes the choice stick. This is tier 1 of docs/per-team-car-shapes.md
   -- a per-team pick between the two nose pools already present in the shared car .dat. */

unsigned long PerTeamNose = 0;

/* Scan override.cfg for "noseNN = 0|1" (NN = team 1..14). Fills nose[]/set[], returns count. */
static int CarShapeParseNose(const char *cfgpath, unsigned char *nose, unsigned char *set)
{
  FILE *f;
  char line[300];
  int loaded = 0;

  f = fopen(cfgpath, "rb");
  if (!f) return 0;
  while (fgets(line, sizeof(line), f)) {
    char *p = line, *eq, *v;
    int team;
    while (*p == ' ' || *p == '\t') p++;
    if (*p==';' || *p=='#' || *p=='[' || *p=='\r' || *p=='\n' || *p==0) continue;
    if ((p[0]|0x20)!='n' || (p[1]|0x20)!='o' || (p[2]|0x20)!='s' || (p[3]|0x20)!='e') continue;
    team = atoi(p + 4);
    if (team < 1 || team > CS_TEAMS) continue;
    eq = strchr(p, '=');
    if (!eq) continue;
    v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    nose[team-1] = (atoi(v) != 0) ? 1 : 0;     /* 0 = low nose, anything else = high */
    set[team-1] = 1;
    loaded++;
  }
  fclose(f);
  return loaded;
}

void CarShapeInit(void)
{
  char *cfg;
  unsigned char nose[CS_TEAMS], set[CS_TEAMS], *site;
  unsigned long *pCB394;
  int i, loaded;

  cfg = GetCfgString("SeasonOverrides");
  if (!cfg || !cfg[0]) return;                 /* override file absent -> feature off */

  for (i = 0; i < CS_TEAMS; i++) { nose[i] = 0; set[i] = 0; }
  loaded = CarShapeParseNose(cfg, nose, set);
  if (loaded < 1) return;                       /* no noseNN keys */

  /* dword_CB394 read in sub_677D0:
       00067818  8B 04 95 <disp32>   mov eax, dword_CB394[edx*4]
     The disp32 is the *runtime flat* address (the loader relocates it at load -- e.g. it reads
     0x520394 here, not the link-time value), so only check the opcode/SIB (8B 04 95) and the
     stable low byte 0x94 that identifies THIS read (vs neighbouring CB35C/CB354/CB358, low bytes
     5C/54/58). Read the real address from the operand -- exactly where the engine reads. */
  site = (unsigned char *)IDAtoFlat(0x67818);
  if (site[0]!=0x8B || site[1]!=0x04 || site[2]!=0x95 || site[3]!=0x94) {
    sprintf(strbuf, "- PerTeamNose: opcode mismatch at 0x67818 (%02X %02X %02X %02X); DISABLED\n",
            site[0], site[1], site[2], site[3]);
    LogLine(strbuf);
    return;
  }
  pCB394 = (unsigned long *)IDACodeReftoDataRef(0x6781B);   /* &dword_CB394[0] (runtime flat addr) */

  for (i = 0; i < CS_TEAMS; i++)
    if (set[i]) pCB394[i] = nose[i];

  PerTeamNose = 1;
  sprintf(strbuf, "- PerTeamNose: ON (%d team nose override(s))\n", loaded);
  LogLine(strbuf);
}
