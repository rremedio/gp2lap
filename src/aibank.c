#include <stdio.h>          // sprintf
#include "aibank.h"
#include "miscahf.h"        // IDAtoFlat, IDACodeReftoDataRef
#include "cfgmain.h"        // GetCfgULong
#include "basiclog.h"       // LogLine / strbuf

/* On a banked track (track cmd 0xAD -> per-segment banking tseg+0x26) GP2's cars sit at the right
   HEIGHT but don't ROLL ("floating on a flat track"). The in-race car model is rotated by 3
   angles (mapped empirically): car+0x15E = YAW, T_TrackDataOfs = PITCH, and word_173F8C = ROLL.
   word_173F8C is written at IDA 0x67C2B (= bp = 173FC8); the segment banking is never folded in.

   Fix: replace that store with a stub that writes bp (original) then ADDS the car's current
   segment banking (seg = [esi+10h], banking = [seg+0x26]) into word_173F8C, so the body leans
   onto the banked surface. Sign '+' and identity scale both verified in-game (banking is already
   in model-angle units). Self-disables on flat track (banking 0).

   Scope: this is the per-car drawer used by EXTERNAL / TV / replay views. Cockpit view renders
   opponent cars through a different path (not hooked here). See docs/track-banking-cmd-0xAD.md. */

unsigned long AIBankingRoll = 0;            /* 0 = off */
unsigned long pW173F8C      = 0;            /* flat &word_173F8C (drawn-model roll slot) */
unsigned long pD40EC        = 0;            /* flat &word_D40EC (cockpit camera roll) */

extern void BankRoll(void);                 /* asm: drawn-car-model roll (patched over 0x67C2B) */
extern void CockpitRoll(void);              /* asm: cockpit camera roll  (patched over 0x383B9) */

void AiBankingInit(void)
{
  unsigned long *p;
  unsigned char *site;

  p = GetCfgULong("AIBankingRoll");
  if (!p || !*p) {                          /* feature off -> stock untouched */
    LogLine("- AIBankingRoll: off\n");
    return;
  }

  /* roll-slot write in the per-car drawer:  00067C2B  66 89 2D 8C 3F 17 00  mov word_173F8C, bp */
  site = (unsigned char *)IDAtoFlat(0x67C2B);
  if (site[0] != 0x66 || site[1] != 0x89 || site[2] != 0x2D) {
    sprintf(strbuf, "- AIBankingRoll: off (opcode mismatch at 0x67C2B: %02X %02X %02X)\n",
            site[0], site[1], site[2]);
    LogLine(strbuf);
    return;
  }
  pW173F8C = (unsigned long)IDACodeReftoDataRef(0x67C2E);       /* &word_173F8C */

  /* replace the 7-byte "mov word_173F8C, bp" with "call BankRoll" (5) + 2 NOPs */
  site[0] = 0xE8;                                               /* call rel32 */
  *(long *)(site + 1) = (long)((unsigned long)BankRoll - (unsigned long)(site + 5));
  site[5] = 0x90;
  site[6] = 0x90;

  /* COCKPIT (first-person) view roll. The cockpit camera already has a roll DOF fed by
     word_173FC8 (= elevation + 1x banking) -> word_D40EC -> word_F9CF0 -> roll matrix; but that
     native roll is subtle, so the view reads as "level" while the player car model (rolled by
     word_173F8C = 173FC8 + banking, our other hook) visibly banks. Make the view roll the SAME
     as the model: at 0x383B9 in sub_0_38365 (`mov word_D40EC, ax`, AX = word_173FC8, EDI = the
     player car's segment) add the segment banking so word_D40EC = 173FC8 + banking. Cockpit-only:
     TV/external camera handlers zero word_F9CF0 afterward. */
  site = (unsigned char *)IDAtoFlat(0x383B9);
  if (site[0] == 0x66 && site[1] == 0xA3) {
    pD40EC = (unsigned long)IDACodeReftoDataRef(0x383BB);       /* &word_D40EC */
    site[0] = 0xE8;                                             /* call rel32 */
    *(long *)(site + 1) = (long)((unsigned long)CockpitRoll - (unsigned long)(site + 5));
    site[5] = 0x90;                                             /* cockpit view roll armed (silent) */
  } else {
    sprintf(strbuf, "- AIBankingRoll: cockpit view roll unavailable (opcode mismatch at 0x383B9:"
            " %02X %02X)\n", site[0], site[1]);
    LogLine(strbuf);
  }

  AIBankingRoll = 1;
  LogLine("- AIBankingRoll: on\n");
}
