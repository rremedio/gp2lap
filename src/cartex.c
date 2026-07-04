#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cartex.h"
#include "miscahf.h"        // IDAtoFlat, IDACodeReftoDataRef
#include "cfgmain.h"        // GetCfgString
#include "basiclog.h"       // LogLine / strbuf
#include "override.h"       // shared SeasonOverrides model (per-car livery/cockpit)
#include "svga/svgabmp.h"   // readstream_svgabmp
#include "gp2hook.h"        // dwTrackChecksum

#define CT_W      256
#define CT_H      164
#define CT_SZ     (CT_W * CT_H)      /* 41984 */
#define CT_MAXCAR 64                 /* carId masked to 0x3F */
#define CT_TEAMS  20                 /* stock 14 + override-added 15..20; s_pCache /
                                        s_pTeamTab / cockpit tables are all >=20-wide */

unsigned long PerCarTextures = 0;
unsigned long PerCarCockpit  = 0;

static unsigned char *s_img[CT_MAXCAR];   /* override image per carId (NULL = none) */
static int            s_remapped[CT_MAXCAR]; /* override image converted global->local indices yet? */
static unsigned char  s_baked[CT_MAXCAR]; /* 1 = per-car livery (number baked, suppress blit); 0 = added-team base (engine blits number) */
static unsigned char *s_snap[CT_TEAMS];   /* pristine (stock) atlas snapshot per team */
static int            s_snapped[CT_TEAMS];
static unsigned char  s_carpal[CT_MAXCAR][256];  /* per-car flat sub0 palette (built from its BMP) */
static int            s_carPalN[CT_MAXCAR];      /* per-car palette length = sorted colours + 1 zero */
static unsigned char  s_snappal[CT_TEAMS][1024]; /* stock 4 sub-palettes per team (4*256) for restore */
static int            s_snapPalSz[CT_TEAMS];     /* stock pal_sz per team (for restore) */
static unsigned long  s_snapCsum;

/* GP2 data globals, bootstrapped from code operands in sub_65D3B (see later task) */
static unsigned char **s_pCar    = 0;  /* &dword_D5490 (car ptr)        */
static unsigned short *s_pJamId  = 0;  /* &word_18330A                  */
static unsigned char  *s_pJamTab = 0;  /* &word_4C4EA8[0] (word table)  */
static unsigned char  *s_pDesc   = 0;  /* &unk_184A8C[0] (descriptors)  */
static unsigned char  *s_palBase = 0;  /* unk_462D7C: base for descriptor +0x0C palette offset */
static unsigned char  *s_pCache  = 0;  /* unk_D6CC0[14]: engine's "whose number is in atlas" cache */
static unsigned char  *s_pTeamTab = 0; /* t_CaridTeamTab: car_id of each team's first driver */

/* Per-car cockpit colours (independent feature, same SeasonOverrides file, [CockpitColors]). */
extern void MyCockpitColors(void);        /* asm read-site trampoline in lammcall.asm */
static unsigned char  s_ckpit[CT_MAXCAR][3]; /* per-car cockpit colour bases (the 3 ramp bases) */
static unsigned char  s_ckpitSet[CT_MAXCAR]; /* 1 if this car has a cockpit colour override */
static int            s_ckLoaded = 0;        /* number of cockpit overrides parsed */
static unsigned char **s_pCarView = 0;       /* &p_CarInViewCS (0xD47B4): cockpit car-struct ptr */
static unsigned char  *s_pCkBase  = 0;       /* &byte_CA0A0: the 3 active cockpit ramp bases */
static unsigned char  *s_pTeamCk  = 0;       /* &arCockpitColors (0x178FC2): stock per-team, 3 bytes/team */

/* Load one 256x164x8 BMP into a fresh CT_SZ buffer. readstream_svgabmp validates
   dimensions/bit-depth and already flips rows to top-down (atlas order). Returns the
   buffer or NULL (logged). */
