#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "carlivery.h"
#include "miscahf.h"        /* IDAtoFlat / IDACodeReftoDataRef */
#include "basiclog.h"       /* LogLine / strbuf */
#include "override.h"       /* OverrideTeam / OverrideActiveTeams / OV_STOCKTEAMS */
#include "svga/svgabmp.h"   /* readstream_svgabmp */

#define CL_W        256
#define CL_H        164
#define CL_SZ       (CL_W * CL_H)          /* 41984 */
#define CL_MAX      6                      /* teams 15..20 */
#define CL_JAMBASE  573                    /* free jam-id block (roadmap 6-of-573..578) */
#define CL_CLONEJAM 544                    /* team-14 body atlas: descriptor + attr clone source */

/* --- globals the asm stub MyBodyJam reads (see lammcall.asm) --- */
unsigned short TeamLiveryJam[21];          /* [teamNr 0..20] = our jam-id, 0 = none */
unsigned long  pWord18330A = 0;            /* &word_18330A (resolved) */
unsigned long  BodyJamOrig = 0;            /* flat sub_677D0 (the stock resolver) */
extern void MyBodyJam(void);               /* asm remap stub */

/* --- resolved registration pointers --- */
static unsigned char  *s_pool    = 0;      /* unk_184A8C descriptor pool base */
static unsigned char  *s_palBase = 0;      /* unk_462D7C palette base */
static unsigned char  *s_attr1   = 0;      /* unk_4C54D0[jamid] */
static unsigned char  *s_attr2   = 0;      /* unk_4C57E4[jamid] */
static unsigned short *s_map     = 0;      /* word_4C4EA8[jamid] = descriptor byte offset */
static unsigned long  *s_pCursor = 0;      /* dword_184A78 descriptor cursor (absolute ptr) */
static unsigned long  *s_pCount  = 0;      /* dword_184A28 running item count */

/* --- per added-team livery slots --- */
static int            g_n = 0;
static int            g_jam[CL_MAX];        /* jam-id = 573 + slot */
static unsigned char *g_img[CL_MAX];        /* WORKING index image (cartex overwrites per-car draw) */
static unsigned char *g_pal[CL_MAX];        /* WORKING 4 sub-palettes x palN */
static int            g_palN[CL_MAX];
static unsigned char *g_base[CL_MAX];        /* pristine base image (re-decrypt substitute) */
static unsigned char *g_basePal[CL_MAX];     /* pristine base palette */

/* Load one 256x164x8 BMP into a fresh CL_SZ buffer (top-down atlas order). */
static unsigned char *LoadBmp(const char *path)
{
  FILE *f; unsigned char *buf; unsigned char pal[768]; long r;
  f = fopen(path, "rb");
  if (!f) { sprintf(strbuf, "- CarLivery: open FAILED '%s'\n", path); LogLine(strbuf); return 0; }
  buf = (unsigned char *)malloc(CL_SZ);
  if (!buf) { fclose(f); LogLine("- CarLivery: malloc FAILED\n"); return 0; }
  r = readstream_svgabmp(CL_W, CL_H, buf, pal, f);
  fclose(f);
  if (r != 1) { sprintf(strbuf, "- CarLivery: bad BMP '%s' (need 256x164x8)\n", path); LogLine(strbuf); free(buf); return 0; }
  return buf;
}

/* Build a flat palette from the BMP (distinct global colours sorted -> sub0 + one trailing
   zero) and rewrite the image to indices into it. Returns palette length. Same scheme as the
   per-car cartex path, so the engine reads it identically. */
static int BuildPal(unsigned char *im, unsigned char *carpal)
{
  unsigned char present[256], lut[256]; int g, k, n;
  for (g = 0; g < 256; g++) present[g] = 0;
  for (k = 0; k < CL_SZ; k++) present[im[k]] = 1;
  for (g = 1; g <= 13; g++) present[g] = 1;            /* keep the greyscale ramp 1..13 */
  n = 0;
  for (g = 0; g < 256; g++) if (present[g]) { lut[g] = (unsigned char)n; carpal[n] = (unsigned char)g; n++; }
  if (n < 256) carpal[n++] = 0;
  for (k = 0; k < CL_SZ; k++) im[k] = lut[im[k]];
  return n;
}

