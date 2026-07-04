#ifndef JAMIDCAP_H
#define JAMIDCAP_H

/* Raise GP2's jam-id ceiling from 788 (0x314) to JIC_NEWCAP by relocating the three
   packed jam-id-indexed arrays -- the id->descriptor-offset map word_4C4EA8 and the two
   per-id byte-attr arrays unk_4C54D0 / unk_4C57E4 -- to a GP2Lap buffer, and bumping the
   4 cap immediates that bound them.

   Ids 0..787 stay byte-identical to stock; the change only ADDS usable ids 788..NEWCAP-1.
   That band is collision-proof by construction: the STOCK exe hangs on a jam-id >= 788
   (the register path sub_70B07 does `cmp dx,314h; jb ok` with fall-through `jmp $`), so no
   track -- stock or custom -- can ship a JAM there without hanging un-modded GP2. Foundational
   for any GP2Lap atlas registration (e.g. added-team liveries at 788+).

   The map arrays are transient (rebuilt every weekend by sub_70AB6's init loop, which now uses
   the relocated bases + raised cap), so no snapshot and no save-game impact. Applied once at
   attach. Bails (leaves stock intact) if any operand/immediate fails its opcode check.
   See docs/plans/2026-07-03-jam-id-cap-raise.md (in the vault). */

#define JIC_NEWCAP 1024   /* 0x400; MUST be <= 0xFFFF -- the register guard `cmp dx,..` is 16-bit */

extern unsigned long JamIdCapRaised;   /* 0 until relocation succeeds; consumers gate the high id block on this */

void JamIdCapInit(void);   /* relocate the map + raise the cap once (call at attach, AHFAfterGp2Init) */

#endif /* JAMIDCAP_H */
