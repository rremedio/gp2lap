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

/* stock snapshot of the three perf tables, taken once at init BEFORE any override write,
   so an unset team can be reverted to stock when per-track overrides change track to track. */
static unsigned short g_stockRace[TP_TEAMS], g_stockQual[TP_TEAMS], g_stockRel[TP_TEAMS];
static int g_haveSnap = 0;

/* 1c: per-team downforce wobble. A self-contained PRNG (seeded once per launch from the
   BIOS tick, independent of GP2's RNG) rolls a +/- offset ONCE PER WEEKEND (slot change),
   so a team's DF "form" is fixed for the weekend but varies each playthrough. */
static unsigned long g_rng = 0;
static int g_rollSlot = -1;             /* calendar slot the current wobble was rolled for */
static int g_dfRoll[TP_TEAMS];          /* per-team signed % offset for this weekend */

static int RngNext(void)                /* 0..0x7FFF */
{
  g_rng = g_rng * 1103515245UL + 12345UL;
  return (int)((g_rng >> 16) & 0x7FFF);
}

/* Recompute and write the per-team physics from the layered model:
   per-track (when haveTrack, via OverrideTrackTeam) ?? season (OverrideTeam) ?? stock.
   mass/downforce go to the GP2Lap tables the asm stubs read (0 = stock); the perf tables
   are written directly, reverting an unset team to its stock snapshot. Called at init
   (haveTrack=0) and each SOS. Quiet -- TpClamp still warns on an out-of-range value. */
static void ApplyMerged(int haveTrack)
{
  unsigned char *perf = (unsigned char *)pTeamHPQual;
  unsigned char *race = 0, *qual = 0, *rel = 0;
  int team;

  if (perf) { race = perf; qual = perf + TP_PERF_QUALOFS; rel = perf + TP_PERF_RELOFS; }

  for (team = 1; team <= TP_TEAMS; team++) {
    const OvTeam *s  = OverrideTeam(team);
    const OvTeam *tt = haveTrack ? OverrideTrackTeam(team) : 0;
    int idx = team - 1, off = idx * 2;

    /* mass -> TeamMassLbs (0 = stock via MyTeamMass) */
    if      (tt && tt->massSet) { long lbs=(tt->mass*2205L+500L)/1000L; if(lbs<1)lbs=1; TeamMassLbs[idx]=(unsigned long)lbs; }
    else if (s->massSet)        { long lbs=(s->mass *2205L+500L)/1000L; if(lbs<1)lbs=1; TeamMassLbs[idx]=(unsigned long)lbs; }
    else                        TeamMassLbs[idx] = 0;

    /* downforce -> TeamDFMult (0 = stock via MyScaleDF). Effective multiplier =
       (per-track ?? season base, else stock) + the per-weekend wobble g_dfRoll (1c). */
    {
      int  range = (tt && tt->dfRangeSet) ? (int)tt->dfRange : (s->dfRangeSet ? (int)s->dfRange : 0);
      long base  = (tt && tt->dfSet)      ? tt->downforce    : (s->dfSet      ? s->downforce     : 0);
      if (range > 0) {
        long m = (base > 0 ? base : 100) + g_dfRoll[idx];   /* wobble around the base (or stock 100) */
        if (m < 1) m = 1; if (m > 200) m = 200;
        TeamDFMult[idx] = (unsigned long)m;
      } else if (base > 0) {
        if (base < 1) base = 1; if (base > 200) base = 200;
        TeamDFMult[idx] = (unsigned long)base;
      } else {
        TeamDFMult[idx] = 0;                                 /* no override -> stock */
      }
    }

    if (!race || !g_haveSnap) continue;           /* perf tables unavailable */

    /* power / qualpower / reliability -> perf tables (unset -> stock snapshot).
       NB reliability is rolled at session init (CalcCarDamage); if that runs before this
       SOS apply, a per-track reliability change lands the FOLLOWING session. power/mass/DF
       are read later in the session, so they take effect immediately. */
    if      (tt && tt->powerSet) *(unsigned short*)(race+off) = (unsigned short)(TpClamp(tt->power,0,TP_PS_MAX,"power",team)+TP_PS_BIAS);
    else if (s->powerSet)        *(unsigned short*)(race+off) = (unsigned short)(TpClamp(s->power, 0,TP_PS_MAX,"power",team)+TP_PS_BIAS);
    else                         *(unsigned short*)(race+off) = g_stockRace[idx];

    if      (tt && tt->qualpowerSet) *(unsigned short*)(qual+off) = (unsigned short)(TpClamp(tt->qualpower,0,TP_PS_MAX,"qualpower",team)+TP_PS_BIAS);
    else if (s->qualpowerSet)        *(unsigned short*)(qual+off) = (unsigned short)(TpClamp(s->qualpower, 0,TP_PS_MAX,"qualpower",team)+TP_PS_BIAS);
    else                             *(unsigned short*)(qual+off) = g_stockQual[idx];

    if      (tt && tt->reliabilitySet) *(unsigned short*)(rel+off) = (unsigned short)TpClamp(tt->reliability,0,TP_REL_MAX,"reliability",team);
    else if (s->reliabilitySet)        *(unsigned short*)(rel+off) = (unsigned short)TpClamp(s->reliability, 0,TP_REL_MAX,"reliability",team);
    else                               *(unsigned short*)(rel+off) = g_stockRel[idx];
  }
}

