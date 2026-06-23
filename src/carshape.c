#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "carshape.h"
#include "miscahf.h"        // IDAtoFlat, IDACodeReftoDataRef
#include "cfgmain.h"        // GetCfgString
#include "basiclog.h"       // LogLine / strbuf
#include "override.h"       // shared SeasonOverrides model ([Team N] shape/nose)

/* Per-team car-shape overrides, read from override.cfg (SeasonOverrides). Two tiers:
   Tier 1 (noseNN): the per-team low/high nose, picked from the static 14-dword table dword_CB394
     that sub_677D0 reads into dword_CB358 every car draw (CB358 drives the nose-pool morph). CB394
     is never written at runtime, so we write it once at init.
   Tier 2 (shapeNN): a full per-team car .dat (a byte-for-byte copy of the 54536-byte car object).
     The engine draws internal object 0 (t_interobjs[0]) for every car; sub_677D0 runs just before
     that read with ESI = car. We hook that call, and per draw memcpy the team's geometry over the
     live object IN PLACE -- keeping the live object's already-correct runtime header pointers
     (+0x04..+0x24) and the engine's LOD field (+0x38, dword_E928C). Because every .dat shares the
     object's byte layout, the sections land at the same offsets and the kept pointers stay valid,
     so NO pointer rebasing is needed (the .dat's own pointers are link-time and would be garbage at
     runtime). Same-topology .dats only (shared face/edge/point order). See docs/per-team-car-shapes.md. */

#define CS_TEAMS 14
#define CS_OBJSZ 54536          /* size of the car object block (o_interobj00) */

unsigned long PerTeamNose  = 0;
unsigned long PerTeamShape = 0;

/* tier 2 state. We swap ONLY the points section (vertex {x,y,z,id} data) -- the part that defines
   the shape. Faces, edges, the scale ramp, every section pointer and all structure stay exactly as
   the live stock object, so nothing pointer-bearing is ever touched. */
unsigned char        *CarShapeCarPtr = 0;        /* car ptr (ESI), stashed by the asm hook each draw */
static unsigned char *s_dat[CS_TEAMS];           /* per-team override object bytes (NULL = none) */
static unsigned char **s_pInterobjs = 0;         /* &t_interobjs[0] */
static unsigned char *s_objBuf  = 0;             /* the live car object the engine draws */
static unsigned char *s_pristine = 0;            /* pristine stock object (for restore) */
static unsigned long  s_ptsOff = 0;              /* points section offset within the object */
static unsigned long  s_ptsSz  = 0;              /* points section size (bytes) */
static unsigned long  s_sclOff = 0;              /* scale-ramp section offset within the object */
static unsigned long  s_sclSz  = 0;              /* scale-ramp section size (bytes) */
static int            s_curTeam = -1;            /* team whose geometry is currently in s_objBuf */

/* ---------------- tier 1: per-team nose ---------------- */

static void CarShapeInitNose(const char *cfg)
{
  unsigned char nose[CS_TEAMS], set[CS_TEAMS], *site;
  unsigned long *pCB394;
  int i, loaded;

  for (i = 0; i < CS_TEAMS; i++) { nose[i] = 0; set[i] = 0; }
  loaded = 0;
  for (i = 0; i < CS_TEAMS; i++) {
    const OvTeam *t = OverrideTeam(i + 1);
    if (t->noseSet) { nose[i] = (unsigned char)t->nose; set[i] = 1; loaded++; }
  }
  if (loaded < 1) return;

  /* dword_CB394 read in sub_677D0:  00067818  8B 04 95 <disp32>  mov eax, dword_CB394[edx*4]
     The disp32 is the runtime flat address (relocated at load), so only check the opcode/SIB and
     the stable low byte 0x94 that identifies THIS read; take the real address from the operand. */
  site = (unsigned char *)IDAtoFlat(0x67818);
  if (site[0]!=0x8B || site[1]!=0x04 || site[2]!=0x95 || site[3]!=0x94) {
    sprintf(strbuf, "- PerTeamNose: opcode mismatch at 0x67818 (%02X %02X %02X %02X); DISABLED\n",
            site[0], site[1], site[2], site[3]);
    LogLine(strbuf);
    return;
  }
  pCB394 = (unsigned long *)IDACodeReftoDataRef(0x6781B);   /* &dword_CB394[0] (runtime flat addr) */

  for (i = 0; i < CS_TEAMS; i++)
    if (set[i]) pCB394[i] = nose[i];

  PerTeamNose = 1;
  sprintf(strbuf, "- PerTeamNose: ON (%d team nose override(s))\n", loaded);
  LogLine(strbuf);
}

