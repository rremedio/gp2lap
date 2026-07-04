#ifndef CARHELMET_H
#define CARHELMET_H

/* Per-driver custom helmet textures. Stock GP2's HELMTS1.JAM holds 28 helmet
   designs (jam-ids 545..572, each a 74x15 REGION of a shared 256-wide atlas); a driver's
   helmet = t_CarNrTeamTab[carId-1] + 545, and added drivers default to helmet #0. A per-seat
   `Helmet1/Helmet2 = <74x15 8bpp BMP>` overrides that driver's helmet.

   Registering a NEW jam-id scrambles the UV (the per-face UV table is descriptor/flag-driven and
   only maps to a stock slot's declared texel space), so instead -- exactly like cartex for car
   bodies -- we OVERWRITE the driver's resolved stock helmet slot IN PLACE, per-draw, keyed by
   carId, restoring the stock snapshot for a non-override driver. The descriptor (dims/flags/UV)
   is untouched, so the UV stays correct.

   Hook = re-point `call sub_41D50` @0x440B1 (per textured polygon); our stub runs the overwrite
   BEFORE sub_41D50 builds its palette LUT. Detection: word_18330C==0x221 and word_18330A in
   545..572. Three gotchas the JAM decode surfaced: the 74x15 region lives at 256 STRIDE in the
   shared atlas (write row-by-row, not contiguous); BMP rows pad to 4 bytes (74->76, our loader
   skips it, else the helmet shears + bleeds into the next); and the palette is repointed at a
   per-car buffer (never the fixed engine pool -> no overflow / no slot-sharing contention when
   several added drivers land on slot 545). */

void CarHelmetInit(void);   /* init: collect Helmet BMPs, install the per-draw overwrite hook (0x440B1) */

#endif /* CARHELMET_H */
