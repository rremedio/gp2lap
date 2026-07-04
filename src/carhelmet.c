#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "carhelmet.h"
#include "miscahf.h"        /* IDAtoFlat / IDACodeReftoDataRef */
#include "basiclog.h"       /* LogLine / strbuf */
#include "override.h"       /* OverrideCar / OV_MAXCAR */
#include "gp2hook.h"        /* dwTrackChecksum */

/* Phase 4a.3 (overwrite-in-place model, like cartex): a driver's helmet = a stock helmet atlas
   (jam 545..572). We NEVER register a new atlas (that scrambles the UV); instead we overwrite the
   driver's resolved stock helmet slot's image+palette IN PLACE, per-draw, keyed by carId, and
   restore the stock snapshot for a driver without an override -- exactly how cartex overrides a
   team body livery. The descriptor (dims/flags/UV) is untouched, so the UV stays correct.

   Hook = re-point `call sub_41D50` @0x440B1 (per textured polygon). Our stub runs the overwrite
   BEFORE sub_41D50 so it builds its palette LUT from our palette. Detection: word_18330C==0x221
   (set only by the helmet setup) and resolved jam word_18330A in 545..572. carId = dword_D5490
   [+0xA6]. Both stock and added drivers work uniformly (added drivers resolve to slot 545). */

#define CH_HELMBASE 545
#define CH_HELMTOP  572
#define CH_SLOTS    28                 /* helmet slots 545..572 */
#define CH_MAXCAR   64
#define CH_STRIDE   256                /* helmets are 74x15 REGIONS of a shared 256-wide atlas */

unsigned long PerCarHelmet = 0;
unsigned long HelmetTexOrig = 0;               /* chained sub_41D50 (read by the asm stub) */
extern void MyHelmetTex(void);
void __near _cdecl AHFHelmetSwap(void);        /* fwd (fp defined after it) */

/* per-car override helmet image (loaded lazily at the slot's dimensions) */
static char           s_path[CH_MAXCAR][256];
static int            s_pathSet[CH_MAXCAR];
static unsigned char *s_img[CH_MAXCAR];        /* index image (NULL until loaded) */
static int            s_remapped[CH_MAXCAR];
static unsigned char  s_carpal[CH_MAXCAR][256];
static int            s_carPalN[CH_MAXCAR];
static unsigned char *s_palbuf[CH_MAXCAR];      /* per-car palette buffer we point desc +0x0C at */

/* per-slot stock snapshot (to restore a non-override driver) + which carId currently occupies it.
   The palette is NOT copied over the engine pool (fixed 4*palSz would overflow the neighbour for a
   colour-rich texture in a small slot) -- we repoint desc +0x0C at our own buffer and just save the
   stock offset/size to put back. */
static unsigned char *s_snap[CH_SLOTS];
static int            s_snapped[CH_SLOTS];
static unsigned long  s_snapPalOff[CH_SLOTS];   /* stock descriptor +0x0C (palette offset)   */
static int            s_snapPalSz[CH_SLOTS];    /* stock descriptor +0x10 (palette size)     */
static int            s_slotCar[CH_SLOTS];      /* carId whose helmet is in the slot now (-1 = stock) */
static unsigned long  s_snapCsum = 0;

/* helmet sub-image dims (all helmets are the same size; read from the first resolved slot) */
static int s_hw = 0, s_hh = 0, s_hsz = 0;

/* GP2 globals (resolved from the same code operands cartex uses, plus the two helmet selectors) */
static unsigned char **s_pCar    = 0;  /* &dword_D5490 */
static unsigned short *s_pJamId  = 0;  /* &word_18330A (resolved jam)     */
static unsigned short *s_p18330C = 0;  /* &word_18330C (0x221 during helmet faces) */
static unsigned char  *s_pJamTab = 0;  /* &word_4C4EA8[0] */
static unsigned char  *s_pDesc   = 0;  /* &unk_184A8C[0] */
static unsigned char  *s_palBase = 0;  /* &unk_462D7C     */