static unsigned char *CarTexLoadBmp(const char *path)
{
  FILE *f;
  unsigned char *buf;
  unsigned char pal[768];    /* readstream needs a palette buffer; we discard it (pixels are indices) */
  long r;

  f = fopen(path, "rb");
  if (!f) { sprintf(strbuf, "- CarTex: open FAILED '%s'\n", path); LogLine(strbuf); return 0; }
  buf = (unsigned char *)malloc(CT_SZ);
  if (!buf) { fclose(f); LogLine("- CarTex: malloc FAILED\n"); return 0; }
  r = readstream_svgabmp(CT_W, CT_H, buf, pal, f);
  fclose(f);
  if (r != 1) {
    sprintf(strbuf, "- CarTex: bad BMP '%s' (r=%ld; need 256x164x8)\n", path, r);
    LogLine(strbuf); free(buf); return 0;
  }
  return buf;
}

void CarTexInit(void)
{
  char *cfg;
  int i, team, loaded;
  unsigned char *s1, *s2, *s3, *s4, *s5, *s6, *s7;

  cfg = GetCfgString("SeasonOverrides");
  if (!cfg || !cfg[0]) return;                 /* key absent -> feature off */

  loaded = 0;                                  /* pull resolved per-car liveries (Car1/Car2) from the shared model */
  for (i = 1; i < CT_MAXCAR; i++) {
    const OvCar *o = OverrideCar(i);
    if (o && o->liverySet && o->livery[0]) {
      if (s_img[i]) { free(s_img[i]); s_img[i] = 0; }
      s_img[i] = CarTexLoadBmp(o->livery);
      if (s_img[i]) { s_baked[i] = 1; loaded++;       /* per-car BMP: number baked in -> suppress the engine blit */
        sprintf(strbuf, "- CarTex: car%02d <- %s\n", i, o->livery); LogLine(strbuf); }
    }
  }

  /* Added-team base liveries (team 15..20 `Livery`): a car with no per-car Car1/Car2 override
     shows its team's base BMP. Added-team cars resolve (via the sub_677D0 clamp) to team-14's
     atlas slot 544, which cartex overwrites per-draw like any other car -- overwrite-in-place, so
     it survives the pause menu (unlike the old jam-788 registration, which resume dropped). Its
     carId comes from the override's per-seat number. baked=0 -> the engine still blits the number. */
  for (team = OV_STOCKTEAMS + 1; team <= OV_TEAMS; team++) {
    const OvTeam *t = OverrideTeam(team);
    int s, carId;
    if (!t || !t->liverySet || !t->livery[0]) continue;
    for (s = 0; s < 2; s++) {
      if (!t->drv[s].numSet) continue;
      carId = t->drv[s].num & 0x3F;
      if (carId < 1 || carId >= CT_MAXCAR || s_img[carId]) continue;   /* per-car override wins */
      s_img[carId] = CarTexLoadBmp(t->livery);
      if (s_img[carId]) { s_baked[carId] = 0; loaded++;
        sprintf(strbuf, "- CarTex: car%02d <- %s (team%02d base)\n", carId, t->livery, team); LogLine(strbuf); }
    }
  }
  if (loaded < 1) { LogLine("- CarTex: no car images loaded; disabled\n"); return; }

  /* Bootstrap GP2 data globals from operands inside sub_65D3B (atlas resolve) and
     sub_41D50 (palette base). Verify opcodes first. */
  s1 = (unsigned char *)IDAtoFlat(0x65D3F);    /* 8B 35 <&dword_D5490>  mov esi,dword_D5490 */
  s2 = (unsigned char *)IDAtoFlat(0x65D84);    /* 0F B7 05 <&word_18330A> */
  s3 = (unsigned char *)IDAtoFlat(0x65D8B);    /* 0F B7 04 45 <&word_4C4EA8> */
  s4 = (unsigned char *)IDAtoFlat(0x65D9A);    /* 05 <&unk_184A8C>  add eax,offset unk_184A8C */
  s5 = (unsigned char *)IDAtoFlat(0x41E39);    /* 81 C6 <&unk_462D7C>  add esi,offset palBase */
  s6 = (unsigned char *)IDAtoFlat(0x65D72);    /* 3A 8A <&unk_D6CC0>  cmp cl,unk_D6CC0[edx] */
  s7 = (unsigned char *)IDAtoFlat(0x65D64);    /* 3A 04 55 <&t_CaridTeamTab>  cmp al,tab[edx*2] */
  if (s1[0]!=0x8B||s1[1]!=0x35 || s2[0]!=0x0F||s2[1]!=0xB7||s2[2]!=0x05 ||
      s3[0]!=0x0F||s3[1]!=0xB7||s3[2]!=0x04||s3[3]!=0x45 || s4[0]!=0x05 ||
      s5[0]!=0x81||s5[1]!=0xC6 || s6[0]!=0x3A||s6[1]!=0x8A ||
      s7[0]!=0x3A||s7[1]!=0x04||s7[2]!=0x55) {
    LogLine("- CarTex: opcode mismatch at sub_65D3B/41D50; DISABLED\n");
    for (i = 0; i < CT_MAXCAR; i++) if (s_img[i]) { free(s_img[i]); s_img[i] = 0; }
    return;
  }
  s_pCar    = (unsigned char **)IDACodeReftoDataRef(0x65D41);  /* &dword_D5490 */
  s_pJamId  = (unsigned short *)IDACodeReftoDataRef(0x65D87);  /* &word_18330A */
  s_pJamTab = (unsigned char  *)IDACodeReftoDataRef(0x65D8F);  /* &word_4C4EA8 */
  s_pDesc   = (unsigned char  *)IDACodeReftoDataRef(0x65D9B);  /* &unk_184A8C  */
  s_palBase = (unsigned char  *)IDACodeReftoDataRef(0x41E3B);  /* &unk_462D7C (palette base) */
  s_pCache  = (unsigned char  *)IDACodeReftoDataRef(0x65D74);  /* &unk_D6CC0 (number-blit cache) */
  s_pTeamTab= (unsigned char  *)IDACodeReftoDataRef(0x65D67);  /* &t_CaridTeamTab (first driver per team) */

  /* Snapshots are allocated LAZILY (per team, on that team's first per-car draw) -- only teams
     actually drawn with a per-car override ever need one. Pre-allocating all CT_TEAMS wasted
     ~0.8MB of the DOS4GW heap and could fail outright when JamTextureExtraMB reserves a large
     pool. s_snap[]/s_snapped[] are zero-initialised statics; a failed lazy alloc just skips the
     restore for that team (its non-override teammate keeps the previous livery -- no crash). */
  for (i = 0; i < CT_TEAMS; i++) { s_snap[i] = 0; s_snapped[i] = 0; }
  s_snapCsum = 0;

  PerCarTextures = 1;
  sprintf(strbuf, "- CarTex: ON, %d car image(s) loaded\n", loaded);
  LogLine(strbuf);
}

