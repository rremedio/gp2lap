#ifndef CALENDAR_H
#define CALENDAR_H

/* Phase 3a: variable-length championship calendar. The SeasonOverrides file may carry a
   [Calendar] section shortening the season to the first N of the 16 calendar slots:

     [Calendar]
     Rounds = 8      ; championship runs slots 1..8; slots 9..16 are skipped

   Track ORDER/content stays stock -- each slot's track is chosen via its track file plus
   magic-data / per-track override (Phase 1), so reordering the calendar was unnecessary.

   GP2 stores the round count as dword_0_1795F8 @0x1795F8 (stock 16), a static constant with
   NO runtime writer, read by end-of-season (0x6B405), the standings column bound (0x9532B),
   the champ-matrix search (0x8453F) and the round-vs-count check (0x95712). Setting it to N
   is enough end-to-end for the championship LOGIC: no structure overflows for N<16 (the
   40x16 finish matrix word_17968D simply uses columns 0..N-1). Leaving t_trackid? @0x179600
   stock (identity: round r -> track r) keeps the season a normal prefix of the default 16.

   The DISPLAY pages (track-selection map, season-results columns) loop over a hard-coded 16
   independent of this count, so slots N..15 would still render; SessionsCalendar bounds those
   loops separately (see calendar.c UI patches). >16 rounds is out of scope (3b: by-value
   relocation of the finish matrix + wider standings screen).

   dword_0_1795F8 sits in the savegame block, so a game SAVED under a shortened season restores
   it on load; an older stock save loaded afterwards keeps its own 16-round season (a season in
   progress cannot be retroactively reshaped) -- hence a one-time init write, no re-apply. */

void CalendarInit(void);   /* apply [Calendar] Rounds (round count + UI-loop bounds), if set */

#endif /* CALENDAR_H */
