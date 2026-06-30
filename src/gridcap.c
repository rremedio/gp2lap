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
   N cars. Visibility is per-car: flags_90 bits 0x80 (invisible) + 0x20 (out-of-cockpit)
   are the universal "car not present" gate, honoured by every render / AI / standings /
   runners / end-of-race reader. The car-placement loop sub_0_2C785 runs in both sessions;
   NON-RACE calls rCarRetires on cars (sets those flags) so inactive ones stay hidden, but
   the RACE branch activates all 26 and never retires -> the (26-N) tail become phantoms.

   So the fix mirrors non-race, data-only (no runtime code patching -> dynrec-safe):
     1. Finaliser patches (installed once at startup): make sub_0_2C19D copy N and write
        w_NumCars_26_ = min(N,26), and zero t_GridTable[N..25] so the tail structs that
        InitCarStructs builds get carId 0.
     2. Each frame (EOFHook): retire those carId-0 tail cars (flags_90 |= 0x80|0x20),
        exactly as non-race already does. Idempotent; for a full field there is no carId-0
        tail, so it is a no-op = stock behaviour. */

/* the two finaliser call sites (CODE; IDAtoFlat OK). */
#define GC_COPYCNT  0x2C1ADUL    /* mov ecx,26            B9 1A 00 00 00            (5) */
#define GC_FIELDWR  0x2C1C7UL    /* mov w_NumCars_26_,26  66 C7 05 <disp32> 1A 00   (9) */

#define GC_HIDE_FLAGS 0xA0       /* flags_90: 0x80 invisible | 0x20 out-of-cockpit */

extern void GridCapCopyCount(void);   /* asm: ECX = min(w_NumCars_26_,26)              */
extern void GridCapFinalize(void);    /* asm: field=min(.,26); zero grid tail          */

static const unsigned char stockCopy[5] = { 0xB9, 0x1A, 0x00, 0x00, 0x00 }; /* mov ecx,26 */

static int g_armed = 0;

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
  unsigned char *pc;       /* 0x2C1AD copy-loop count  */
  unsigned char *pf;       /* 0x2C1C7 field-size write  */

  cfg = GetCfgULong("AllowSmallGrid");
  if (!cfg || !*cfg) return;          /* DEFAULT OFF: do nothing, stock behaviour */

  if (!pNumCars) { LogLine("- SmallGrid: pNumCars unresolved; DISABLED\n"); return; }

  pc = IDAtoFlat(GC_COPYCNT);
  pf = IDAtoFlat(GC_FIELDWR);

  /* all-or-nothing verify (the field-write disp32 is the RELOCATED &w_NumCars_26_, == pNumCars,
     NOT the IDA literal) */
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

  /* install the two finaliser call patches (startup -> dynrec-safe) */
  pc[0] = 0xE8;
  *(long *)(pc + 1) = (long)((unsigned long)GridCapCopyCount - (unsigned long)(pc + 5));

  pf[0] = 0xE8;
  *(long *)(pf + 1) = (long)((unsigned long)GridCapFinalize - (unsigned long)(pf + 5));
  pf[5] = 0x90; pf[6] = 0x90; pf[7] = 0x90; pf[8] = 0x90;

  g_armed = 1;
  LogLine("- SmallGrid: enabled (retire carId-0 grid tail; field = min(N,26))\n");
}

/* Called each frame from EOFHook. Retire the carId-0 phantom tail (flags_90 |= 0x80|0x20)
   exactly as non-race sessions do, so the (26-N) padding cars are invisible/absent from
   render, AI, standings, runners and the end-of-race check. Only races have a carId-0 tail
   (the finaliser zeroes it), so this is naturally race-scoped and a no-op for a full field. */
void GridCapHideTail(void)
{
  int i;
  if (!g_armed || !pCarStructs) return;
  for (i = 0; i < 26; i++) {
    GP2Car *c = &pCarStructs[i];
    if ((c->id & 0x3F) == 0)
      c->flags_90 |= GC_HIDE_FLAGS;
  }
}
