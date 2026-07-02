#include <stdio.h>
#include "sessions.h"
#include "override.h"       /* OverrideWeekend / OverrideSprint */
#include "miscahf.h"        /* IDAtoFlat / IDACodeReftoDataRef */
#include "basiclog.h"       /* LogLine / strbuf */

/* The three sites carrying the 0xF3 session-mask literal (immediate byte at instr+6; the
   disp32 in between is the relocated &byte_0_179646, so it is NOT verified). */
#define SS_INIT 0x6A76AUL   /* C6 05 <disp32> F3   mov byte_0_179646, 0F3h  (init)         */
#define SS_CHK1 0x6B246UL   /* 80 3D <disp32> F3   cmp byte_0_179646, 0F3h  (fresh check)  */
#define SS_CHK2 0x6B3E4UL   /* 80 3D <disp32> F3   cmp byte_0_179646, 0F3h  (fresh check)  */

/* --- 2b sprint: the warmup dispatch slot + the addresses the SprintCave_ (asm) reads --- */
#define SP_DISPATCH 0x6A7D9UL /* t_FuncTab6A7C9[4] (warmup slot entry)        */
#define SP_WARMUP   0x6AB7FUL /* loc_0_6AB7F (stock warmup handler)           */
extern void SprintCave(void);

unsigned long Sp_GridA=0, Sp_GridB=0, Sp_InitStart=0, Sp_GridFin=0, Sp_RaceMain=0, Sp_Cleanup=0, Sp_LapDerive=0;
unsigned long Sp_pGridReady=0, Sp_pLaps=0, Sp_pLapBound=0, Sp_pTimeCap=0, Sp_pRaceMode=0, Sp_pMask=0, Sp_pAbort=0;
unsigned long Sp_jLoop=0, Sp_jExit=0, Sp_Laps=0;

/* repoint the warmup dispatch entry to the sprint cave (called only when Sprint=1) */
static void SprintArm(int laps)
{
  unsigned long *te = (unsigned long *)IDAtoFlat(SP_DISPATCH);
  unsigned long  warmup = (unsigned long)IDAtoFlat(SP_WARMUP);

  Sp_GridA     = (unsigned long)IDAtoFlat(0x2C039UL);
  Sp_GridB     = (unsigned long)IDAtoFlat(0x2C12DUL);
  Sp_InitStart = (unsigned long)IDAtoFlat(0x2C25FUL);   /* InitStartOrder */
  Sp_GridFin   = (unsigned long)IDAtoFlat(0x2C19DUL);
  Sp_RaceMain  = (unsigned long)IDAtoFlat(0x6A352UL);   /* RaceMainFunc */
  Sp_Cleanup   = (unsigned long)IDAtoFlat(0x6A6B0UL);
  Sp_LapDerive = (unsigned long)IDAtoFlat(0x1710FUL);
  Sp_jLoop     = (unsigned long)IDAtoFlat(0x6A793UL);   /* offer next session */
  Sp_jExit     = (unsigned long)IDAtoFlat(0x6AE10UL);   /* abort -> exit weekend */

  Sp_pGridReady = (unsigned long)IDACodeReftoDataRef(0x6AD19UL); /* byte_0_179648 */
  Sp_pLaps      = (unsigned long)IDACodeReftoDataRef(0x17129UL); /* w_LapsInThisRace 0x179638 */
  Sp_pLapBound  = (unsigned long)IDACodeReftoDataRef(0x6AD66UL); /* dword_0_17963A */
  Sp_pTimeCap   = (unsigned long)IDACodeReftoDataRef(0x6AD71UL); /* word_0_179642 */
  Sp_pRaceMode  = (unsigned long)IDACodeReftoDataRef(0x6AD79UL); /* b_RaceMode 0x17964A */
  Sp_pMask      = (unsigned long)IDACodeReftoDataRef(0x6AD54UL); /* byte_0_179646 */
  Sp_pAbort     = (unsigned long)IDACodeReftoDataRef(0x6ADE7UL); /* byte_0_174E4E */

  if (laps < 1)   laps = 1;
  if (laps > 255) laps = 255;
  Sp_Laps = (unsigned long)laps;

  if (*te == warmup && Sp_pGridReady && Sp_pLaps && Sp_pLapBound && Sp_pTimeCap &&
      Sp_pRaceMode && Sp_pMask && Sp_pAbort) {
    *te = (unsigned long)SprintCave;
    sprintf(strbuf, "- Sessions: sprint armed (warmup slot -> %d-lap race)\n", laps); LogLine(strbuf);
  } else {
    LogLine("- Sessions: sprint dispatch/data-ref mismatch; sprint DISABLED\n");
  }
}

void SessionsInit(void)
{
  unsigned char  mask, final;
  unsigned char *p1, *p2, *p3;
  int sprint, laps = 0;

  if (!OverrideWeekend(&mask)) return;             /* no [Weekend] section -> stock 0xF3 */

  sprint = OverrideSprint(&laps);
  /* control bits 6/7 always on; a sprint needs the warmup slot (bit4) offered */
  final = (unsigned char)(mask | 0xC0 | (sprint ? 0x10 : 0));

  p1 = (unsigned char *)IDAtoFlat(SS_INIT);
  p2 = (unsigned char *)IDAtoFlat(SS_CHK1);
  p3 = (unsigned char *)IDAtoFlat(SS_CHK2);

  /* all-or-nothing verify: opcode bytes + the 0xF3 immediate at each site */
  if (p1[0]!=0xC6 || p1[1]!=0x05 || p1[6]!=0xF3 ||
      p2[0]!=0x80 || p2[1]!=0x3D || p2[6]!=0xF3 ||
      p3[0]!=0x80 || p3[1]!=0x3D || p3[6]!=0xF3) {
    LogLine("- Sessions: mask-literal site mismatch; [Weekend] DISABLED\n");
    return;
  }

  p1[6] = final; p2[6] = final; p3[6] = final;

  sprintf(strbuf, "- Sessions: weekend mask 0x%02X (FriP%d FriQ%d SatP%d SatQ%d Warm%d Race%d)\n",
          final, (final>>0)&1, (final>>1)&1, (final>>2)&1, (final>>3)&1, (final>>4)&1, (final>>5)&1);
  LogLine(strbuf);
  if (!((final >> 5) & 1))
    LogLine("- Sessions: RACE disabled -- championship rounds go unscored; for test weekends only\n");

  if (sprint) SprintArm(laps);      /* warmup slot -> a 2nd (shorter) race */
}
