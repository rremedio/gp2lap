#ifndef _GRIDCAP_H
#define _GRIDCAP_H

/* 2026: small-grid support. Lets GP2 race a grid of FEWER than 26 cars when more
   than two drivers are disabled (otherwise GP2 forces the field back to 26 and
   conjures phantom / duplicate cars from stale grid-table tail entries).

   GATED behind the cfg key AllowSmallGrid (ULONGTYPE, default 0). When unset the
   module patches NOTHING and stock behaviour is byte-for-byte unchanged.

   The race grid is finalised by sub_0_2C19D (IDA 0x2C19D), which runs AFTER
   sub_0_14EB4 has set w_NumCars_26_ (IDA 0xCCB54, 16-bit) to N = the valid-carId
   count for this session. The stock finalise then (a) copies t_CarStartOrder ->
   t_GridTable (IDA 0xCCB58) for a HARD 26 entries and (b) writes a HARD 26 into
   w_NumCars_26_. We retarget just those two literals to min(N,26):

     0x2C1AD  mov ecx,26              -> call GridCapCopyCount  (ecx = min(N,26))
     0x2C1C7  mov w_NumCars_26_,26    -> call GridCapFinalize   (field = min(N,26)
                                         AND zero t_GridTable[field..25])

   min(N,26) keeps stock identical (N>=26 -> 26; zero-loop empties nothing), so
   the patch is a no-op for a full field and only ever SHRINKS the grid. Zeroing
   the grid-table tail turns the would-be phantom slots into empty (carId 0)
   entries in t_CarStructs.

   Those carId-0 tail cars are then retired the SAME way, at the SAME point, that
   non-race sessions retire their inactive cars: a placement-loop detour over
   sub_0_2C785 @0x2C7A1 (the "test b_RaceMode,0FFh / jns L_NoRace" head) runs the
   engine's own SILENT retire idiom on any RACE car with carId 0 -- rCarRetires plus
   set field_5E bit1 to suppress the race-only retirement announcer -- BEFORE the
   session goes live. So the phantoms never render, never classify into the race
   results, and never trigger an on-screen "<driver> is out of the race" message.
   (Placement is bounded by a literal 26, so it reaches the tail regardless of the
   shrunk field.) An earlier per-frame EOFHook flags_90 retire was REPLACED by this:
   it retired too late (results ghosts) and the un-suppressed flag flips fired the
   announcer every frame (the "retiring" message storm).

   All-or-nothing: all three sites (two finalise literals + the placement host) are
   verified against their exact stock opcode bytes before ANY is written; a mismatch
   disables the whole feature. Installed once at startup -> dynrec-safe. */

void GridCapInit(void);       /* read AllowSmallGrid; verify + install the grid patches */

#endif /* _GRIDCAP_H */
