#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "override.h"
#include "basiclog.h"        /* LogLine, strbuf (the host test provides definitions) */

#ifndef OVERRIDE_HOSTTEST
#include "miscahf.h"         /* IDACodeReftoDataRef */
#include "cfgmain.h"         /* GetCfgString */
#endif

static OvGeneral g_gen;
static OvTeam    g_team[OV_TEAMS];
static OvCar     g_car[OV_MAXCAR];
static OvTrack   g_track[OV_TRACKS];
static OvTeam    g_ttrack[OV_TEAMS];        /* per-track team model (filled by OverrideParseTrackFile) */
static char      g_ovdir[256];              /* dir of the override file (with trailing sep), or "" */
static unsigned char g_weekendMask = 0xF3;  /* [Weekend] session mask (from stock 0xF3) */
static int       g_weekendSet = 0;          /* 1 if a [Weekend] section was present */
static int       g_sprintOn = 0;            /* [Weekend] Sprint = 1 */
static int       g_sprintLaps = 0;          /* [Weekend] SprintLaps */
static int       g_calRounds = 0;           /* [Calendar] Rounds = N (0 = stock 16-round) */

const OvGeneral *OverrideGeneral(void)      { return &g_gen; }
const OvTeam    *OverrideTeam(int t)        { return (t >= 1 && t <= OV_TEAMS)  ? &g_team[t-1]   : 0; }
const OvCar     *OverrideCar(int c)         { return (c >= 1 && c <  OV_MAXCAR) ? &g_car[c]      : 0; }
const OvTrack   *OverrideTrack(int t)       { return (t >= 1 && t <= OV_TRACKS) ? &g_track[t-1]  : 0; }
const OvTeam    *OverrideTrackTeam(int t)   { return (t >= 1 && t <= OV_TEAMS)  ? &g_ttrack[t-1] : 0; }
const char      *OverrideBaseDir(void)      { return g_ovdir; }
int OverrideWeekend(unsigned char *maskOut) { if (maskOut) *maskOut = g_weekendMask; return g_weekendSet; }
int OverrideSprint(int *lapsOut)            { if (lapsOut) *lapsOut = g_sprintLaps; return g_sprintOn; }
int OverrideCalendarRounds(void) { return g_calRounds; }

/* ---------------- small text helpers ---------------- */