/* ---------------- tier 2: per-team .dat shape ---------------- */

static unsigned char *CarShapeLoadDat(const char *path)
{
  FILE *f;
  unsigned char *buf;
  long n;

  f = fopen(path, "rb");
  if (!f) { sprintf(strbuf, "- PerTeamShape: open FAILED '%s'\n", path); LogLine(strbuf); return 0; }
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n != CS_OBJSZ) {
    sprintf(strbuf, "- PerTeamShape: '%s' is %ld bytes (need %d); skipped\n", path, n, CS_OBJSZ);
    LogLine(strbuf); fclose(f); return 0;
  }
  buf = (unsigned char *)malloc(CS_OBJSZ);
  if (!buf) { fclose(f); LogLine("- PerTeamShape: malloc FAILED\n"); return 0; }
  if (fread(buf, 1, CS_OBJSZ, f) != CS_OBJSZ) {
    free(buf); fclose(f); LogLine("- PerTeamShape: read FAILED\n"); return 0;
  }
  fclose(f);
  return buf;
}

static int CarShapeLoadDats(const char *cfgpath)
{
  int team, loaded = 0;
  for (team = 1; team <= CS_TEAMS; team++) {
    const OvTeam *t = OverrideTeam(team);
    if (!t->shapeSet || !t->shape[0]) continue;
    if (s_dat[team-1]) { free(s_dat[team-1]); s_dat[team-1] = 0; }
    s_dat[team-1] = CarShapeLoadDat(t->shape);
    if (s_dat[team-1]) { loaded++;
      sprintf(strbuf, "- PerTeamShape: team%02d <- %s\n", team, t->shape); LogLine(strbuf); }
  }
  return loaded;
}

void __near _cdecl AHFCarShapeSwap(void)
{
  unsigned char *car;
  int team;

  if (!PerTeamShape) return;
  car = CarShapeCarPtr;
  if (!car) return;

  team = (int)(car[0x25] & 0xFF) - 1;          /* teamNr 1.. -> 0-based (engine clamps >=0) */
  if (team < 0) team = 0;
  if (team >= CS_TEAMS) return;

  if (s_dat[team]) {                            /* this team has an override shape */
    if (s_curTeam != team) {
      memcpy(s_objBuf + s_ptsOff, s_dat[team] + s_ptsOff, s_ptsSz);   /* team vertices */
      memcpy(s_objBuf + s_sclOff, s_dat[team] + s_sclOff, s_sclSz);   /* team scale ramp */
      s_curTeam = team;
    }
  } else {                                      /* no override -> restore stock geometry */
    if (s_curTeam != -1) {
      memcpy(s_objBuf + s_ptsOff, s_pristine + s_ptsOff, s_ptsSz);
      memcpy(s_objBuf + s_sclOff, s_pristine + s_sclOff, s_sclSz);
      s_curTeam = -1;
    }
  }
  /* original sub_677D0 (chained from the asm stub) sets the team nose/colour globals */
}

void (__near _cdecl *fpCarShapeCode)(void) = AHFCarShapeSwap;

static void CarShapeFreeDats(void)
{
  int i;
  for (i = 0; i < CS_TEAMS; i++) if (s_dat[i]) { free(s_dat[i]); s_dat[i] = 0; }
}