/* Idempotent: register any added-team atlas whose jam-id map slot is empty (0xFFFF). The
   per-weekend jam load wipes the map, so this runs at init AND at each session start. */
static void RegisterPass(void)
{
  int i;
  if (!g_n || !s_pool || !s_map || !s_pCursor || !s_pCount || !s_palBase || !s_attr1 || !s_attr2)
    return;
  for (i = 0; i < g_n; i++) {
    int jam = g_jam[i];
    unsigned long cloneOff, off;
    unsigned char *clone, *desc;
    if (s_map[jam] != 0xFFFF) continue;                /* still registered this weekend */
    cloneOff = s_map[CL_CLONEJAM];
    if (cloneOff == 0xFFFF) continue;                  /* clone source (team 14) not loaded yet */
    clone = s_pool + cloneOff;
    desc  = (unsigned char *)*s_pCursor;               /* append at the live cursor */
    off   = (unsigned long)desc - (unsigned long)s_pool;
    memcpy(desc, clone, 32);                           /* clone team-14 metadata (flags/attr/w/h) */
    *(unsigned long  *)(desc + 0x00) = (unsigned long)g_img[i];                           /* image ptr (abs)   */
    *(unsigned short *)(desc + 0x04) = CL_W;
    *(unsigned short *)(desc + 0x06) = CL_H;
    *(unsigned long  *)(desc + 0x0C) = (unsigned long)g_pal[i] - (unsigned long)s_palBase; /* pal offset        */
    *(unsigned short *)(desc + 0x10) = (unsigned short)g_palN[i];
    *(unsigned short *)(desc + 0x12) = (unsigned short)jam;
    s_map[jam]   = (unsigned short)off;                /* jam-id -> descriptor byte offset */
    s_attr1[jam] = s_attr1[CL_CLONEJAM];               /* clone the two per-jam attr bytes */
    s_attr2[jam] = s_attr2[CL_CLONEJAM];
    *s_pCursor  += 32;                                 /* advance descriptor cursor */
    *s_pCount   += 1;                                  /* advance running item count */
  }
}

/* Re-point the single call site of sub_677D0 (0x67A5F) to the asm remap stub. That call is
   already owned by GP2Lap's generic code-hook table -- CondPatchCodeHooks (attach flow, BEFORE
   AHFAfterGp2Init) re-points it to Hook_CarShape (per-team shape). So we CHAIN onto whatever
   currently sits there: MyBodyJam calls the existing target (Hook_CarShape -> sub_677D0, which
   sets word_18330A), then overrides word_18330A for a team 15-20 with a registered livery. Both
   the stock resolver and Hook_CarShape are register/esi-transparent and retn, so calling the
   current target and reading [esi+25h] afterwards is safe. */
static int InstallRemap(void)
{
  unsigned char *p = (unsigned char *)IDAtoFlat(0x67A5FUL);
  long disp;
  if (p[0] != 0xE8) {
    sprintf(strbuf, "- CarLivery: call-site @0x67A5F not a call (%02X); remap DISABLED\n", p[0]);
    LogLine(strbuf); return 0;
  }
  disp = *(long *)(p + 1);
  BodyJamOrig = (unsigned long)(p + 5 + disp);   /* current target (Hook_CarShape or sub_677D0) */
  *(long *)(p + 1) = (long)((unsigned long)MyBodyJam - (unsigned long)(p + 5));
  return 1;
}

