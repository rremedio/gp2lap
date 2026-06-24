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

const OvGeneral *OverrideGeneral(void)      { return &g_gen; }
const OvTeam    *OverrideTeam(int t)        { return (t >= 1 && t <= OV_TEAMS) ? &g_team[t-1] : 0; }
const OvCar     *OverrideCar(int c)         { return (c >= 1 && c <  OV_MAXCAR) ? &g_car[c]   : 0; }

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
  int section = 0;            /* 0 none, 1 [General], 2 [Team N] */
  int team = 0;
  int nGen=0, nLiv=0, nCk=0, nShape=0, nNose=0, nMass=0, nDf=0;

  memset(&g_gen, 0, sizeof(g_gen));
  memset(g_team, 0, sizeof(g_team));
  memset(g_car,  0, sizeof(g_car));

  if (!path || !path[0]) return -1;
  f = fopen(path, "rb");
  if (!f) { sprintf(strbuf, "- Override: '%s' not found; disabled\n", path); LogLine(strbuf); return -1; }

  while (fgets(line, sizeof(line), f)) {
    char *p = ltrim(line), *rb, *eq, *key, *ke, *val;

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
      } else { section = 0;
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
      else { sprintf(strbuf,"- Override: unknown key '%s' in [Team %d]; ignored\n", key, team); LogLine(strbuf); }
    }
    else {
      sprintf(strbuf,"- Override: key '%s' before any section; ignored\n", key); LogLine(strbuf);
    }
  }
  fclose(f);

  sprintf(strbuf, "- Override: '%s' loaded - General:%d liveries:%d cockpits:%d shapes:%d noses:%d mass:%d df:%d\n",
          path, nGen, nLiv, nCk, nShape, nNose, nMass, nDf);
  LogLine(strbuf);
  return 0;
}

#ifndef OVERRIDE_HOSTTEST
void OverrideLoad(void)
{
  const unsigned char *tab = (const unsigned char *)IDACodeReftoDataRef(0x65D67UL); /* &t_CaridTeamTab */
  char *cfg = GetCfgString("SeasonOverrides");
  OverrideParseFile(cfg, tab);
}
#endif