static void CarShapeInitDat(const char *cfg)
{
  unsigned char *s;
  int i, loaded;

  for (i = 0; i < CS_TEAMS; i++) s_dat[i] = 0;
  loaded = CarShapeLoadDats(cfg);
  if (loaded < 1) return;

  /* bootstrap t_interobjs[0] = the live car object:
       00065A05  BF <offset t_interobjs>   mov edi, offset t_interobjs */
  s = (unsigned char *)IDAtoFlat(0x65A05);
  if (s[0] != 0xBF) {
    LogLine("- PerTeamShape: opcode mismatch at 0x65A05; DISABLED\n");
    CarShapeFreeDats(); return;
  }
  s_pInterobjs = (unsigned char **)IDACodeReftoDataRef(0x65A06);   /* &t_interobjs[0] */
  s_objBuf = s_pInterobjs[0];
  if (!s_objBuf) {
    LogLine("- PerTeamShape: t_interobjs[0] is NULL; DISABLED\n");
    CarShapeFreeDats(); return;
  }
  s_pristine = (unsigned char *)malloc(CS_OBJSZ);
  if (!s_pristine) {
    LogLine("- PerTeamShape: pristine malloc FAILED; DISABLED\n");
    CarShapeFreeDats(); return;
  }
  memcpy(s_pristine, s_objBuf, CS_OBJSZ);       /* keep the stock object (for restore + offsets) */
  s_curTeam = -1;                               /* buffer currently holds stock */

  /* Locate the points section from the live object's own runtime pointers:
       header +0x10 = pointsBegin, +0x14 = edgesBegin (both runtime flat addresses).
     Offset within the object = pointer - object base (= s_objBuf). This also validates the
     flat-pointer model: a sane offset confirms the header pointers are object-relative. */
  s_ptsOff = *(unsigned long *)(s_pristine + 0x10) - (unsigned long)s_objBuf;
  s_ptsSz  = *(unsigned long *)(s_pristine + 0x14) - *(unsigned long *)(s_pristine + 0x10);
  /* The scale ramp (+0x04 scaleBegin .. +0x08 scaleEnd) is the morph's other half -- the engine
     offsets scaleBegin by stride*CB358 exactly like pointsBegin, so a custom shape needs its own
     scale data too. It is pure 16-bit data (no pointers), so it is safe to swap the same way. */
  s_sclOff = *(unsigned long *)(s_pristine + 0x04) - (unsigned long)s_objBuf;
  s_sclSz  = *(unsigned long *)(s_pristine + 0x08) - *(unsigned long *)(s_pristine + 0x04);
  if (s_ptsOff >= CS_OBJSZ || s_ptsSz == 0 || s_ptsOff + s_ptsSz > CS_OBJSZ ||
      s_sclOff >= CS_OBJSZ || s_sclSz == 0 || s_sclOff + s_sclSz > CS_OBJSZ) {
    sprintf(strbuf, "- PerTeamShape: section out of range (pts 0x%lX/0x%lX scl 0x%lX/0x%lX); DISABLED\n",
            s_ptsOff, s_ptsSz, s_sclOff, s_sclSz);
    LogLine(strbuf);
    free(s_pristine); s_pristine = 0;
    CarShapeFreeDats();
    return;
  }

  PerTeamShape = 1;
  sprintf(strbuf, "- PerTeamShape: ON (%d shape(s), pts off=0x%lX sz=0x%lX, scl off=0x%lX sz=0x%lX)\n",
          loaded, s_ptsOff, s_ptsSz, s_sclOff, s_sclSz);
  LogLine(strbuf);
}

void CarShapeInit(void)
{
  char *cfg;

  cfg = GetCfgString("SeasonOverrides");
  if (!cfg || !cfg[0]) return;                  /* override file absent -> features off */
  CarShapeInitNose(cfg);                        /* tier 1 */
  CarShapeInitDat(cfg);                         /* tier 2 */
}