void TeamPhysInit(void)
{
  unsigned char *perf = (unsigned char *)pTeamHPQual;
  int i, anyMass = 0, anyDF = 0, anyRange = 0, anyPerTrack = 0, applied = 0;

  for (i = 0; i < TP_TEAMS; i++) { TeamMassLbs[i] = 0; TeamDFMult[i] = 0; g_dfRoll[i] = 0; }

  /* seed the DF-wobble PRNG once per launch from the BIOS tick (0000:046C), so the wobble
     varies each playthrough; fall back to a constant if that read yields 0. */
  g_rng = *(volatile unsigned long *)0x46CUL;
  if (!g_rng) g_rng = 0x13579BDFUL;

  /* snapshot stock perf BEFORE any override write (needed to revert an unset team) */
  if (perf) {
    unsigned char *race = perf, *qual = perf + TP_PERF_QUALOFS, *rel = perf + TP_PERF_RELOFS;
    for (i = 0; i < TP_TEAMS; i++) {
      g_stockRace[i] = *(unsigned short *)(race + i*2);
      g_stockQual[i] = *(unsigned short *)(qual + i*2);
      g_stockRel[i]  = *(unsigned short *)(rel  + i*2);
    }
    g_haveSnap = 1;
  } else LogLine("- TeamPerf: pTeamHPQual unresolved; perf skipped\n");

  /* what does the season layer set, and are there any per-track override files? */
  for (i = 1; i <= TP_TEAMS; i++)  { const OvTeam  *t  = OverrideTeam(i);  if (t->massSet) anyMass=1; if (t->dfSet) anyDF=1; if (t->dfRangeSet) anyRange=1; }
  for (i = 1; i <= OV_TRACKS; i++) { const OvTrack *tr = OverrideTrack(i); if (tr && tr->overrideFileSet) anyPerTrack=1; }

  ApplyMerged(0);   /* apply the season layer (mass/DF tables + perf tables) */

  /* MASS: 0x2C510 `add eax, d_carstdweight` (03 05 <abs32>) in FLoadToCarWght.
     Bootstrap &d_carstdweight from the operand FIRST (it lives in the bytes we overwrite), then
     replace with `call MyTeamMass` + NOP. The stub adds the per-team weight, or *d_carstdweight
     (the live global, edited or not) for unset teams -- so exe mass edits are respected.
     Arm whenever the season OR any per-track file may set mass (a 0 table entry = stock). */
  if (anyMass || anyPerTrack) {
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
  if (anyDF || anyRange || anyPerTrack) {
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

/* Called from AHFSOSHook (start of session). Merge the current track's override file (if
   any) over the season layer and (re)apply -- so per-track physics take effect and revert
   cleanly when the track changes. Keyed on pTrackIndex (calendar slot 0..15). */
void PerTrackPhysSOS(void)
{
  const OvTrack *tr;
  int slot, haveTrack = 0;

  if (!PerTeamPhysics) return;                     /* nothing armed -> nothing to do */
  if (!pTrackIndex) return;
  slot = (int)*pTrackIndex;                        /* 0..15 */
  tr = OverrideTrack(slot + 1);
  if (tr && tr->overrideFileSet) {
    char path[512]; const char *dir = OverrideBaseDir();
    if (dir && dir[0]) sprintf(path, "%s%s", dir, tr->overrideFile);
    else               strcpy(path, tr->overrideFile);
    if (OverrideParseTrackFile(path) >= 0) haveTrack = 1;
  }

  /* 1c: re-roll each team's DF wobble once per weekend (when the calendar slot changes).
     Range = per-track ?? season; offset = uniform[-range, +range] % points. */
  if (slot != g_rollSlot) {
    int team, any = 0;
    char *w = strbuf;
    w += sprintf(w, "- TeamPhys: DF wobble (slot %d):", slot);
    for (team = 1; team <= TP_TEAMS; team++) {
      const OvTeam *s  = OverrideTeam(team);
      const OvTeam *tt = haveTrack ? OverrideTrackTeam(team) : 0;
      int range = (tt && tt->dfRangeSet) ? (int)tt->dfRange : (s->dfRangeSet ? (int)s->dfRange : 0);
      g_dfRoll[team-1] = (range > 0) ? (RngNext() % (2*range + 1)) - range : 0;
      if (range > 0) { w += sprintf(w, " t%02d %+d", team, g_dfRoll[team-1]); any = 1; }
    }
    if (any) { sprintf(w, "\n"); LogLine(strbuf); }
    g_rollSlot = slot;
  }

  ApplyMerged(haveTrack);
  if (haveTrack) { sprintf(strbuf, "- TeamPhys: per-track overrides applied (slot %d)\n", slot); LogLine(strbuf); }
}
