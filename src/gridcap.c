#include <stdio.h>
#include "gridcap.h"
#include "cfgmain.h"        /* GetCfgULong */
#include "miscahf.h"        /* IDAtoFlat */
#include "basiclog.h"       /* LogLine / strbuf */

/* Small-grid support: retarget the two hard "26" literals in the race-grid
   finaliser (sub_0_2C19D) to min(N,26), where N = w_NumCars_26_ at finalise
   entry. See gridcap.h for the rationale and gp2def of the two sites. */

/* IDA addresses of the two patch sites (write via IDAtoFlat). */
#define GC_COPYCNT  0x2C1ADUL    /* mov ecx,26            B9 1A 00 00 00      (5) */
#define GC_FIELDWR  0x2C1C7UL    /* mov w_NumCars_26_,26  66 C7 05 54 CB 0C 00 1A 00 (9) */

/* the asm stubs (lammcall.asm); each reads w_NumCars_26_ via ds:_pNumCars and
   t_GridTable via ds:_pCarIDs, both already resolved by WWPatchDataHooks. */
extern void GridCapCopyCount(void);   /* sets ECX = min(w_NumCars_26_,26)        */
extern void GridCapFinalize(void);    /* field = min(.,26); zero grid tail[.. 25] */

/* exact stock opcode bytes we require before patching anything. */
static const unsigned char stockCopy[5] =
    { 0xB9, 0x1A, 0x00, 0x00, 0x00 };                       /* mov ecx, 26       */
static const unsigned char stockField[9] =
    { 0x66, 0xC7, 0x05, 0x54, 0xCB, 0x0C, 0x00, 0x1A, 0x00 };/* mov [CCB54],26   */

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
  unsigned char *pc;       /* 0x2C1AD copy-loop count */
  unsigned char *pf;       /* 0x2C1C7 field-size write */

  cfg = GetCfgULong("AllowSmallGrid");
  if (!cfg || !*cfg) return;          /* DEFAULT OFF: do nothing, stock behaviour */

  pc = IDAtoFlat(GC_COPYCNT);
  pf = IDAtoFlat(GC_FIELDWR);

  /* all-or-nothing verify: confirm BOTH sites before writing EITHER. */
  if (!BytesMatch(pc, stockCopy, 5)) {
    LogLine("- SmallGrid: site 0x2C1AD mismatch; DISABLED\n");
    return;
  }
  if (!BytesMatch(pf, stockField, 9)) {
    LogLine("- SmallGrid: site 0x2C1C7 mismatch; DISABLED\n");
    return;
  }

  /* 0x2C1AD: mov ecx,26  ->  call GridCapCopyCount  (exact 5-byte replacement) */
  pc[0] = 0xE8;
  *(long *)(pc + 1) =
      (long)((unsigned long)GridCapCopyCount - (unsigned long)(pc + 5));

  /* 0x2C1C7: mov w_NumCars_26_,26 (9 bytes)  ->  call GridCapFinalize + 4*NOP */
  pf[0] = 0xE8;
  *(long *)(pf + 1) =
      (long)((unsigned long)GridCapFinalize - (unsigned long)(pf + 5));
  pf[5] = 0x90;
  pf[6] = 0x90;
  pf[7] = 0x90;
  pf[8] = 0x90;

  LogLine("- SmallGrid: enabled\n");
}
