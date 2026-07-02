#include <stdio.h>
#include "sessions.h"
#include "override.h"       /* OverrideWeekend */
#include "miscahf.h"        /* IDAtoFlat */
#include "basiclog.h"       /* LogLine / strbuf */

/* The three sites carrying the 0xF3 session-mask literal (immediate byte at instr+6; the
   disp32 in between is the relocated &byte_0_179646, so it is NOT verified). */
#define SS_INIT 0x6A76AUL   /* C6 05 <disp32> F3   mov byte_0_179646, 0F3h  (init)         */
#define SS_CHK1 0x6B246UL   /* 80 3D <disp32> F3   cmp byte_0_179646, 0F3h  (fresh check)  */
#define SS_CHK2 0x6B3E4UL   /* 80 3D <disp32> F3   cmp byte_0_179646, 0F3h  (fresh check)  */

void SessionsInit(void)
{
  unsigned char  mask, final;
  unsigned char *p1, *p2, *p3;

  if (!OverrideWeekend(&mask)) return;             /* no [Weekend] section -> stock 0xF3 */

  final = (unsigned char)(mask | 0xC0);            /* force the menu control bits 6/7 on */

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
}
