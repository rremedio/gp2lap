#ifndef MAGICD_H
#define MAGICD_H

/* Phase 1a: load per-track "magic data" (.m2d files) into GP2's 24 per-track-slot
   tuning tables at init. The SeasonOverrides file references one .m2d per track slot:

     [Track 3]
     MagicData = magic/spa.m2d     ; path relative to the override file

   Each .m2d = 24 decimal u16 lines (table order 1..24), the gp2-workshop format.
   Values are written into slot (N-1) of each of the 24 tables using the SAME byte
   layout gp2-workshop uses (byte-exact .m2d compatibility, incl. the legacy community
   stride-6 layout of tables 14-17/19-21). The magic block (IDA 0xD57F4..0xD5C3A) sits
   BELOW the savegame block and persists, so a single write at init covers every race
   in the run (the pit-geometry tables 14-21 are consumed at track load, and init runs
   before any track loads, so they are in place in time).

   MUST run AFTER CatchTrackInfos (which also writes magic table 1 = tyre wear from the
   track file); a per-slot .m2d then takes precedence for that slot. See
   docs/magic-data.md and the season-realism roadmap (both in the vault). */

void MagicDataInit(void);   /* apply every [Track N] MagicData .m2d referenced in the override */

#endif /* MAGICD_H */