/* Build a flat palette straight FROM the override BMP, exactly like GP2Edit: the distinct
   global colours the image uses, sorted ascending, become sub0 followed by ONE trailing zero;
   the image is rewritten to indices into that sorted list. Returns the new palette length (the
   caller writes carpal to all 4 sub-palettes and sets the descriptor pal_sz to it). */
static int CarTexBuildFromBmp(unsigned char *im, unsigned char *carpal)
{
  unsigned char present[256], lut[256];
  int g, k, n;

  for (g = 0; g < 256; g++) present[g] = 0;
  for (k = 0; k < CT_SZ; k++) present[im[k]] = 1;
  for (g = 1; g <= 13; g++) present[g] = 1;        /* always include the greyscale ramp 1..13 */
  n = 0;
  for (g = 0; g < 256; g++)                       /* ascending -> sorted palette */
    if (present[g]) { lut[g] = (unsigned char)n; carpal[n] = (unsigned char)g; n++; }
  if (n < 256) carpal[n++] = 0;                    /* one trailing zero after the colours found */
  for (k = 0; k < CT_SZ; k++) im[k] = lut[im[k]];  /* image -> indices into the sorted palette */
  return n;                                        /* new palette length = colours + 1 */
}

void __near _cdecl AHFCarTexSwap(void)
{
  unsigned char *car, *atlas, *pal0;
  int carId, team, snapKey, palSz, s, pn;
  unsigned int jamid, off;

  if (!PerCarTextures) return;

  /* New track -> atlases re-decrypted -> re-capture pristine snapshots. */
  if (dwTrackChecksum != s_snapCsum) {
    int t;
    for (t = 0; t < CT_TEAMS; t++) s_snapped[t] = 0;
    s_snapCsum = dwTrackChecksum;
  }

  car = *s_pCar;                          /* dword_D5490 */
  if (!car) return;

  team = (int)(car[0x25] & 0xFF) - 1;     /* teamNr 1..20 -> 0..19 (engine clamps >=0) */
  if (team < 0) team = 0;
  if (team >= CT_TEAMS) return;

  /* Added teams 15..20 (index >=14) all resolve, via the sub_677D0 clamp, to team-14's atlas
     (slot 544). They share ONE physical slot + number-cache with real team-14 cars, so the
     snapshot/restore + cache key is team-14's index (13) -- NOT their own -- else each added team
     would capture a dirty (already-overwritten) 544 as its "stock" base. The per-car overwrite
     itself stays keyed by carId. */
  snapKey = (team >= OV_STOCKTEAMS) ? (OV_STOCKTEAMS - 1) : team;

  carId = car[0xA6] & 0x3F;               /* strip player bit7 */

  /* Resolve the team atlas exactly as sub_65D3B does. */
  jamid = *s_pJamId;                                   /* word_18330A */
  off   = *(unsigned short *)(s_pJamTab + jamid * 2);  /* word_4C4EA8[jamid] */
  if (off == 0xFFFF) return;                           /* no atlas */
  atlas = *(unsigned char **)(s_pDesc + off);          /* unk_184A8C[off] = image base */
  if (!atlas) return;

  /* Only ever touch a standard 256x164 car-body atlas. In some paths (notably the RCR /
     distant-car renderer) this hook resolves a DIFFERENT jam -- e.g. a small tyre atlas
     (~57x46) -- and blasting 256x164 over it overruns neighbouring images (garbage tyres).
     descriptor: width @ +0x04, height @ +0x06 (JAM_ENTRY). */
  if (*(unsigned short *)(s_pDesc + off + 0x04) != CT_W ||
      *(unsigned short *)(s_pDesc + off + 0x06) != CT_H) return;

  /* The team atlas palette: descriptor +0x0C = offset RELATIVE to unk_462D7C; +0x10 = pal_sz
     (sub_41D50). The 4 sub-palettes are consecutive, palSz bytes each. */
  pal0  = s_palBase + *(unsigned long *)(s_pDesc + off + 0x0C);
  palSz = (int)*(unsigned short *)(s_pDesc + off + 0x10);
  if (palSz < 1 || palSz > 256) return;

  /* Snapshot the STOCK atlas image + its 4 sub-palettes + pal_sz once per snapKey (to restore
     non-override cars; the palette is shared per slot, so an override car dirties it). Allocate
     the buffer lazily on first use. Added teams 15..20 share team-14's slot 544 (snapKey 13), so
     this captures pristine stock-544 once -- taken on whichever of those cars draws first, before
     any of them overwrites it. */
  if (!s_snapped[snapKey]) {
    if (!s_snap[snapKey]) s_snap[snapKey] = (unsigned char *)malloc(CT_SZ);   /* lazy: only drawn teams */
    if (s_snap[snapKey]) {
      memcpy(s_snap[snapKey], atlas, CT_SZ);
      memcpy(s_snappal[snapKey], pal0, 4 * palSz);
      s_snapPalSz[snapKey] = palSz;
    }
    s_snapped[snapKey] = 1;
  }

  /* First use of this car's override: build its flat palette from the BMP (distinct colours
     sorted -> sub0 + one trailing zero) and rewrite the image to indices into it. */
  if (s_img[carId] && !s_remapped[carId]) {
    s_carPalN[carId] = CarTexBuildFromBmp(s_img[carId], s_carpal[carId]);
    s_remapped[carId] = 1;
  }

  /* Re-apply on EVERY draw (the engine re-decrypts the atlas + palette to stock at moments we
     cannot track). Write the palette (4 sub-palettes = our sub0), shrink the descriptor pal_sz
     to our length, and write the index image. */
  if (s_img[carId]) {
    pn = s_carPalN[carId];
    *(unsigned short *)(s_pDesc + off + 0x10) = (unsigned short)pn;   /* new pal_sz */
    for (s = 0; s < 4; s++) memcpy(pal0 + s * pn, s_carpal[carId], pn);
    memcpy(atlas, s_img[carId], CT_SZ);
    /* The engine's number-blit cache (unk_D6CC0) is indexed by the car's OWN team (14..19 for
       added teams), NOT the shared physical slot -- so it must be keyed by `team`, not snapKey,
       or the suppression misses and the engine re-blits its number (visibly fighting our baked
       number, LOD/camera-distance dependent). Snapshot/image stay keyed by snapKey (the slot). */
    if (s_pCache && s_pTeamTab) {
      if (s_baked[carId])
        s_pCache[team] = (car[0xA6] == s_pTeamTab[team*2]) ? 1 : 2;  /* number baked in -> suppress the blit */
      else
        s_pCache[team] = 0xFF;                                        /* base livery, no number -> let the engine blit it */
    }
  } else if (s_snap[snapKey]) {
    *(unsigned short *)(s_pDesc + off + 0x10) = (unsigned short)s_snapPalSz[snapKey];  /* restore */
    memcpy(pal0, s_snappal[snapKey], 4 * s_snapPalSz[snapKey]);
    memcpy(atlas, s_snap[snapKey], CT_SZ);
    if (s_pCache) s_pCache[team] = 0xFF;               /* let the engine blit the stock number */
  }
  /* original sub_65D3B (chained from the asm stub) blits the number for stock cars */
}

