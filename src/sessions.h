#ifndef SESSIONS_H
#define SESSIONS_H

/* Phase 2a: enable/disable weekend sessions from the [Weekend] override section.
   GP2's weekend is a bitmask menu loop -- byte_0_179646 (init 0xF3 @0x6A770) gates which
   sessions the Grand Prix menu offers: bit 0 Friday practice, 1 Friday qualifying,
   2 Saturday practice, 3 Saturday qualifying, 4 warmup, 5 race; bits 6/7 = menu controls.
   Clearing a bit removes that session (the menu gates on the mask; no menu-struct patch
   needed). We patch the init immediate to the configured mask (control bits 6/7 forced on
   so the menu can still exit), plus the two "fresh weekend" checks
   (cmp byte_0_179646,0F3h @0x6B24C / @0x6B3EA) to the same value, so a reduced weekend is
   not misread as already-in-progress. Disabling all qualifying leaves the default grid
   order; disabling the race leaves an un-scored (test) weekend. See docs/gp2lap/
   session-sequence.md and the season-realism roadmap (in the vault). */

void SessionsInit(void);   /* apply the [Weekend] session mask (3-immediate patch) at init */

#endif /* SESSIONS_H */
