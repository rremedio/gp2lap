#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drvdata.h"
#include "miscahf.h"        /* IDAtoFlat */
#include "basiclog.h"       /* LogLine / strbuf */
#include "override.h"       /* shared SeasonOverrides model ([Team N] per-driver keys) */

/* Per-driver data override. Applies the resolved SeasonOverrides driver fields into GP2.EXE's
   own driver tables at late init (DriverDataInit), and re-applies the two tables that the
   savegame / network restore overwrites (DriverDataReapply). See drvdata.h + the docs. */

/* IDA addresses of the GP2.EXE driver tables (write via IDAtoFlat; little-endian). */
#define DD_NAMES   0x179026UL   /* t_DriverNames  40 x 24 bytes, NUL-terminated */
#define DD_SKILL   0x1745E8UL   /* word_1745E8    40 x 4: qual @+0, race @+2     */
#define DD_RNGWT   0x174688UL   /* word_174688    40 x 4: B/range @+0, A/wt @+2  */
#define DD_TAB     0x178F9AUL   /* t_CaridTeamTab 40 bytes (carId|MP|selected)   */
#define DD_RGS     0x6BD46UL    /* RestoreGameState (savegame/network restore)   */

#define DD_SKILLBIAS 0x3D87     /* 15751: skill rating bias added to clamped skill */

/* the asm wrap on the restore call sites: calls the original RestoreGameState, then reapply */
extern void MyRestoreGameState(void);
unsigned long RestoreGameStateAddr = 0;                 /* read by the asm stub (orig RGS flat) */
void (__near _cdecl *fpDrvDataReapply)(void) = DriverDataReapply;

/* one record per slot we actually patched, used by DriverDataReapply after a restore */
typedef struct {
  int  tabIndex;        /* (team-1)*2 + slot, 0..27 */
  int  disabled;        /* 1 = write 0x00 (no driver) */
  int  cid;             /* effective carId 0..63 (valid when !disabled) */
  int  selectedSet;     /* 1 = force bit7; 0 = preserve current table bit7 */
  int  selected;        /* human flag when selectedSet */
  int  idx;             /* driverIndex (cid-1) 0..39, or -1 if no name to restore */
  int  nameSet;         /* 1 = restore the 24-byte name */
  char name[24];        /* the padded 24-byte name as written at init */
} DrvSlot;

static DrvSlot g_slot[OV_TEAMS * 2];
static int     g_nSlots = 0;

/* clamp v into [lo,hi]; warn (override.c style) when it was out of range */
static long ClampWarn(long v, long lo, long hi, const char *what, int team, int slot)
{
  if (v < lo) { sprintf(strbuf, "- DriverData: team%02d seat%d %s %ld < %ld; clamped\n",
                        team, slot+1, what, v, lo); LogLine(strbuf); return lo; }
  if (v > hi) { sprintf(strbuf, "- DriverData: team%02d seat%d %s %ld > %ld; clamped\n",
                        team, slot+1, what, v, hi); LogLine(strbuf); return hi; }
  return v;
}

/* build the packed t_CaridTeamTab byte: bits0-5 carId, bit6 MP (always 0), bit7 selected.
   bit7Src supplies the preserved selected bit when selectedSet is 0. */
static unsigned char PackedByte(int cid, int selectedSet, int selected, unsigned char bit7Src)
{
  unsigned char b = (unsigned char)(cid & 0x3F);   /* bit6 left clear by the mask */
  if (selectedSet) { if (selected) b |= 0x80; }
  else             { b |= (unsigned char)(bit7Src & 0x80); }
  return b;
}

/* Install the asm wrap over the three E8 calls to RestoreGameState. All-or-nothing:
   verify each is a 5-byte E8 call whose target is RestoreGameState, then re-point them
   (recomputing each rel32) at MyRestoreGameState. */
static void InstallRestoreHook(void)
{
  static const unsigned long site[3] = { 0x6B600UL, 0x6BBFBUL, 0x6BE24UL };
  unsigned char *rgs = IDAtoFlat(DD_RGS);
  unsigned char *p[3];
  int i, ok = 1;

  RestoreGameStateAddr = (unsigned long)rgs;

  for (i = 0; i < 3; i++) {
    long disp;
    p[i] = IDAtoFlat(site[i]);
    if (p[i][0] != 0xE8) { ok = 0; break; }
    disp = *(long *)(p[i] + 1);
    if ((unsigned char *)(p[i] + 5 + disp) != rgs) { ok = 0; break; }
  }
  if (!ok) {
    LogLine("- DriverData: restore call-site mismatch; reapply hook DISABLED\n");
    return;
  }
  for (i = 0; i < 3; i++)
    *(long *)(p[i] + 1) = (long)((unsigned long)MyRestoreGameState - (unsigned long)(p[i] + 5));
  LogLine("- DriverData: restore-reapply hook armed (3 sites)\n");
}