/* asm hook calls this pointer */
void (__near _cdecl *fpCarTexCode)(void) = AHFCarTexSwap;

/* ------------------------------------------------------------------------- *
 *  Per-car cockpit colours
 *
 *  Stock GP2 colours the cockpit per TEAM: rUpdCarsCockpit (IDA 0x69ED6) reads
 *  the cockpit car's teamNr and writes three palette-index ramp bases
 *  (byte_CA0A0/A1/A2) from arCockpitColors[(teamNr-1)*3]; rCkpitColAdjust
 *  (0x7139D) then shifts the cockpit's 0x2x / 0x3x / 0x9x-0xAx ramps by those
 *  bases at blit time. We replace the per-team computation in place with a call
 *  to MyCockpitColors so timing matches stock exactly: for a car with a
 *  [CockpitColors] override we write its three bases, otherwise we reproduce the
 *  stock per-team lookup byte-for-byte. See docs/gp2lap/per-car-cockpit-colors.md.
 * ------------------------------------------------------------------------- */
void __near _cdecl AHFCockpitColors(void)
{
  unsigned char *car;
  int carId, k;
  unsigned char team;

  if (!PerCarCockpit) return;
  car = *s_pCarView;                       /* p_CarInViewCS: the cockpit car */
  if (!car) return;

  carId = car[0xA6] & 0x3F;                /* strip player bit7 -> car number 1..40 */
  if (carId >= 1 && carId < CT_MAXCAR && s_ckpitSet[carId]) {
    for (k = 0; k < 3; k++) s_pCkBase[k] = s_ckpit[carId][k];   /* per-car override */
    return;
  }
  /* fallback: reproduce stock (dec al; and eax,0FFh; eax*3; read arCockpitColors[eax+k]) */
  team = (unsigned char)(car[0x25] - 1);
  for (k = 0; k < 3; k++) s_pCkBase[k] = s_pTeamCk[team * 3 + k];
}

