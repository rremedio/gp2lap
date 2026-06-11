#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cartex.h"
#include "miscahf.h"        // IDAtoFlat, IDACodeReftoDataRef
#include "cfgmain.h"        // GetCfgString
#include "basiclog.h"       // LogLine / strbuf
#include "svga/svgabmp.h"   // readstream_svgabmp
#include "gp2hook.h"        // dwTrackChecksum

#define CT_W      256
#define CT_H      164
#define CT_SZ     (CT_W * CT_H)      /* 41984 */
#define CT_MAXCAR 64                 /* carId masked to 0x3F */
#define CT_TEAMS  14

unsigned long PerCarTextures = 0;

static unsigned char *s_img[CT_MAXCAR];   /* override image per carId (NULL = none) */
static int            s_remapped[CT_MAXCAR]; /* override image converted global->local indices yet? */
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

/* Parse override.cfg: flat "carNN = path" lines. ';' and '#' comments, [sections] ignored.
   Returns number of car images loaded. */
static int CarTexParse(const char *cfgpath)
{
  FILE *f;
  char line[300], path[256];
  int loaded = 0;

  f = fopen(cfgpath, "rb");
  if (!f) { sprintf(strbuf, "- CarTex: SeasonOverrides file '%s' not found; disabled\n", cfgpath);
            LogLine(strbuf); return 0; }

  while (fgets(line, sizeof(line), f)) {
    char *p = line, *eq, *v, *e;
    int carId;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == ';' || *p == '#' || *p == '[' || *p == '\r' || *p == '\n' || *p == 0) continue;
    /* key must be carNN */
    if ((p[0]|0x20) != 'c' || (p[1]|0x20) != 'a' || (p[2]|0x20) != 'r') continue;
    carId = atoi(p + 3);
    if (carId < 1 || carId >= CT_MAXCAR) continue;
    eq = strchr(p, '=');
    if (!eq) continue;
    v = eq + 1;
    while (*v == ' ' || *v == '\t' || *v == '"') v++;
    /* copy value, trim trailing ws/quote/newline */
    strncpy(path, v, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
    e = path + strlen(path);
    while (e > path && (e[-1]=='\r'||e[-1]=='\n'||e[-1]==' '||e[-1]=='\t'||e[-1]=='"')) *--e = 0;
    if (!path[0]) continue;
    if (s_img[carId]) { free(s_img[carId]); s_img[carId] = 0; }
    s_img[carId] = CarTexLoadBmp(path);
    if (s_img[carId]) { loaded++;
      sprintf(strbuf, "- CarTex: car%02d <- %s\n", carId, path); LogLine(strbuf); }
  }
  fclose(f);
  return loaded;
}

void CarTexInit(void)
{
  char *cfg;
  int i, loaded;
  unsigned char *s1, *s2, *s3, *s4, *s5, *s6, *s7;

  cfg = GetCfgString("SeasonOverrides");
  if (!cfg || !cfg[0]) return;                 /* key absent -> feature off */

  loaded = CarTexParse(cfg);
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

  /* Pre-allocate pristine snapshot buffers (avoid malloc in the render path).
     If any allocation fails, disable cleanly: with a missing snapshot a
     non-overridden teammate would not be restored and would keep the previous
     car's livery (its restore branch is skipped). */
  for (i = 0; i < CT_TEAMS; i++) {
    s_snap[i] = (unsigned char *)malloc(CT_SZ);
    s_snapped[i] = 0;
    if (!s_snap[i]) {
      int j;
      LogLine("- CarTex: snapshot alloc FAILED; DISABLED\n");
      for (j = 0; j < CT_TEAMS;  j++) if (s_snap[j]) { free(s_snap[j]); s_snap[j] = 0; }
      for (j = 0; j < CT_MAXCAR; j++) if (s_img[j])  { free(s_img[j]);  s_img[j]  = 0; }
      return;
    }
  }
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
  int carId, team, palSz, s, pn;
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

  team = (int)(car[0x25] & 0xFF) - 1;     /* teamNr 1..14 -> 0..13 (engine clamps >=0) */
  if (team < 0) team = 0;
  if (team >= CT_TEAMS) return;

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

  /* Snapshot the STOCK atlas image + its 4 sub-palettes + pal_sz once per team (to restore
     non-override cars; the palette is shared per team, so an override teammate dirties it). */
  if (!s_snapped[team]) {
    if (s_snap[team]) {
      memcpy(s_snap[team], atlas, CT_SZ);
      memcpy(s_snappal[team], pal0, 4 * palSz);
      s_snapPalSz[team] = palSz;
    }
    s_snapped[team] = 1;
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
    /* our BMP has its number baked in -> suppress the engine's number blit (force a cache HIT). */
    if (s_pCache && s_pTeamTab)
      s_pCache[team] = (car[0xA6] == s_pTeamTab[team*2]) ? 1 : 2;
  } else if (s_snap[team]) {
    *(unsigned short *)(s_pDesc + off + 0x10) = (unsigned short)s_snapPalSz[team];  /* restore */
    memcpy(pal0, s_snappal[team], 4 * s_snapPalSz[team]);
    memcpy(atlas, s_snap[team], CT_SZ);
    if (s_pCache) s_pCache[team] = 0xFF;               /* let the engine blit the stock number */
  }
  /* original sub_65D3B (chained from the asm stub) blits the number for stock cars */
}

/* asm hook calls this pointer */
void (__near _cdecl *fpCarTexCode)(void) = AHFCarTexSwap;
