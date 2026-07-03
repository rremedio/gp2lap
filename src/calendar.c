#include <stdio.h>
#include "calendar.h"
#include "override.h"       /* OverrideCalendarRounds */
#include "miscahf.h"        /* IDACodeReftoDataRef / IDAtoFlat */
#include "basiclog.h"       /* LogLine / strbuf */

/* disp32 operand whose relocated value = &dword_0_1795F8 (mov ecx,[..] @0x8453F+2),
   the championship round count (stock 16). */
#define CAL_COUNTREF 0x84541UL

/* Championship-results grid literal-16 loop bounds (the driver x round cell matrix).
   Each is a `mov <reg>,10h` whose imm dword = 16; we drop the low byte to N so only the
   first N round columns draw. The header pair is COUPLED: 0x83904 is the column count and
   0x8390B is the label base (label id = 1599 + (16-ecx)), so both must move to N together
   or the round-number headers shift. 0x83A6C (data cells) is standalone. The standings
   LIST / points / next-race screens read dword_0_1795F8 directly and self-limit -> no patch.
   {IDA instr addr, expected opcode}; the 0x10 immediate sits at instr+1. */
#define RG_HDR_CNT  0x83904UL   /* B9 mov ecx,10h  header column count  */
#define RG_HDR_BASE 0x8390BUL   /* BE mov esi,10h  header label base    */
#define RG_DAT_CNT  0x83A6CUL   /* B9 mov ecx,10h  data column count    */

/* verify a `mov reg,10h` site (opcode + imm32==16); return the flat instr ptr or 0 */
static unsigned char *RgSite(unsigned long ida, unsigned char opcode)
{
  unsigned char *p = (unsigned char *)IDAtoFlat(ida);
  if (p && p[0]==opcode && p[1]==0x10 && p[2]==0 && p[3]==0 && p[4]==0) return p;
  return 0;
}

void CalendarInit(void)
{
  unsigned long *countAddr = (unsigned long *)IDACodeReftoDataRef(CAL_COUNTREF);
  int rounds = OverrideCalendarRounds();

  if (rounds <= 0) return;                                       /* no [Calendar] Rounds */

  if (!countAddr) { LogLine("- Calendar: round-count global unresolved; DISABLED\n"); return; }

  /* sanity: the round count must read as the stock 16 before we touch it -- guards against
     a mis-resolved address corrupting the save block. */
  if (*countAddr != 16) {
    sprintf(strbuf, "- Calendar: stock round count reads %lu (want 16); DISABLED\n", *countAddr);
    LogLine(strbuf);
    return;
  }

  *countAddr = (unsigned long)rounds;   /* season now ends after round `rounds`; slots N..15 skipped */

  sprintf(strbuf, "- Calendar: %d-round season (slots 1..%d; %d skipped)\n",
          rounds, rounds, 16 - rounds);
  LogLine(strbuf);

  /* bound the results-grid display loops to N (all-or-nothing verify first) */
  {
    unsigned char *hc = RgSite(RG_HDR_CNT,  0xB9);
    unsigned char *hb = RgSite(RG_HDR_BASE, 0xBE);
    unsigned char *dc = RgSite(RG_DAT_CNT,  0xB9);
    if (hc && hb && dc) {
      hc[1] = (unsigned char)rounds;    /* header column count */
      hb[1] = (unsigned char)rounds;    /* header label base (coupled with hc)   */
      dc[1] = (unsigned char)rounds;    /* data-cell column count */
      LogLine("- Calendar: results grid bounded to season length\n");
    } else {
      LogLine("- Calendar: results-grid loop site mismatch; grid still shows 16 columns\n");
    }
  }
}
