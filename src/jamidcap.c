#include <stdio.h>          /* sprintf */
#include <stdlib.h>
#include <string.h>
#include "jamidcap.h"
#include "miscahf.h"        /* IDAtoFlat */
#include "basiclog.h"       /* LogLine / strbuf */

#define JIC_STOCKCAP 0x314UL              /* 788 entries */
#define JIC_WORDSZ   (JIC_STOCKCAP * 2)   /* word_4C4EA8 stock byte size = 0x628 */

unsigned long JamIdCapRaised = 0;

/* which of the three arrays a displacement site points at */
enum { A_WORD = 0, A_54D0 = 1, A_57E4 = 2 };

/* every displacement site that embeds an array base: {IDA code addr, operand byte offset, array}.
   word_4C4EA8 sites carry disp32 @ +4; the byte-array sites carry disp32 @ +2. Enumerated from
   the annotated listing (grep of the base displacements A8 4E 4C 00 / D0 54 4C 00 / E4 57 4C 00). */
static const struct { unsigned long addr; int off; int arr; } g_disp[] = {
  /* word_4C4EA8 -- 11 sites, disp32 @ +4 (init loop 0x70AE4, register 0x70B71, reads elsewhere) */
  { 0x41D68UL, 4, A_WORD }, { 0x4A25CUL, 4, A_WORD }, { 0x4A299UL, 4, A_WORD },
  { 0x55C63UL, 4, A_WORD }, { 0x55C73UL, 4, A_WORD }, { 0x64924UL, 4, A_WORD },
  { 0x65D8BUL, 4, A_WORD }, { 0x70AE4UL, 4, A_WORD }, { 0x70B71UL, 4, A_WORD },
  { 0x7656EUL, 4, A_WORD }, { 0x7657AUL, 4, A_WORD },
  /* unk_4C54D0 -- 4 sites, disp32 @ +2 (init 0x70AEE, register 0x70B9F, reads 0x55DE2/0x64F68) */
  { 0x55DE2UL, 2, A_54D0 }, { 0x64F68UL, 2, A_54D0 }, { 0x70AEEUL, 2, A_54D0 },
  { 0x70B9FUL, 2, A_54D0 },
  /* unk_4C57E4 -- 3 sites, disp32 @ +2 (init 0x70AF5, register 0x70BBA, read 0x55DB1) */
  { 0x55DB1UL, 2, A_57E4 }, { 0x70AF5UL, 2, A_57E4 }, { 0x70BBAUL, 2, A_57E4 }
};
#define JIC_NDISP (int)(sizeof(g_disp) / sizeof(g_disp[0]))

void JamIdCapInit(void)
{
  unsigned long oldBase[3], newBase[3];
  unsigned long need;
  unsigned char *buf;
  int i;

  /* read the stock array bases straight from their init-loop operands (no ddelta needed) */
  oldBase[A_WORD] = *(unsigned long *)(IDAtoFlat(0x70AE4UL) + 4);
  oldBase[A_54D0] = *(unsigned long *)(IDAtoFlat(0x70AEEUL) + 2);
  oldBase[A_57E4] = *(unsigned long *)(IDAtoFlat(0x70AF5UL) + 2);

  /* sanity: the three arrays must be stock-adjacent (word[788] then two byte[788]) */
  if (oldBase[A_54D0] != oldBase[A_WORD] + JIC_WORDSZ ||
      oldBase[A_57E4] != oldBase[A_54D0] + JIC_STOCKCAP) {
    sprintf(strbuf, "- JamIdCap: arrays not stock-adjacent (0x%08lX/0x%08lX/0x%08lX); DISABLED\n",
            oldBase[A_WORD], oldBase[A_54D0], oldBase[A_57E4]);
    LogLine(strbuf);
    return;
  }

  /* verify every displacement operand currently reads its array's stock base */
  for (i = 0; i < JIC_NDISP; i++) {
    unsigned long v = *(unsigned long *)(IDAtoFlat(g_disp[i].addr) + g_disp[i].off);
    if (v != oldBase[g_disp[i].arr]) {
      sprintf(strbuf, "- JamIdCap: disp site 0x%lX = 0x%08lX (want 0x%08lX); DISABLED\n",
              g_disp[i].addr, v, oldBase[g_disp[i].arr]);
      LogLine(strbuf);
      return;
    }
  }

  /* verify the 4 cap immediates are the stock 0x314 (the register guard 0x70B68 is a 16-bit imm) */
  if (*(unsigned long  *)(IDAtoFlat(0x4A250UL) + 2) != JIC_STOCKCAP ||
      *(unsigned long  *)(IDAtoFlat(0x4A28DUL) + 2) != JIC_STOCKCAP ||
      *(unsigned long  *)(IDAtoFlat(0x70AFDUL) + 2) != JIC_STOCKCAP ||
      *(unsigned short *)(IDAtoFlat(0x70B68UL) + 3) != (unsigned short)JIC_STOCKCAP) {
    LogLine("- JamIdCap: a cap immediate != 0x314; DISABLED\n");
    return;
  }

  /* one buffer: word[NEWCAP] then byte[NEWCAP] then byte[NEWCAP] */
  need = (unsigned long)JIC_NEWCAP * 2 + (unsigned long)JIC_NEWCAP + (unsigned long)JIC_NEWCAP;
  buf = (unsigned char *)malloc(need);
  if (!buf) {
    LogLine("- JamIdCap: buffer alloc FAILED; staying on the stock 788 cap\n");
    return;
  }
  newBase[A_WORD] = (unsigned long)buf;
  newBase[A_54D0] = (unsigned long)buf + (unsigned long)JIC_NEWCAP * 2;
  newBase[A_57E4] = newBase[A_54D0] + (unsigned long)JIC_NEWCAP;

  /* pre-init to the stock loop's values so nothing reads garbage before the first
     per-weekend sub_70AB6 rebuilds the map (word 0xFFFF = free, byte attrs 0x10) */
  memset((void *)newBase[A_WORD], 0xFF, (size_t)JIC_NEWCAP * 2);
  memset((void *)newBase[A_54D0], 0x10, (size_t)JIC_NEWCAP);
  memset((void *)newBase[A_57E4], 0x10, (size_t)JIC_NEWCAP);

  /* re-point every displacement operand to its array's new base */
  for (i = 0; i < JIC_NDISP; i++)
    *(unsigned long *)(IDAtoFlat(g_disp[i].addr) + g_disp[i].off) = newBase[g_disp[i].arr];

  /* raise the 4 cap immediates (three 32-bit, one 16-bit) to NEWCAP */
  *(unsigned long  *)(IDAtoFlat(0x4A250UL) + 2) = (unsigned long)JIC_NEWCAP;
  *(unsigned long  *)(IDAtoFlat(0x4A28DUL) + 2) = (unsigned long)JIC_NEWCAP;
  *(unsigned long  *)(IDAtoFlat(0x70AFDUL) + 2) = (unsigned long)JIC_NEWCAP;
  *(unsigned short *)(IDAtoFlat(0x70B68UL) + 3) = (unsigned short)JIC_NEWCAP;

  JamIdCapRaised = 1;
  sprintf(strbuf,
    "- JamIdCap: cap 788 -> %d; map relocated 0x%08lX -> 0x%08lX (%lu B, %d sites)\n",
    JIC_NEWCAP, oldBase[A_WORD], newBase[A_WORD], need, JIC_NDISP);
  LogLine(strbuf);
}
