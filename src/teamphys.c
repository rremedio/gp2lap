#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "teamphys.h"
#include "miscahf.h"        // IDAtoFlat, IDACodeReftoDataRef
#include "basiclog.h"       // LogLine / strbuf
#include "override.h"       // shared SeasonOverrides model ([Team N] mass/downforce)
#include "gp2def.h"         // BYTE/DWORD/... (types used by gp2glob.h)
#include "gp2glob.h"        // pTeamHPQual (&0x174598 = team perf/skill/reliability base)

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

/* Per-team engine performance tables (race power / qual power / reliability): word tables,
   2-byte stride, team index 0..13. They live BELOW the savegame block and are never overwritten
   at runtime, so a one-time startup write persists -- no re-apply hook needed. The two power
   tables store a biased "PS" value (stored = clamp(PS,0,1579) + 0x8031); reliability is raw
   clamp(0,32767), higher = more fragile.
   NB: these are DATA addresses, NOT reachable via IDAtoFlat (that maps CODE). All three sit in the
   region whose base (t_TeamPerfValue 0x174598) GP2Lap resolves into pTeamHPQual; the siblings are
   fixed byte offsets from it. */
#define TP_PERF_QUALOFS 0x28UL    /* word_1745C0 - 0x174598 (qual power) */
#define TP_PERF_RELOFS  0x190UL   /* t_teamwhat  - 0x174598 (reliability) */
#define TP_PS_BIAS   0x8031       /* "PS" bias added on the two power tables only */
#define TP_PS_MAX    1579L
#define TP_REL_MAX   32767L

/* clamp v into [lo,hi]; warn (teamphys logging style) when it was out of range */
static long TpClamp(long v, long lo, long hi, const char *what, int team)
{
  if (v < lo) { sprintf(strbuf, "- TeamPerf: team%02d %s %ld < %ld; clamped\n", team, what, v, lo);
                LogLine(strbuf); return lo; }
  if (v > hi) { sprintf(strbuf, "- TeamPerf: team%02d %s %ld > %ld; clamped\n", team, what, v, hi);
                LogLine(strbuf); return hi; }
  return v;
}

static void TeamPerfApply(void)
{
  unsigned char *perf = (unsigned char *)pTeamHPQual;   /* &t_TeamPerfValue (0x174598) */
  unsigned char *race, *qual, *rel;
  int team, nTeams = 0;

  if (!perf) { LogLine("- TeamPerf: pTeamHPQual unresolved; skipped\n"); return; }
  race = perf;                    /* 0x174598 */
  qual = perf + TP_PERF_QUALOFS;  /* 0x1745C0 */
  rel  = perf + TP_PERF_RELOFS;   /* 0x174728 */

  for (team = 1; team <= TP_TEAMS; team++) {
    const OvTeam *t = OverrideTeam(team);
    int off = (team - 1) * 2;       /* team index 0..13, 2 bytes per team */
    int touched = 0;
    if (!t) continue;
    if (t->powerSet) {
      long v = TpClamp(t->power, 0, TP_PS_MAX, "power", team);
      *(unsigned short *)(race + off) = (unsigned short)(v + TP_PS_BIAS);
      sprintf(strbuf, "- TeamPerf: team%02d power %ld PS\n", team, v); LogLine(strbuf); touched = 1;
    }
    if (t->qualpowerSet) {
      long v = TpClamp(t->qualpower, 0, TP_PS_MAX, "qualpower", team);
      *(unsigned short *)(qual + off) = (unsigned short)(v + TP_PS_BIAS);
      sprintf(strbuf, "- TeamPerf: team%02d qualpower %ld PS\n", team, v); LogLine(strbuf); touched = 1;
    }
    if (t->reliabilitySet) {
      long v = TpClamp(t->reliability, 0, TP_REL_MAX, "reliability", team);
      *(unsigned short *)(rel + off) = (unsigned short)v;   /* raw, no PS bias */
      sprintf(strbuf, "- TeamPerf: team%02d reliability %ld\n", team, v); LogLine(strbuf); touched = 1;
    }
    if (touched) nTeams++;
  }

  if (nTeams) { sprintf(strbuf, "- TeamPerf: %d teams patched\n", nTeams); LogLine(strbuf); }
}

void TeamPhysInit(void)
{
  int i, anyMass = 0, anyDF = 0, applied = 0;

  for (i = 0; i < TP_TEAMS; i++) { TeamMassLbs[i] = 0; TeamDFMult[i] = 0; }

  for (i = 1; i <= TP_TEAMS; i++) {
    const OvTeam *t = OverrideTeam(i);
    if (t->massSet) {
      long val = t->mass;
      long lbs = (val * 2205L + 500L) / 1000L;        /* kg -> lbs (x2.205) */
      if (lbs < 1) lbs = 1;                           /* keep non-zero = "set" */
      TeamMassLbs[i-1] = (unsigned long)lbs; anyMass = 1;
      sprintf(strbuf, "- TeamPhys: team%02d mass %ld kg (%ld lb)\n", i, val, lbs); LogLine(strbuf);
    }
    if (t->dfSet) {
      long val = t->downforce;
      if (val < 1) val = 1; if (val > 200) val = 200;
      TeamDFMult[i-1] = (unsigned long)val; anyDF = 1;
      sprintf(strbuf, "- TeamPhys: team%02d downforce %ld%%\n", i, val); LogLine(strbuf);
    }
  }

  /* engine perf tables: independent DATA writes (run regardless of mass/downforce) */
  TeamPerfApply();

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
