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

   Those carId-0 tail cars are then hidden the SAME way non-race sessions hide
   their inactive cars: per-car flags_90 |= 0x80|0x20 (invisible + out-of-cockpit),
   the universal "car not present" gate honoured by render/AI/standings/runners/
   end-of-race. GridCapHideTail() does this each frame from EOFHook (pure data; the
   position-table pipeline / C9E40 are NOT the field pin and are left untouched).

   All-or-nothing: both finalise sites are verified against their exact stock opcode
   bytes before EITHER is written; any mismatch disables the whole feature. */

void GridCapInit(void);       /* read AllowSmallGrid; verify + install finalise patches */
void GridCapHideTail(void);   /* per-frame (EOFHook): retire the carId-0 grid tail */

#endif /* _GRIDCAP_H */