static char *ltrim(char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

/* strip trailing CR/LF/space/tab/quote in place (paths + tokens) */
static void rstrip(char *s)
{
  char *e = s + strlen(s);
  while (e > s && (e[-1]=='\r'||e[-1]=='\n'||e[-1]==' '||e[-1]=='\t'||e[-1]=='"')) *--e = 0;
}

/* case-insensitive equality of NUL-terminated tokens */
static int ieq(const char *a, const char *b)
{
  while (*a && *b) { if (((*a)|0x20) != ((*b)|0x20)) return 0; a++; b++; }
  return *a == 0 && *b == 0;
}

/* copy a value into dst[256], skipping a leading quote, stripping trailing junk */
static void copyval(char *dst, const char *v)
{
  while (*v == ' ' || *v == '\t' || *v == '"') v++;
  strncpy(dst, v, 255); dst[255] = 0;
  rstrip(dst);
}

/* parse "b0,b1,b2" (each 0..255, decimal or 0x..). Returns 1 on success. */
static int parsetriple(const char *v, unsigned char *out)
{
  char *q = (char *)v;
  long b; int i;
  for (i = 0; i < 3; i++) {
    while (*q == ' ' || *q == '\t' || *q == '"') q++;
    b = strtol(q, &q, 0);
    if (b < 0 || b > 255) return 0;
    out[i] = (unsigned char)b;
    while (*q == ' ' || *q == '\t') q++;
    if (i < 2) { if (*q != ',') return 0; q++; }
  }
  return 1;
}

/* parse a list of n values (each 0..255, decimal or 0x..). Returns 1 on success. */
static int parselist(const char *v, unsigned char *out, int n)
{
  char *q = (char *)v;
  long b; int i;
  for (i = 0; i < n; i++) {
    while (*q == ' ' || *q == '\t' || *q == '"') q++;
    b = strtol(q, &q, 0);
    if (b < 0 || b > 255) return 0;
    out[i] = (unsigned char)b;
    while (*q == ' ' || *q == '\t') q++;
    if (i < n-1) { if (*q != ',') return 0; q++; }
  }
  while (*q == ' ' || *q == '\t') q++;
  if (*q == ',') return 0;            /* reject too many values */
  return 1;
}

/* case-insensitive prefix test */
static int ci_startswith(const char *s, const char *pre)
{
  while (*pre) { if (((*s)|0x20) != ((*pre)|0x20)) return 0; s++; pre++; }
  return 1;
}

/* match "<base><1|2>" exactly; on hit set *slot to 0/1 and return 1 */
static int slotKey(const char *key, const char *base, int *slot)
{
  int n = (int)strlen(base);
  if (!ci_startswith(key, base)) return 0;
  if ((key[n]=='1' || key[n]=='2') && key[n+1]==0) { *slot = key[n]-'1'; return 1; }
  return 0;
}

/* copy a value into dst[24], skipping leading quote, stripping trailing junk */
static void copyval24(char *dst, const char *v)
{
  char tmp[256];
  copyval(tmp, v);
  strncpy(dst, tmp, 23);
  dst[23] = 0;
}

static void copyval13(char *dst, const char *v)   /* team/engine name: 12 chars + NUL */
{
  char tmp[256];
  copyval(tmp, v);
  strncpy(dst, tmp, 12);
  dst[12] = 0;
}

/* resolve a team's driver slot (0/1) to a carId via t_CaridTeamTab; 0 = empty/disabled */
static int slotCar(const unsigned char *tab, int team1, int slot)
{
  return tab ? (tab[(team1-1)*2 + slot] & 0x3F) : 0;
}

/* ---------------- the parser ---------------- */

int OverrideParseFile(const char *path, const unsigned char *tab)
{
  FILE *f;
  char line[300];
  int section = 0;            /* 0 none, 1 [General], 2 [Team N], 3 [Track N], 4 [Weekend], 5 [Calendar] */
  int team = 0, trk = 0;
  int nGen=0, nLiv=0, nCk=0, nShape=0, nNose=0, nMass=0, nDf=0;
  int nDrv=0, nTeam=0, nPit=0, nTrack=0, nWeekend=0, nCal=0;

  memset(&g_gen,  0, sizeof(g_gen));
  memset(g_team,  0, sizeof(g_team));
  memset(g_car,   0, sizeof(g_car));
  memset(g_track, 0, sizeof(g_track));
  g_weekendMask = 0xF3; g_weekendSet = 0;
  g_sprintOn = 0; g_sprintLaps = 0;
  g_calRounds = 0;

  if (!path || !path[0]) return -1;

  /* remember the override file's directory (with trailing separator) so per-track
     files referenced from it (e.g. MagicData=magic/spa.m2d) resolve relative to it */
  g_ovdir[0] = 0;
  { int i, cut = -1;
    for (i = 0; path[i] && i < (int)sizeof(g_ovdir)-1; i++)
      if (path[i]=='/' || path[i]=='\\') cut = i;
    if (cut >= 0) { memcpy(g_ovdir, path, cut+1); g_ovdir[cut+1] = 0; }
  }
  f = fopen(path, "rb");
  if (!f) { sprintf(strbuf, "- Override: '%s' not found; disabled\n", path); LogLine(strbuf); return -1; }

  while (fgets(line, sizeof(line), f)) {
    char *p = ltrim(line), *rb, *eq, *key, *ke, *val;
    int slot;

    if (*p==';' || *p=='#' || *p=='\r' || *p=='\n' || *p==0) continue;

    /* section header: [General] / [Team N] (anything after ] is a comment) */
    if (*p == '[') {
      rb = strchr(p, ']');
      if (!rb) continue;
      *rb = 0; p = ltrim(p + 1); rstrip(p);
      if (ieq(p, "general")) { section = 1; }
      else if ((p[0]|0x20)=='t' && (p[1]|0x20)=='e' && (p[2]|0x20)=='a' && (p[3]|0x20)=='m') {
        char *q = ltrim(p + 4);
        team = atoi(q);
        if (team >= 1 && team <= OV_TEAMS) { section = 2; }
        else { section = 0;
               sprintf(strbuf, "- Override: [%s] team out of range; skipped\n", p); LogLine(strbuf); }
      }
      else if ((p[0]|0x20)=='t' && (p[1]|0x20)=='r' && (p[2]|0x20)=='a' && (p[3]|0x20)=='c' && (p[4]|0x20)=='k') {
        char *q = ltrim(p + 5);
        trk = atoi(q);
        if (trk >= 1 && trk <= OV_TRACKS) { section = 3; }
        else { section = 0;
               sprintf(strbuf, "- Override: [%s] track out of range; skipped\n", p); LogLine(strbuf); }
      }
      else if (ieq(p, "weekend"))  { section = 4; g_weekendMask = 0xF3; g_weekendSet = 1; }
      else if (ieq(p, "calendar")) { section = 5; }
      else { section = 0;
               sprintf(strbuf, "- Override: unknown section [%s]; ignored\n", p); LogLine(strbuf); }
      continue;
    }

    eq = strchr(p, '=');
    if (!eq) continue;
    ke = eq; while (ke > p && (ke[-1]==' '||ke[-1]=='\t')) ke--; *ke = 0;   /* key trimmed */
    key = p;
    val = ltrim(eq + 1);

    if (section == 1) {                          /* ---- [General] ---- */
      if      (ieq(key,"pitrefuelspeed"))    { g_gen.pitRefuelSpeed   = strtol(val,0,0); g_gen.set|=OVG_REFUELSPEED;  nGen++; }
      else if (ieq(key,"pitrefuelbasems"))   { g_gen.pitRefuelBaseMs  = strtol(val,0,0); g_gen.set|=OVG_REFUELBASE;   nGen++; }
      else if (ieq(key,"pitrefuelcapms"))    { g_gen.pitRefuelCapMs   = strtol(val,0,0); g_gen.set|=OVG_REFUELCAP;    nGen++; }
      else if (ieq(key,"pittyrechangems"))   { g_gen.pitTyreChangeMs  = strtol(val,0,0); g_gen.set|=OVG_TYRECHANGE;   nGen++; }
      else if (ieq(key,"pitdamagerepairms")) { g_gen.pitDamageRepairMs= strtol(val,0,0); g_gen.set|=OVG_DAMAGEREPAIR; nGen++; }
      else if (ieq(key,"disablerefuel"))     { g_gen.disableRefuel    = strtol(val,0,0); g_gen.set|=OVG_DISABLEREFUEL;nGen++; }
      else { sprintf(strbuf,"- Override: unknown key '%s' in [General]; ignored\n", key); LogLine(strbuf); }
    }
    else if (section == 2) {                      /* ---- [Team N] ---- */
      if (ieq(key,"car1") || ieq(key,"car2")) {
        int carId = slotCar(tab, team, key[3]-'1');
        if (carId) { copyval(g_car[carId].livery, val); g_car[carId].liverySet = 1; nLiv++; }
        else { sprintf(strbuf,"- Override: [Team %d] %s ignored (no driver in that slot)\n", team, key); LogLine(strbuf); }
      }
      else if (ieq(key,"cp1") || ieq(key,"cp2")) {
        int carId = slotCar(tab, team, key[2]-'1');
        unsigned char tr[3];
        if (!carId) { sprintf(strbuf,"- Override: [Team %d] %s ignored (no driver in that slot)\n", team, key); LogLine(strbuf); }
        else if (parsetriple(val, tr)) { g_car[carId].cp[0]=tr[0]; g_car[carId].cp[1]=tr[1]; g_car[carId].cp[2]=tr[2]; g_car[carId].cpSet=1; nCk++; }
        else { sprintf(strbuf,"- Override: [Team %d] %s bad colour triple; skipped\n", team, key); LogLine(strbuf); }
      }
      else if (ieq(key,"shape"))     { copyval(g_team[team-1].shape, val); g_team[team-1].shapeSet=1; nShape++; }
      else if (ieq(key,"nose"))      { g_team[team-1].nose = (atoi(val)!=0)?1:0; g_team[team-1].noseSet=1; nNose++; }
      else if (ieq(key,"mass"))      { g_team[team-1].mass = strtol(val,0,0);    g_team[team-1].massSet=1; nMass++; }
      else if (ieq(key,"downforce")) { g_team[team-1].downforce = strtol(val,0,0); g_team[team-1].dfSet=1; nDf++; }
      else if (ieq(key,"downforcerange")) { g_team[team-1].dfRange = strtol(val,0,0); g_team[team-1].dfRangeSet=1; nDf++; }
      else if (slotKey(key,"name",&slot))     { copyval24(g_team[team-1].drv[slot].name,val); g_team[team-1].drv[slot].nameSet=1; nDrv++; }
      else if (slotKey(key,"qual",&slot))     { g_team[team-1].drv[slot].qual=strtol(val,0,0); g_team[team-1].drv[slot].qualSet=1; nDrv++; }
      else if (slotKey(key,"race",&slot))     { g_team[team-1].drv[slot].race=strtol(val,0,0); g_team[team-1].drv[slot].raceSet=1; nDrv++; }
      else if (slotKey(key,"range",&slot))    { g_team[team-1].drv[slot].range=strtol(val,0,0); g_team[team-1].drv[slot].rangeSet=1; nDrv++; }
      else if (slotKey(key,"weight",&slot))   { g_team[team-1].drv[slot].weight=strtol(val,0,0); g_team[team-1].drv[slot].weightSet=1; nDrv++; }
      else if (slotKey(key,"num",&slot))      { g_team[team-1].drv[slot].num=atoi(val); g_team[team-1].drv[slot].numSet=1; nDrv++; }
      else if (slotKey(key,"selected",&slot)) { g_team[team-1].drv[slot].selected=(atoi(val)!=0); g_team[team-1].drv[slot].selectedSet=1; nDrv++; }
      else if (slotKey(key,"disabled",&slot)) { g_team[team-1].drv[slot].disabled=(atoi(val)!=0); g_team[team-1].drv[slot].disabledSet=1; nDrv++; }
      else if (ieq(key,"power"))       { g_team[team-1].power=strtol(val,0,0); g_team[team-1].powerSet=1; nTeam++; }
      else if (ieq(key,"qualpower"))   { g_team[team-1].qualpower=strtol(val,0,0); g_team[team-1].qualpowerSet=1; nTeam++; }
      else if (ieq(key,"reliability")) { g_team[team-1].reliability=strtol(val,0,0); g_team[team-1].reliabilitySet=1; nTeam++; }
      else if (ieq(key,"teamname"))    { copyval13(g_team[team-1].teamName, val);   g_team[team-1].teamNameSet=1;   nTeam++; }
      else if (ieq(key,"enginename"))  { copyval13(g_team[team-1].engineName, val); g_team[team-1].engineNameSet=1; nTeam++; }
      else if (ieq(key,"pitcrew"))     { if(parselist(val,g_team[team-1].pitcrew,14)) { g_team[team-1].pitcrewSet=1; nPit++; }
                                         else { sprintf(strbuf,"- Override: [Team %d] pitcrew needs 14 values; skipped\n",team); LogLine(strbuf); } }
      else { sprintf(strbuf,"- Override: unknown key '%s' in [Team %d]; ignored\n", key, team); LogLine(strbuf); }
    }
    else if (section == 3) {                      /* ---- [Track N] ---- */
      if (ieq(key,"magicdata")) {
        char tmp[256]; copyval(tmp, val);
        strncpy(g_track[trk-1].magicData, tmp, sizeof(g_track[trk-1].magicData)-1);
        g_track[trk-1].magicData[sizeof(g_track[trk-1].magicData)-1] = 0;
        g_track[trk-1].magicDataSet = 1; nTrack++;
      }
      else if (ieq(key,"override")) {
        char tmp[256]; copyval(tmp, val);
        strncpy(g_track[trk-1].overrideFile, tmp, sizeof(g_track[trk-1].overrideFile)-1);
        g_track[trk-1].overrideFile[sizeof(g_track[trk-1].overrideFile)-1] = 0;
        g_track[trk-1].overrideFileSet = 1; nTrack++;
      }
      else { sprintf(strbuf,"- Override: unknown key '%s' in [Track %d]; ignored\n", key, trk); LogLine(strbuf); }
    }
    else if (section == 4) {                      /* ---- [Weekend] ---- session enable bits */
      int bit = -1;
      if      (ieq(key,"fridaypractice"))     bit = 0;
      else if (ieq(key,"fridayqualifying"))   bit = 1;
      else if (ieq(key,"saturdaypractice"))   bit = 2;
      else if (ieq(key,"saturdayqualifying")) bit = 3;
      else if (ieq(key,"warmup"))             bit = 4;
      else if (ieq(key,"race"))               bit = 5;
      if (bit >= 0) {
        if (atoi(val) != 0) g_weekendMask |=  (unsigned char)(1 << bit);
        else                g_weekendMask &= (unsigned char)~(1 << bit);
        nWeekend++;
      }
      else if (ieq(key,"sprint"))     { g_sprintOn = (atoi(val) != 0); nWeekend++; }
      else if (ieq(key,"sprintlaps")) { g_sprintLaps = atoi(val); nWeekend++; }
      else { sprintf(strbuf,"- Override: unknown key '%s' in [Weekend]; ignored\n", key); LogLine(strbuf); }
    }
    else if (section == 5) {                      /* ---- [Calendar] ---- variable season length */
      if (ieq(key,"rounds")) {
        int r = atoi(val);
        if (r >= 1 && r <= OV_MAXROUNDS) { g_calRounds = r; nCal++; }
        else { g_calRounds = 0; LogLine("- Override: [Calendar] Rounds must be 1..16; ignored\n"); }
      }
      else { sprintf(strbuf,"- Override: unknown key '%s' in [Calendar]; ignored\n", key); LogLine(strbuf); }
    }
    else {
      sprintf(strbuf,"- Override: key '%s' before any section; ignored\n", key); LogLine(strbuf);
    }
  }
  fclose(f);

  sprintf(strbuf, "- Override: '%s' loaded - General:%d liveries:%d cockpits:%d shapes:%d noses:%d mass:%d df:%d driver:%d teamperf:%d pitcrew:%d track:%d weekend:%d calendar:%d\n",
          path, nGen, nLiv, nCk, nShape, nNose, nMass, nDf, nDrv, nTeam, nPit, nTrack, nWeekend, nCal);
  LogLine(strbuf);
  return 0;
}

/* Parse a per-track override file into the SEPARATE per-track team model (g_ttrack).
   Only [Team N] physics keys are honoured (mass/downforce/power/qualpower/reliability);
   any other key is ignored with a warning. Returns the count of physics values set,
   or -1 if the file is missing/unset. */
int OverrideParseTrackFile(const char *path)
{
  FILE *f;
  char line[300];
  int section = 0, team = 0, n = 0;

  memset(g_ttrack, 0, sizeof(g_ttrack));
  if (!path || !path[0]) return -1;
  f = fopen(path, "rb");
  if (!f) { sprintf(strbuf, "- Override: per-track '%s' not found; skipped\n", path); LogLine(strbuf); return -1; }

  while (fgets(line, sizeof(line), f)) {
    char *p = ltrim(line), *rb, *eq, *key, *ke, *val;
    if (*p==';' || *p=='#' || *p=='\r' || *p=='\n' || *p==0) continue;

    if (*p == '[') {                              /* only [Team N] matters in a per-track file */
      rb = strchr(p, ']'); if (!rb) continue;
      *rb = 0; p = ltrim(p + 1); rstrip(p);
      if ((p[0]|0x20)=='t' && (p[1]|0x20)=='e' && (p[2]|0x20)=='a' && (p[3]|0x20)=='m') {
        team = atoi(ltrim(p + 4));
        section = (team >= 1 && team <= OV_TEAMS) ? 2 : 0;
      } else section = 0;                          /* [General]/[Track N]/... ignored per-track */
      continue;
    }

    eq = strchr(p, '='); if (!eq) continue;
    ke = eq; while (ke > p && (ke[-1]==' '||ke[-1]=='\t')) ke--; *ke = 0;
    key = p; val = ltrim(eq + 1);

    if (section == 2) {
      OvTeam *t = &g_ttrack[team-1];
      if      (ieq(key,"mass"))          { t->mass=strtol(val,0,0);        t->massSet=1;        n++; }
      else if (ieq(key,"downforce"))     { t->downforce=strtol(val,0,0);   t->dfSet=1;          n++; }
      else if (ieq(key,"downforcerange")){ t->dfRange=strtol(val,0,0);     t->dfRangeSet=1;     n++; }
      else if (ieq(key,"power"))         { t->power=strtol(val,0,0);       t->powerSet=1;       n++; }
      else if (ieq(key,"qualpower"))   { t->qualpower=strtol(val,0,0);   t->qualpowerSet=1;   n++; }
      else if (ieq(key,"reliability")) { t->reliability=strtol(val,0,0); t->reliabilitySet=1; n++; }
      else { sprintf(strbuf, "- Override: per-track [Team %d] key '%s' not honoured per-track; ignored\n", team, key); LogLine(strbuf); }
    }
  }
  fclose(f);
  return n;
}

#ifndef OVERRIDE_HOSTTEST
void OverrideLoad(void)
{
  const unsigned char *tab = (const unsigned char *)IDACodeReftoDataRef(0x65D67UL); /* &t_CaridTeamTab */
  char *cfg = GetCfgString("SeasonOverrides");
  OverrideParseFile(cfg, tab);
}
#endif