/* asm read-site trampoline calls this pointer */
void (__near _cdecl *fpCockpitColCode)(void) = AHFCockpitColors;

void CarCockpitInit(void)
{
  unsigned char *c0, *c1, *c2, *c3, *p;
  int i;

  s_ckLoaded = 0;                          /* pull resolved cockpit colours from the shared model */
  for (i = 1; i < CT_MAXCAR; i++) {
    const OvCar *o = OverrideCar(i);
    if (o && o->cpSet) {
      s_ckpit[i][0] = o->cp[0]; s_ckpit[i][1] = o->cp[1]; s_ckpit[i][2] = o->cp[2];
      s_ckpitSet[i] = 1; s_ckLoaded++;
      sprintf(strbuf, "- CarCkpit: cp%02d <- %u,%u,%u\n", i, o->cp[0], o->cp[1], o->cp[2]); LogLine(strbuf);
    }
  }
  if (s_ckLoaded < 1) return;              /* no cockpit overrides -> stock per-team */

  /* Verify the stock instructions we read operands from and patch over. */
  c0 = (unsigned char *)IDAtoFlat(0x69ED7);  /* 8B 35 <&p_CarInViewCS>  mov esi,p_CarInViewCS */
  c1 = (unsigned char *)IDAtoFlat(0x69EDD);  /* 8A 46 25                mov al,[esi+25h]  (patch start) */
  c2 = (unsigned char *)IDAtoFlat(0x69EEA);  /* 8A 90 <&arCockpitColors> */
  c3 = (unsigned char *)IDAtoFlat(0x69EF0);  /* 88 15 <&byte_CA0A0> */
  if (c0[0]!=0x8B||c0[1]!=0x35 || c1[0]!=0x8A||c1[1]!=0x46||c1[2]!=0x25 ||
      c2[0]!=0x8A||c2[1]!=0x90 || c3[0]!=0x88||c3[1]!=0x15) {
    LogLine("- CarCkpit: opcode mismatch at rUpdCarsCockpit; DISABLED\n");
    return;
  }
  s_pCarView = (unsigned char **)IDACodeReftoDataRef(0x69ED9);  /* &p_CarInViewCS */
  s_pTeamCk  = (unsigned char  *)IDACodeReftoDataRef(0x69EEC);  /* &arCockpitColors (per-team) */
  s_pCkBase  = (unsigned char  *)IDACodeReftoDataRef(0x69EF2);  /* &byte_CA0A0 (3 active bases) */

  /* Replace the 49-byte per-team computation (0x69EDD..0x69F0D) with
     "call MyCockpitColors" + NOP padding. The following "call sub_713EC" (cockpit
     redraw) at 0x69F0E is left intact, so our bases are in place before the redraw. */
  p = (unsigned char *)IDAtoFlat(0x69EDD);
  p[0] = 0xE8;                                                  /* call rel32 */
  *(long *)(p + 1) = (long)((unsigned long)MyCockpitColors - (unsigned long)(p + 5));
  memset(p + 5, 0x90, 49 - 5);                                  /* NOP the displaced bytes */

  PerCarCockpit = 1;
  sprintf(strbuf, "- CarCkpit: ON, %d car colour(s) loaded\n", s_ckLoaded);
  LogLine(strbuf);
}
