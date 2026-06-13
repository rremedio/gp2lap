#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "teamphys.h"
#include "miscahf.h"        // IDAtoFlat, IDACodeReftoDataRef
#include "cfgmain.h"        // GetCfgString
#include "basiclog.h"       // LogLine / strbuf

/* Per-team mass + downforce. Both are overrides LAYERED on top of the EXE (set = override that team,
   unset = leave the exe alone), keyed on car.teamNr (+0x25). The asm stubs (lammcall.asm) read these
   tables; 0 = "team not overridden". See docs/gp2lap/runtime-physics-loading.md. */

#define TP_TEAMS 14

unsigned long PerTeamPhysics = 0;

/* read by the asm stubs */
unsigned long  TeamMassLbs[TP_TEAMS];        /* per-team chassis weight in lbs (0 = unset) */
unsigned long  TeamDFMult[TP_TEAMS];         /* per-team downforce %, 1..200 (0 = unset) */
unsigned long *pCarStdWeight = 0;            /* &d_carstdweight (fallback for unset mass teams) */

extern void MyTeamMass(void);                /* asm: per-team chassis weight at 0x2C510 */
extern void MyScaleDF(void);                 /* asm: per-team downforce scale at 0x168C7 */

/* If left-trimmed 'p' starts with 'name' (case-insensitive) followed by a digit, set *team to that
   number and return 1 (team validated 1..14). */
static int TPKey(const char *p, const char *name, int *team)
{
  int i;
  for (i = 0; name[i]; i++)
    if ((p[i] | 0x20) != (name[i] | 0x20)) return 0;
  if (p[i] < '0' || p[i] > '9') return 0;
  *team = atoi(p + i);
  return (*team >= 1 && *team <= TP_TEAMS);
}

void TeamPhysInit(void)
{
  char *cfg;
  FILE *f;
  char line[300];
  int i, anyMass = 0, anyDF = 0, applied = 0;

  cfg = GetCfgString("SeasonOverrides");
  if (!cfg || !cfg[0]) return;

  for (i = 0; i < TP_TEAMS; i++) { TeamMassLbs[i] = 0; TeamDFMult[i] = 0; }

  f = fopen(cfg, "rb");
  if (!f) return;
  while (fgets(line, sizeof(line), f)) {
    char *p = line, *eq, *v;
    int team;
    long val;
    while (*p == ' ' || *p == '\t') p++;
    if (*p==';' || *p=='#' || *p=='[' || *p=='\r' || *p=='\n' || *p==0) continue;
    eq = strchr(p, '=');
    if (!eq) continue;
    v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    val = strtol(v, (char **)0, 0);
    if (TPKey(p, "mass", &team)) {
      long lbs = (val * 2205L + 500L) / 1000L;        /* kg -> lbs (x2.205) */
      if (lbs < 1) lbs = 1;                           /* keep non-zero = "set" */
      TeamMassLbs[team-1] = (unsigned long)lbs; anyMass = 1;
      sprintf(strbuf, "- TeamPhys: team%02d mass %ld kg (%ld lb)\n", team, val, lbs); LogLine(strbuf);
    } else if (TPKey(p, "downforcemultiplier", &team)) {
      if (val < 1) val = 1; if (val > 200) val = 200;
      TeamDFMult[team-1] = (unsigned long)val; anyDF = 1;
      sprintf(strbuf, "- TeamPhys: team%02d downforce %ld%%\n", team, val); LogLine(strbuf);
    }
  }
  fclose(f);

  if (!anyMass && !anyDF) return;

  /* MASS: 0x2C510 `add eax, d_carstdweight` (03 05 <abs32>) in FLoadToCarWght.
     Bootstrap &d_carstdweight from the operand FIRST (it lives in the bytes we overwrite), then
     replace with `call MyTeamMass` + NOP. The stub adds the per-team weight, or *d_carstdweight
     (the live global, edited or not) for unset teams -- so exe mass edits are respected. */
  if (anyMass) {
    unsigned char *p = (unsigned char *)IDAtoFlat(0x2C510);
    if (p[0]==0x03 && p[1]==0x05) {
      pCarStdWeight = (unsigned long *)IDACodeReftoDataRef(0x2C512);
      p[0] = 0xE8;
      *(long *)(p + 1) = (long)((unsigned long)MyTeamMass - (unsigned long)(p + 5));
      p[5] = 0x90;
      LogLine("- TeamPhys: per-team mass armed (0x2C510)\n"); applied++;
    } else LogLine("- TeamPhys: mass opcode mismatch at 0x2C510; mass DISABLED\n");
  }

  /* DOWNFORCE: 0x168C7 `mov eax,[esi+16Ch]` (8B 86 6C 01 00 00) + 0x168CD `mov [esi+4Eh],ax`
     (66 89 46 4E) inside CalcWings? -- right after +0x170 (the front/rear split) is set, so scaling
     +0x16C here keeps balance. Replace the 10 bytes with `call MyScaleDF` + NOPs; the stub scales
     +0x16C by the team %, copies to +0x4E, and skips invalid teamNr (incl. the accel-table dummy). */
  if (anyDF) {
    unsigned char *p = (unsigned char *)IDAtoFlat(0x168C7);
    if (p[0]==0x8B && p[1]==0x86 && p[2]==0x6C && p[3]==0x01 &&
        p[6]==0x66 && p[7]==0x89 && p[8]==0x46 && p[9]==0x4E) {
      p[0] = 0xE8;
      *(long *)(p + 1) = (long)((unsigned long)MyScaleDF - (unsigned long)(p + 5));
      memset(p + 5, 0x90, 5);
      LogLine("- TeamPhys: per-team downforce armed (0x168C7)\n"); applied++;
    } else LogLine("- TeamPhys: downforce opcode mismatch at 0x168C7; downforce DISABLED\n");
  }

  if (applied) PerTeamPhysics = 1;
}
