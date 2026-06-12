#include <stdio.h>
#include <string.h>
#include "aiaccel.h"
#include "miscahf.h"        // IDAtoFlat
#include "cfgmain.h"        // GetCfgULong
#include "basiclog.h"       // LogLine / strbuf

/* The AI accel apply (sub_22AB0, IDA 0x22F02-0x22F58) scales accel by car.enginePower (+0xA2)
   LINEARLY at every speed: word_D41FC[bucket] x D6046 >>2 x enginePower >>14 x throttle ...
   The D41FC table is built (sub_2EE0F) from a FIXED dummy car (enginePower = 0x4000), so the
   table is HP-independent and the only HP term is this linear multiply. The player's real physics
   is traction-limited at low speed (nearly HP-independent there), so lowering a carset's HP cuts
   AI launch / slow-corner-exit accel far more than the human's -> the field looks broken at starts.

   Fix: replace the raw enginePower with a speed-faded effective power
        effHP = 0x4000 + (enginePower - 0x4000) * min(vb, K) / K
   where vb = speed bucket and K = AILaunchFadeBuckets. effHP = 0x4000 (the table's reference
   power -> factor 1.0, no penalty) at a standstill, ramping to the real enginePower by speed K.
   Self-disabling at stock HP (enginePower ~= 0x4000 -> effHP ~= enginePower everywhere).

   The maths/scaling live in the asm stub AccelEffHP (lammcall.asm); here we read cfg, compute K,
   and install the trampoline over the 13-byte enginePower multiply at IDA 0x22F25. */

#define AEF_MAXBUCKET 358      /* 0x166 = (0x16600000 >> 20): the top-speed bucket */
#define AEF_DEFKMH    128      /* default crossover speed (vb runs ~1:1 with km/h) */

unsigned long AILowHpLaunchFix    = 0;             /* 0 = off */
unsigned long AILaunchFadeBuckets = AEF_DEFKMH;    /* K, read directly by the asm stub */

extern void AccelEffHP(void);  /* asm read-site stub in lammcall.asm */

void AiLaunchFixInit(void)
{
  unsigned long *p, kmh;
  unsigned char *site;

  p = GetCfgULong("AILowHpLaunchFix");
  if (!p || !*p) return;                           /* feature off -> stock untouched */

  kmh = AEF_DEFKMH;
  p = GetCfgULong("AILaunchFadeKmh");
  if (p && *p) kmh = *p;
  if (kmh < 1) kmh = 1;                             /* K >= 1: the stub divides by it */
  if (kmh > AEF_MAXBUCKET) kmh = AEF_MAXBUCKET;
  AILaunchFadeBuckets = kmh;                        /* ~1:1 km/h -> speed bucket */

  /* Replace the 13-byte enginePower multiply (IDA 0x22F25-0x22F31) with "call AccelEffHP" + NOPs.
     Stock bytes: 0F BF 96 A2 00 00 00  F7 EA  0F AC D0 0E
                  (movsx edx,[esi+0A2h]; imul edx; shrd eax,edx,0Eh) -- verify before patching. */
  site = (unsigned char *)IDAtoFlat(0x22F25);
  if (site[0]!=0x0F || site[1]!=0xBF || site[2]!=0x96 || site[3]!=0xA2 ||
      site[7]!=0xF7 || site[8]!=0xEA || site[9]!=0x0F || site[10]!=0xAC) {
    LogLine("- AILowHpLaunchFix: opcode mismatch at 0x22F25; DISABLED\n");
    return;
  }
  site[0] = 0xE8;                                                  /* call rel32 */
  *(long *)(site + 1) = (long)((unsigned long)AccelEffHP - (unsigned long)(site + 5));
  memset(site + 5, 0x90, 13 - 5);                                  /* NOP the displaced bytes */

  AILowHpLaunchFix = 1;
  sprintf(strbuf, "- AILowHpLaunchFix: ON (HP fade crossover ~%lu km/h)\n", kmh);
  LogLine(strbuf);
}