/* Load a w x h 8bpp BMP into a compact w*h top-down buffer. Unlike readstream_svgabmp this
   HANDLES 4-byte row padding (helmets are 74 wide -> 76-byte rows) and the bottom-up/top-down
   row order -- a contiguous read would shear the image and bleed into the next helmet. */
static unsigned char *LoadBmpN(const char *path, int w, int h)
{
  FILE *f = fopen(path, "rb");
  unsigned char h14[14], h40[40], *buf;
  long ofs, hh; int biW, bits, comp, stride, r, topdown;
  if (!f) { sprintf(strbuf, "- CarHelmet: open FAILED '%s'\n", path); LogLine(strbuf); return 0; }
  if (fread(h14, 1, 14, f) != 14 || fread(h40, 1, 40, f) != 40 || h14[0] != 'B' || h14[1] != 'M') {
    fclose(f); sprintf(strbuf, "- CarHelmet: not a BMP '%s'\n", path); LogLine(strbuf); return 0; }
  ofs = (long)((unsigned long)h14[10] | ((unsigned long)h14[11]<<8) | ((unsigned long)h14[12]<<16) | ((unsigned long)h14[13]<<24));
  biW = (int)((unsigned long)h40[4] | ((unsigned long)h40[5]<<8) | ((unsigned long)h40[6]<<16) | ((unsigned long)h40[7]<<24));
  hh  = (long)((unsigned long)h40[8] | ((unsigned long)h40[9]<<8) | ((unsigned long)h40[10]<<16) | ((unsigned long)h40[11]<<24));
  topdown = (hh < 0); if (hh < 0) hh = -hh;
  bits = (int)((unsigned)h40[14] | ((unsigned)h40[15]<<8));
  comp = (int)((unsigned long)h40[16] | ((unsigned long)h40[17]<<8) | ((unsigned long)h40[18]<<16) | ((unsigned long)h40[19]<<24));
  if (biW != w || (int)hh != h || bits != 8 || comp != 0) {
    fclose(f); sprintf(strbuf, "- CarHelmet: bad BMP '%s' (need %dx%d x8 uncompressed)\n", path, w, h); LogLine(strbuf); return 0; }
  buf = (unsigned char *)malloc((size_t)(w * h));
  if (!buf) { fclose(f); LogLine("- CarHelmet: malloc FAILED\n"); return 0; }
  stride = (w + 3) & ~3;                                  /* padded row size (74 -> 76) */
  if (fseek(f, ofs, SEEK_SET) != 0) { free(buf); fclose(f); return 0; }
  for (r = 0; r < h; r++) {
    int dst = topdown ? r : (h - 1 - r);                  /* bottom-up BMP -> flip to top-down */
    if (fread(buf + dst * w, (size_t)w, 1, f) != 1) { free(buf); fclose(f); LogLine("- CarHelmet: BMP read short\n"); return 0; }
    if (stride > w) fseek(f, (long)(stride - w), SEEK_CUR);   /* skip row padding */
  }
  fclose(f);
  return buf;
}

/* flat palette from the BMP (distinct colours sorted -> sub0 + trailing zero), image -> indices */
static int BuildFromBmp(unsigned char *im, unsigned char *carpal, int sz)
{
  unsigned char present[256], lut[256]; int g, k, n;
  for (g = 0; g < 256; g++) present[g] = 0;
  for (k = 0; k < sz; k++) present[im[k]] = 1;
  /* Pin palette indices 0..13 to global colours 0..13 (index 0 = colour 0 = transparent) -- every
     stock helmet does this, so no real colour ever lands on the transparent index 0, and the low
     ramp matches. Paint transparent areas as colour 0, visible helmet colours as >=1. */
  for (g = 0; g <= 13; g++) present[g] = 1;
  n = 0;
  for (g = 0; g < 256; g++) if (present[g]) { lut[g] = (unsigned char)n; carpal[n] = (unsigned char)g; n++; }
  if (n < 256) carpal[n++] = 0;
  for (k = 0; k < sz; k++) im[k] = lut[im[k]];
  return n;
}

