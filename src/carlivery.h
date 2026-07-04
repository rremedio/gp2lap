#ifndef CARLIVERY_H
#define CARLIVERY_H

/* Base car-body liveries for override-added teams 15-20 (stock GP2 only ships
   JAM atlases for the 14 stock teams). Each added team supplies a `Livery = <256x164x8 BMP>`;
   GP2Lap registers it as a new atlas under a free jam-id (788..793 when the jam-id cap has been
   raised -- the collision-proof zone above the stock 786 max; else a 573..578 fallback) and remaps
   the team->atlas resolver so the added team draws its own body.

   Registration = the "clone-descriptor" path: copy team-14's (jam 544) 32-byte descriptor +
   its two attr-table bytes, then point the copy's image at a GP2Lap-owned index buffer and its
   palette at a GP2Lap-owned palette buffer (both filled from the BMP, same layout as the
   per-car cartex path). No engine image/palette pool is used, so nothing the engine re-decrypts
   per weekend can corrupt it -- but the jam-id -> descriptor MAP is rebuilt per weekend (the
   loader wipes it), so registration is IDEMPOTENT at session start (re-register only when the
   map slot reads 0xFFFF). The remap is one call-site re-point (sub_677D0 @0x67A5F) to an asm
   stub that runs the stock resolver then, for a team 15-20 with a registered livery, overwrites
   word_18330A with its jam-id. Shape stays team-14 (the safe default via the stock clamp).

   Per-CAR liveries for the added teams (Car1/Car2) reuse the per-car cartex path (its 14-team cap lifted to 20). */

void CarLiveryInit(void);   /* init: load BMPs, install the sub_677D0 remap, first register pass */
void CarLiverySOS(void);    /* session start: idempotent re-register (map is wiped per weekend) */

#endif /* CARLIVERY_H */
