#include <stdio.h>
#include "gridcap.h"
#include "cfgmain.h"        /* GetCfgULong */
#include "miscahf.h"        /* IDAtoFlat */
#include "basiclog.h"       /* LogLine / strbuf */
#include "gp2def.h"         /* GP2Car (id, flags_90); WORD/BYTE */
#include "gp2glob.h"        /* pNumCars = runtime &w_NumCars_26_ */
#include "gp2lap.h"         /* pCarStructs = runtime &t_CarStructs */

/* Small-grid support (cfg AllowSmallGrid, default off): race fewer than 26 cars when
   >2 drivers are disabled, instead of padding with carId-0 "phantom" cars.

   How GP2 really works (verified): the field is NOT pinned by the position-table length
   or the C9E40 render gate -- non-race sessions use the SAME 26-entry tables yet show only
   N cars. The car-placement loop sub_0_2C785 runs in both sessions; the NON-RACE path
   (L_NoRace @0x2C7DB) runs InitCarESI + rCarRetires per car so inactive ones are retired
   BEFORE the session goes live, but the RACE branch activates all 26 -> the (26-N) tail
   become phantoms that leak into the race-results classification.

   So the fix mirrors non-race at the SAME point (placement), data-driven where possible:
     1. Finaliser patches (startup): make sub_0_2C19D copy N and write w_NumCars_26_ =
        min(N,26), and zero t_GridTable[N..25] so the tail structs InitCarStructs builds
        get carId 0. (Placement is bounded by a literal 26, so it still reaches the tail.)
     2. Placement detour (startup, @0x2C7A1): for a RACE car whose carId is 0, run the
        engine's own SILENT retire idiom (sub_0_2C91D = rCarRetires + set field_5E bit1)
        instead of activating it -- exactly as non-race retires its inactive cars, BEFORE
        anything classifies or announces them. field_5E bit1 suppresses the race-only
        retirement announcer (which fires on flags_90 0x20 + field_5E bits clear), so the
        phantoms retire without the on-screen "<driver> is out of the race" messages.

   Earlier approach (REPLACED): a per-frame EOFHook flags_90 |= 0xA0 retire. It worked for
   the grid but was too LATE -- the phantoms were briefly active (results ghosts) and the
   announcer fired on every flag flip (the "retiring" message storm). Retiring at placement
   like non-race fixes both. For a full field there is no carId-0 tail -> everything no-ops. */

/* the two finaliser call sites + the placement detour host (CODE; IDAtoFlat OK). */
#define GC_COPYCNT   0x2C1ADUL   /* mov ecx,26            B9 1A 00 00 00            (5) */
#define GC_FIELDWR   0x2C1C7UL   /* mov w_NumCars_26_,26  66 C7 05 <disp32> 1A 00   (9) */
#define GC_PLACEHOST 0x2C7A1UL   /* test b_RaceMode,0FFh / jns L_NoRace  F6 05 <d32> FF 79 31 (9) */

/* placement-detour runtime targets (resolved in GridCapInit; read by GridCapPlace_). */
#define GC_INITCARESI  0x2C69AUL /* InitCarESI                                       */
#define GC_RCARRETIRES 0x2C495UL /* rCarRetires (pusha-framed; esi = car)            */
#define GC_REJOIN      0x2C7F1UL /* L_NoRace2 (grid spacing + loop continue)         */
#define GC_NORACE      0x2C7DBUL /* L_NoRace  (stock non-race retire)               */
#define GC_RACEBR      0x2C7AAUL /* stock race branch                               */

extern void GridCapCopyCount(void);   /* asm: ECX = min(w_NumCars_26_,26)              */
extern void GridCapFinalize(void);    /* asm: field=min(.,26); zero grid tail          */
extern void GridCapPlace(void);       /* asm: placement detour (silent retire phantoms) */

/* read by GridCapPlace_ (lammcall.asm) */
unsigned long GcBRaceModePtr = 0;     /* &b_RaceMode (relocated disp32)                */
unsigned long GcInitCarESI   = 0;     /* InitCarESI  flat                              */
unsigned long GcRCarRetires  = 0;     /* rCarRetires flat                              */
unsigned long GcPlaceRejoin  = 0;     /* L_NoRace2   flat                              */
unsigned long GcPlaceNoRace  = 0;     /* L_NoRace    flat                              */
unsigned long GcPlaceRace    = 0;     /* race branch flat                              */

static const unsigned char stockCopy[5] = { 0xB9, 0x1A, 0x00, 0x00, 0x00 }; /* mov ecx,26 */

static int g_armed = 0;

/* ---- Results fix: compact the finish order at menu setup ------------------------
   The renderer (results-list widget, loc_0_82C0C in sub_0_82BAA) draws
   dword_0_4CB608 rows straight from t_CarRaceOrder (0x179CF5). At race-end the
   finish-time sort sub_0_2F481 mis-ranks the carId-0 phantoms to the FRONT
   (drvidx=(0&0x3F)-1=-1 -> reads t_RaceTimes out of bounds, not the 0x8000 retired
   sentinel -> treated as ~0 time -> sorts first), shoving the real finishers to the
   tail. The reals keep their correct relative (time) order among themselves; only the
   carId-0 entries are spurious. So at menu setup we compact: drop the carId-0 entries
   and shift the reals to the front, preserving their order. GridCapMenuHook_ (asm)
   runs this from the dword_0_4CB608 write inside sub_0_82A1E -- once, post-race, after
   the sort and before the widget renders. */
extern void GridCapMenuHook(void);         /* asm stub over the 0x82A42 count write */
unsigned long pCB608Val = 0;               /* &dword_0_4CB608 (row count; asm does the store) */
void (__near _cdecl *fpGridCapMenu)(void) = 0;

