#ifndef AIBANK_H
#define AIBANK_H

/* AI/opponent car banking-roll fix: make drawn cars roll onto banked track (cmd 0xAD). */
void AiBankingInit(void);

extern unsigned long AIBankingRoll;   /* 1 once the patch is installed */

#endif