/* Called from the asm stub (per textured polygon) BEFORE sub_41D50. If this is a helmet face and
   the slot's occupant needs to change, overwrite the slot's atlas+palette in place (or restore
   stock). Cheap for non-helmet faces (one compare). */
void __near _cdecl AHFHelmetSwap(void)
{
  unsigned char *car, *atlas, *pal0;
  int carId, slot, palSz, s, pn;
  unsigned int jam, off;

  if (!PerCarHelmet) return;
  if (*s_p18330C != 0x221) return;                     /* not a helmet face */

  jam = *s_pJamId;                                      /* resolved helmet jam */
  if (jam < CH_HELMBASE || jam > CH_HELMTOP) return;
  slot = (int)jam - CH_HELMBASE;

  car = *s_pCar; if (!car) return;
  carId = car[0xA6] & 0x3F;

  /* new track -> stock helmet atlases re-decrypted -> drop snapshots */
  if (dwTrackChecksum != s_snapCsum) {
    for (s = 0; s < CH_SLOTS; s++) { s_snapped[s] = 0; s_slotCar[s] = -1; }
    s_snapCsum = dwTrackChecksum;
  }

  /* slot already holds this car's helmet (or stock, for a non-override car)? nothing to do */
  if (s_slotCar[slot] == carId) return;
  if (s_slotCar[slot] == -1 && !(carId < CH_MAXCAR && s_pathSet[carId])) return;

  /* resolve the slot's atlas exactly as sub_41D50 will */
  off   = *(unsigned short *)(s_pJamTab + jam * 2);
  if (off == 0xFFFF) return;
  atlas = *(unsigned char **)(s_pDesc + off);
  if (!atlas) return;
  if (!s_hw) { s_hw = *(unsigned short *)(s_pDesc + off + 0x04); s_hh = *(unsigned short *)(s_pDesc + off + 0x06); s_hsz = s_hw * s_hh; }
  pal0  = s_palBase + *(unsigned long *)(s_pDesc + off + 0x0C);
  palSz = (int)*(unsigned short *)(s_pDesc + off + 0x10);
  if (palSz < 1 || palSz > 256 || s_hsz < 1) return;

  /* snapshot stock slot once: the 74x15 region is spread across the 256-wide atlas, so copy the
     IMAGE row-by-row (256 stride) into a compact w*h buffer; for the palette just remember the
     stock +0x0C offset / +0x10 size (we repoint, never overwrite the pool). */
  if (!s_snapped[slot]) {
    if (!s_snap[slot]) s_snap[slot] = (unsigned char *)malloc((size_t)s_hsz);
    if (s_snap[slot]) for (s = 0; s < s_hh; s++) memcpy(s_snap[slot] + s * s_hw, atlas + s * CH_STRIDE, (size_t)s_hw);
    s_snapPalOff[slot] = *(unsigned long  *)(s_pDesc + off + 0x0C);
    s_snapPalSz[slot]  = (int)*(unsigned short *)(s_pDesc + off + 0x10);
    s_snapped[slot] = 1;
  }

  if (carId < CH_MAXCAR && s_pathSet[carId]) {          /* this car has a custom helmet */
    if (!s_img[carId]) {                                /* lazy-load at the slot's dims (compact w*h) */
      s_img[carId] = LoadBmpN(s_path[carId], s_hw, s_hh);
      if (!s_img[carId]) { s_pathSet[carId] = 0; return; }   /* load failed -> keep stock */
    }
    if (!s_remapped[carId]) { s_carPalN[carId] = BuildFromBmp(s_img[carId], s_carpal[carId], s_hsz); s_remapped[carId] = 1; }
    if (!s_palbuf[carId]) {                              /* our own palette buffer (any size, no cap) */
      s_palbuf[carId] = (unsigned char *)malloc(4 * 256);
      if (!s_palbuf[carId]) { s_pathSet[carId] = 0; return; }
    }
    pn = s_carPalN[carId];
    for (s = 0; s < 4; s++) memcpy(s_palbuf[carId] + s * pn, s_carpal[carId], (size_t)pn);
    *(unsigned long  *)(s_pDesc + off + 0x0C) = (unsigned long)s_palbuf[carId] - (unsigned long)s_palBase;
    *(unsigned short *)(s_pDesc + off + 0x10) = (unsigned short)pn;
    for (s = 0; s < s_hh; s++) memcpy(atlas + s * CH_STRIDE, s_img[carId] + s * s_hw, (size_t)s_hw);   /* 256 stride */
    s_slotCar[slot] = carId;
  } else if (s_snap[slot]) {                             /* non-override: restore stock */
    *(unsigned long  *)(s_pDesc + off + 0x0C) = s_snapPalOff[slot];   /* stock palette back */
    *(unsigned short *)(s_pDesc + off + 0x10) = (unsigned short)s_snapPalSz[slot];
    for (s = 0; s < s_hh; s++) memcpy(atlas + s * CH_STRIDE, s_snap[slot] + s * s_hw, (size_t)s_hw);   /* 256 stride */
    s_slotCar[slot] = -1;
  }
}