void CarLiveryInit(void)
{
  int team, i;

  for (i = 0; i < 21; i++) TeamLiveryJam[i] = 0;
  g_n = 0;

  /* load a base livery BMP for each fielded added team (15..active) that set Livery */
  for (team = OV_STOCKTEAMS + 1; team <= OverrideActiveTeams() && g_n < CL_MAX; team++) {
    const OvTeam *t = OverrideTeam(team);
    unsigned char carpal[256], *img; int pn, s;
    if (!t || !t->liverySet || !t->livery[0]) continue;
    img = LoadBmp(t->livery);
    if (!img) continue;
    pn = BuildPal(img, carpal);
    g_pal[g_n] = (unsigned char *)malloc(4 * 256);
    if (!g_pal[g_n]) { free(img); LogLine("- CarLivery: pal malloc FAILED\n"); continue; }
    for (s = 0; s < 4; s++) memcpy(g_pal[g_n] + s * pn, carpal, pn);
    /* keep a pristine copy of the base: the engine re-decrypts stock atlases to stock each
       weekend, but our buffers persist, so cartex's per-car overwrite would otherwise leave a
       stale livery as the "base" for a non-override teammate. Restore working<-base at each SOS. */
    g_base[g_n]    = (unsigned char *)malloc(CL_SZ);
    g_basePal[g_n] = (unsigned char *)malloc(4 * 256);
    if (g_base[g_n])    memcpy(g_base[g_n], img, CL_SZ);
    if (g_basePal[g_n]) memcpy(g_basePal[g_n], g_pal[g_n], 4 * pn);
    g_img[g_n]  = img;
    g_palN[g_n] = pn;
    g_jam[g_n]  = CL_JAMBASE + g_n;
    TeamLiveryJam[team] = (unsigned short)g_jam[g_n];
    sprintf(strbuf, "- CarLivery: team%02d body <- %s (jam %d)\n", team, t->livery, g_jam[g_n]); LogLine(strbuf);
    g_n++;
  }
  if (!g_n) return;                                    /* no added-team liveries */

  /* resolve the registration + remap globals (the operands cartex uses, plus the pool cursors) */
  s_pool    = (unsigned char  *)IDACodeReftoDataRef(0x65D9BUL);  /* unk_184A8C */
  s_map     = (unsigned short *)IDACodeReftoDataRef(0x65D8FUL);  /* word_4C4EA8 */
  s_palBase = (unsigned char  *)IDACodeReftoDataRef(0x41E3BUL);  /* unk_462D7C */
  s_pCursor = (unsigned long  *)IDACodeReftoDataRef(0x70B20UL);  /* dword_184A78 */
  s_pCount  = (unsigned long  *)IDACodeReftoDataRef(0x70B55UL);  /* dword_184A28 */
  s_attr1   = (unsigned char  *)IDACodeReftoDataRef(0x70BA1UL);  /* unk_4C54D0 */
  s_attr2   = (unsigned char  *)IDACodeReftoDataRef(0x70BBCUL);  /* unk_4C57E4 */
  pWord18330A = (unsigned long)IDACodeReftoDataRef(0x65D87UL);   /* &word_18330A */

  if (!s_pool || !s_map || !s_palBase || !s_pCursor || !s_pCount || !s_attr1 || !s_attr2 || !pWord18330A) {
    LogLine("- CarLivery: registration globals unresolved; DISABLED\n");
    g_n = 0; return;
  }
  if (!InstallRemap()) { g_n = 0; return; }

  RegisterPass();                                      /* register now (covers pre-session previews) */
  sprintf(strbuf, "- CarLivery: ON, %d added-team liver(y/ies)\n", g_n); LogLine(strbuf);
}

void CarLiverySOS(void)
{
  int i;
  RegisterPass();                                      /* map is wiped per weekend -> re-register */
  /* restore each working atlas to its pristine base so cartex snapshots the base, not a stale
     per-car livery left over from the previous session's last draw (4a.2b). */
  for (i = 0; i < g_n; i++) {
    if (g_base[i]    && g_img[i]) memcpy(g_img[i], g_base[i], CL_SZ);
    if (g_basePal[i] && g_pal[i]) memcpy(g_pal[i], g_basePal[i], 4 * g_palN[i]);
  }
}