#define GC_ORDERREF  0x2F4FBUL             /* code op whose operand = &t_CarRaceOrder */

void __near _cdecl GridCapMenuCompact(void)
{
  unsigned char *ord = (unsigned char *)IDACodeReftoDataRef(GC_ORDERREF);
  unsigned char reals[26];
  int i, j = 0;

  if (!g_armed || !ord) return;
  if (ord[0] & 0x3F) return;               /* front already a real -> compact/valid, nothing to do */
  for (i = 0; i < 26; i++)
    if (ord[i] & 0x3F) reals[j++] = ord[i];
  if (j == 0) return;                       /* no real finishers present (e.g. pre-race) -> leave as-is */
  for (i = 0; i < j; i++) ord[i] = reals[i];
  for (; i < 26; i++) ord[i] = 0;
}
/* -------------------------------------------------------------------------------- */

static int BytesMatch(const unsigned char *p, const unsigned char *want, int n)
{
  int i;
  for (i = 0; i < n; i++)
    if (p[i] != want[i]) return 0;
  return 1;
}

void GridCapInit(void)
{
  unsigned long *cfg;
  unsigned char *pc;       /* 0x2C1AD copy-loop count   */
  unsigned char *pf;       /* 0x2C1C7 field-size write   */
  unsigned char *pp;       /* 0x2C7A1 placement-detour host */

  cfg = GetCfgULong("AllowSmallGrid");
  if (!cfg || !*cfg) return;          /* DEFAULT OFF: do nothing, stock behaviour */

  if (!pNumCars) { LogLine("- SmallGrid: pNumCars unresolved; DISABLED\n"); return; }

  pc = IDAtoFlat(GC_COPYCNT);
  pf = IDAtoFlat(GC_FIELDWR);
  pp = IDAtoFlat(GC_PLACEHOST);

  /* all-or-nothing verify (relocated disp32s are skipped: the field-write disp32 is the
     RELOCATED &w_NumCars_26_ == pNumCars, and the placement-host disp32 is &b_RaceMode). */
  if (!BytesMatch(pc, stockCopy, 5)) {
    LogLine("- SmallGrid: site 0x2C1AD mismatch; DISABLED\n");
    return;
  }
  if (pf[0] != 0x66 || pf[1] != 0xC7 || pf[2] != 0x05 ||
      pf[7] != 0x1A || pf[8] != 0x00 ||
      *(unsigned long *)(pf + 3) != (unsigned long)pNumCars) {
    LogLine("- SmallGrid: site 0x2C1C7 mismatch; DISABLED\n");
    return;
  }
  if (pp[0] != 0xF6 || pp[1] != 0x05 || pp[6] != 0xFF || pp[7] != 0x79 || pp[8] != 0x31) {
    LogLine("- SmallGrid: site 0x2C7A1 mismatch; DISABLED\n");
    return;
  }

  /* resolve the placement-detour runtime targets (read by GridCapPlace_) */
  GcBRaceModePtr = (unsigned long)IDACodeReftoDataRef(GC_PLACEHOST + 2);  /* &b_RaceMode */
  GcInitCarESI   = (unsigned long)IDAtoFlat(GC_INITCARESI);
  GcRCarRetires  = (unsigned long)IDAtoFlat(GC_RCARRETIRES);
  GcPlaceRejoin  = (unsigned long)IDAtoFlat(GC_REJOIN);
  GcPlaceNoRace  = (unsigned long)IDAtoFlat(GC_NORACE);
  GcPlaceRace    = (unsigned long)IDAtoFlat(GC_RACEBR);
  if (!GcBRaceModePtr) {
    LogLine("- SmallGrid: b_RaceMode unresolved; DISABLED\n");
    return;
  }

  /* install the two finaliser call patches + the placement detour (startup -> dynrec-safe) */
  pc[0] = 0xE8;
  *(long *)(pc + 1) = (long)((unsigned long)GridCapCopyCount - (unsigned long)(pc + 5));

  pf[0] = 0xE8;
  *(long *)(pf + 1) = (long)((unsigned long)GridCapFinalize - (unsigned long)(pf + 5));
  pf[5] = 0x90; pf[6] = 0x90; pf[7] = 0x90; pf[8] = 0x90;

  pp[0] = 0xE9;            /* jmp rel32 -> GridCapPlace_ */
  *(long *)(pp + 1) = (long)((unsigned long)GridCapPlace - (unsigned long)(pp + 5));
  pp[5] = 0x90; pp[6] = 0x90; pp[7] = 0x90; pp[8] = 0x90;

  g_armed = 1;
  LogLine("- SmallGrid: enabled (silent engine-retire of carId-0 grid tail at placement)\n");

  /* Results fix: hook the row-count write @0x82A42 (inside sub_0_82A1E, menu setup) so we
     compact t_CarRaceOrder after the sort and before the widget renders. The asm stub does
     the original store (dword_0_4CB608 = eax) then calls the compactor. */
  {
    unsigned char *pw = IDAtoFlat(0x82A42UL);   /* mov dword_0_4CB608,eax  A3 08 B6 4C 00 */
    if (pw[0] == 0xA3) {
      pCB608Val = (unsigned long)IDACodeReftoDataRef(0x82A43UL);   /* &dword_0_4CB608 */
      fpGridCapMenu = GridCapMenuCompact;
      pw[0] = 0xE8;
      *(long *)(pw + 1) = (long)((unsigned long)GridCapMenuHook - (unsigned long)(pw + 5));
      LogLine("- SmallGrid: results-order compaction installed @0x82A42\n");
    } else {
      LogLine("- SmallGrid: site 0x82A42 mismatch; results compaction NOT installed\n");
    }
  }
}