void (__near _cdecl *fpHelmetTexCode)(void) = AHFHelmetSwap;

static int InstallHook(void)
{
  unsigned char *p = (unsigned char *)IDAtoFlat(0x440B1UL);   /* call sub_41D50 (per textured poly) */
  long disp;
  if (p[0] != 0xE8) { sprintf(strbuf, "- CarHelmet: 0x440B1 not a call (%02X); DISABLED\n", p[0]); LogLine(strbuf); return 0; }
  disp = *(long *)(p + 1);
  HelmetTexOrig = (unsigned long)(p + 5 + disp);             /* chain the current target (sub_41D50) */
  *(long *)(p + 1) = (long)((unsigned long)MyHelmetTex - (unsigned long)(p + 5));
  return 1;
}

void CarHelmetInit(void)
{
  int c, loaded = 0;

  for (c = 0; c < CH_MAXCAR; c++) { s_pathSet[c] = 0; s_img[c] = 0; s_remapped[c] = 0; }
  for (c = 0; c < CH_SLOTS; c++)  { s_snap[c] = 0; s_snapped[c] = 0; s_slotCar[c] = -1; }

  for (c = 1; c < OV_MAXCAR && c < CH_MAXCAR; c++) {
    const OvCar *o = OverrideCar(c);
    if (o && o->helmetSet && o->helmet[0]) {
      strncpy(s_path[c], o->helmet, sizeof(s_path[c]) - 1); s_path[c][sizeof(s_path[c]) - 1] = 0;
      s_pathSet[c] = 1; loaded++;
      sprintf(strbuf, "- CarHelmet: car%02d helmet <- %s\n", c, o->helmet); LogLine(strbuf);
    }
  }
  if (!loaded) return;

  s_pCar    = (unsigned char **)IDACodeReftoDataRef(0x65D41UL);  /* &dword_D5490 */
  s_pJamId  = (unsigned short *)IDACodeReftoDataRef(0x65D87UL);  /* &word_18330A */
  s_p18330C = (unsigned short *)IDACodeReftoDataRef(0x41D5BUL);  /* &word_18330C */
  s_pJamTab = (unsigned char  *)IDACodeReftoDataRef(0x65D8FUL);  /* &word_4C4EA8 */
  s_pDesc   = (unsigned char  *)IDACodeReftoDataRef(0x65D9BUL);  /* &unk_184A8C  */
  s_palBase = (unsigned char  *)IDACodeReftoDataRef(0x41E3BUL);  /* &unk_462D7C  */

  if (!s_pCar || !s_pJamId || !s_p18330C || !s_pJamTab || !s_pDesc || !s_palBase) {
    LogLine("- CarHelmet: globals unresolved; DISABLED\n"); return;
  }
  if (!InstallHook()) return;

  s_snapCsum = 0;
  PerCarHelmet = 1;
  sprintf(strbuf, "- CarHelmet: ON (overwrite-in-place), %d custom helmet(s)\n", loaded); LogLine(strbuf);
}