void DriverDataInit(void)
{
  unsigned char *tab   = IDAtoFlat(DD_TAB);
  unsigned char *names = IDAtoFlat(DD_NAMES);
  unsigned char *skill = IDAtoFlat(DD_SKILL);
  unsigned char *rngwt = IDAtoFlat(DD_RNGWT);
  unsigned char  stockTab[40];
  int used[OV_MAXCAR];
  int team, slot, i, idx, stockCid, cid, applied = 0, anyNum = 0;

  /* snapshot the stock table ONCE (resolve carIds from pre-override values) */
  for (i = 0; i < 40; i++) stockTab[i] = tab[i];
  for (i = 0; i < OV_MAXCAR; i++) used[i] = 0;
  g_nSlots = 0;

  for (team = 1; team <= OV_TEAMS; team++) {
    const OvTeam *t = OverrideTeam(team);
    if (!t) continue;
    for (slot = 0; slot < 2; slot++) {
      const OvDriver *d = &t->drv[slot];
      int ti = (team - 1) * 2 + slot;
      DrvSlot *rec;

      if (!(d->nameSet || d->qualSet || d->raceSet || d->rangeSet ||
            d->weightSet || d->numSet || d->selectedSet || d->disabledSet))
        continue;

      stockCid = stockTab[ti] & 0x3F;
      cid      = d->numSet ? d->num : stockCid;

      /* (3) disabled seat: clear the table byte, no driver data for this slot */
      if (d->disabledSet && d->disabled) {
        tab[ti] = 0x00;
        rec = &g_slot[g_nSlots++];
        rec->tabIndex = ti; rec->disabled = 1; rec->cid = 0;
        rec->selectedSet = 0; rec->selected = 0; rec->idx = -1; rec->nameSet = 0;
        sprintf(strbuf, "- DriverData: team%02d seat%d disabled\n", team, slot+1); LogLine(strbuf);
        applied++;
        continue;
      }

      /* carId must address a valid 1..40 driver slot; otherwise skip the whole slot
         (never write a garbage table byte for a bad / zero / negative num). */
      if (cid < 1 || cid > 40) {
        sprintf(strbuf, "- DriverData: team%02d seat%d carId %d out of range 1..40; slot skipped\n",
                team, slot+1, cid); LogLine(strbuf);
        continue;
      }

      /* duplicate-number guard (override-vs-override): drop the later, keep the first */
      if (d->numSet) {
        anyNum = 1;
        if (used[cid]) {
          sprintf(strbuf, "- DriverData: team%02d seat%d num %d duplicates an assigned carId; slot skipped\n",
                  team, slot+1, cid); LogLine(strbuf);
          continue;
        }
      }
      used[cid] = 1;

      /* (4) packed table byte */
      tab[ti] = PackedByte(cid, d->selectedSet, d->selected, stockTab[ti]);

      idx = cid - 1;                 /* always 0..39 here */
      rec = &g_slot[g_nSlots++];
      rec->tabIndex = ti; rec->disabled = 0; rec->cid = cid;
      rec->selectedSet = d->selectedSet; rec->selected = d->selected;
      rec->idx = idx;
      rec->nameSet = 0;

      /* (6) name: 24 bytes, copied + NUL-terminated + zero-padded */
      if (d->nameSet) {
        int n = (int)strlen(d->name);
        if (n > 23) n = 23;
        memcpy(names + idx*24, d->name, n);
        memset(names + idx*24 + n, 0, 24 - n);
        memcpy(rec->name, names + idx*24, 24);
        rec->nameSet = 1;
      }

      /* (7) skill: rating = clamp(skill,0,17016) + bias */
      if (d->qualSet)
        *(unsigned short *)(skill + idx*4 + 0) =
          (unsigned short)(ClampWarn(d->qual, 0, 17016, "qual", team, slot) + DD_SKILLBIAS);
      if (d->raceSet)
        *(unsigned short *)(skill + idx*4 + 2) =
          (unsigned short)(ClampWarn(d->race, 0, 17016, "race", team, slot) + DD_SKILLBIAS);

      /* (8) range B / weight A */
      if (d->rangeSet)
        *(unsigned short *)(rngwt + idx*4 + 0) =
          (unsigned short)ClampWarn(d->range, 0, 32767, "range", team, slot);
      if (d->weightSet)
        *(unsigned short *)(rngwt + idx*4 + 2) =
          (unsigned short)ClampWarn(d->weight, 0, 16384, "weight", team, slot);

      applied++;
    }
  }

  /* post-pass: warn on any duplicate carId left in the table — e.g. a renumber that
     collided with an UNTOUCHED stock car (the override-vs-override guard can't see those). */
  {
    int seen[OV_MAXCAR], c;
    for (c = 0; c < OV_MAXCAR; c++) seen[c] = 0;
    for (i = 0; i < OV_TEAMS * 2; i++) {
      c = tab[i] & 0x3F;
      if (c) {
        if (seen[c]) {
          sprintf(strbuf, "- DriverData: WARNING carId %d in >1 seat after override\n", c);
          LogLine(strbuf);
        }
        seen[c] = 1;
      }
    }
  }

  if (anyNum)
    LogLine("- DriverData: NOTE renumbered car(s); the painted on-car number is carset texture art "
            "and will not change automatically\n");

  sprintf(strbuf, "- DriverData: %d drivers patched\n", applied); LogLine(strbuf);

  if (applied) InstallRestoreHook();
}

/* Re-write ONLY the names + t_CaridTeamTab bytes that RestoreGameState overwrites
   (skill/range/weight live below the save block and persist). Called via fpDrvDataReapply
   from the asm wrap after the original RestoreGameState returns. */
void __near _cdecl DriverDataReapply(void)
{
  unsigned char *tab   = IDAtoFlat(DD_TAB);
  unsigned char *names = IDAtoFlat(DD_NAMES);
  int i;

  for (i = 0; i < g_nSlots; i++) {
    DrvSlot *r = &g_slot[i];
    if (r->disabled) { tab[r->tabIndex] = 0x00; continue; }
    /* preserve the just-restored selected bit when the slot did not force it */
    tab[r->tabIndex] = PackedByte(r->cid, r->selectedSet, r->selected, tab[r->tabIndex]);
    if (r->nameSet && r->idx >= 0)
      memcpy(names + r->idx*24, r->name, 24);
  }
}
